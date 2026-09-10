/*
 * gstmqaenc -- an MQA encoder element for GStreamer.
 *
 *     ... ! audioconvert ! mqaenc ! wavenc ! filesink location=out.wav
 *
 * The element takes stereo PCM at 88.2 or 96 kHz and emits half as many
 * frames per second (S32LE, 24 bits), carrying an MQA stream that
 * unfolds back to the input rate: put `mqadec` after it and the
 * original comes back.
 *
 * It cannot authenticate: see encoder/README.md. A stream from here
 * says what it is and decodes, and no MQA decoder will light up for it.
 *
 * Properties are the stream's own parameters. The defaults are what a
 * 24-bit carrier wants and what real streams use; the ones worth
 * touching are `scale` (the residual quantiser, and so the bitrate) and
 * `bit` (which of the carrier's bits carries the control channel).
 *
 * SPDX-License-Identifier: MIT
 */
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <string.h>
#include "mqae/encode.h"

GST_DEBUG_CATEGORY_STATIC(mqaenc_debug);
#define GST_CAT_DEFAULT mqaenc_debug

#define GST_TYPE_MQAENC (gst_mqaenc_get_type())
G_DECLARE_FINAL_TYPE(GstMqaenc, gst_mqaenc, GST, MQAENC, GstElement)

/* Frames pushed in one go, and taken out in one go. */
#define CHUNK (4 * MQAE_ENCODE_BLOCK)

struct _GstMqaenc {
	GstElement element;
	GstPad *sinkpad, *srcpad;

	struct mqae_encode enc;
	gboolean started;
	gboolean negotiated;
	gint in_rate;
	gint in_shift;
	gint in_bpf;
	guint64 out_frames;
	GstClockTime segment_start;
	gint32 *scratch;               /* input, left-justified            */
	gsize scratch_frames;
	gint32 *out;                   /* carrier frames                   */

	/* the stream's parameters */
	guint scale, level, xbit, orig_rate, auth_level;
	guint variant, salt, filter, depth, gain_index;
};

G_DEFINE_TYPE(GstMqaenc, gst_mqaenc, GST_TYPE_ELEMENT)

enum { PROP_0, PROP_SCALE, PROP_LEVEL, PROP_BIT, PROP_ORIG_RATE, PROP_AUTH,
       PROP_VARIANT, PROP_SALT, PROP_FILTER, PROP_DEPTH, PROP_GAIN };

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
	"sink", GST_PAD_SINK, GST_PAD_ALWAYS,
	GST_STATIC_CAPS("audio/x-raw, "
			"format = (string) { S16LE, S24_32LE, S32LE }, "
			"layout = (string) interleaved, "
			"rate = (int) { 88200, 96000 }, "
			"channels = (int) 2"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
	"src", GST_PAD_SRC, GST_PAD_ALWAYS,
	GST_STATIC_CAPS("audio/x-raw, "
			"format = (string) S32LE, "
			"layout = (string) interleaved, "
			"rate = (int) { 44100, 48000 }, "
			"channels = (int) 2"));

static void gst_mqaenc_stop(GstMqaenc *self)
{
	if (self->started)
		mqae_encode_close(&self->enc);
	self->started = FALSE;
	self->negotiated = FALSE;
	self->out_frames = 0;
	g_free(self->scratch);
	g_free(self->out);
	self->scratch = NULL;
	self->out = NULL;
	self->scratch_frames = 0;
}

/*
 * How long the stream will be, in carrier frames, if anything upstream
 * knows. A stream that says where it ends lets a decoder unfold its
 * last frames instead of passing them through, so the element asks.
 */
static guint64 gst_mqaenc_total(GstMqaenc *self)
{
	gint64 duration = 0;

	if (!gst_pad_peer_query_duration(self->sinkpad, GST_FORMAT_TIME, &duration) ||
	    duration <= 0)
		return 0;
	return gst_util_uint64_scale((guint64)duration, (guint64)self->in_rate / 2,
				     GST_SECOND);
}

static gboolean gst_mqaenc_start(GstMqaenc *self)
{
	struct mqae_config cfg;

	if (self->started)
		mqae_encode_close(&self->enc);
	mqae_config_default(&cfg);
	cfg.src_rate = (unsigned)self->in_rate / 2;
	cfg.orig_rate = self->orig_rate ? self->orig_rate : (unsigned)self->in_rate;
	cfg.xbit = self->xbit;
	cfg.scale_index = self->scale;
	cfg.level = self->level;
	cfg.variant = self->variant;
	cfg.salt_select = self->salt;
	cfg.render_filter = self->filter;
	cfg.render_bitdepth = self->depth;
	cfg.gain_index = self->gain_index;
	cfg.auth.level = self->auth_level;
	if (mqae_encode_open(&self->enc, &cfg, gst_mqaenc_total(self)) < 0)
		return FALSE;
	self->out = g_malloc_n(CHUNK * 2, sizeof *self->out);
	self->started = TRUE;
	self->negotiated = FALSE;
	self->out_frames = 0;
	GST_INFO_OBJECT(self, "encoding %d Hz to a %u Hz carrier, claiming %u Hz;"
			" %llu carrier frames expected",
			self->in_rate, cfg.src_rate, cfg.orig_rate,
			(unsigned long long)self->enc.out.total);
	return TRUE;
}

static gboolean gst_mqaenc_negotiate(GstMqaenc *self)
{
	GstCaps *caps = gst_caps_new_simple("audio/x-raw",
					    "format", G_TYPE_STRING, "S32LE",
					    "layout", G_TYPE_STRING, "interleaved",
					    "rate", G_TYPE_INT, self->in_rate / 2,
					    "channels", G_TYPE_INT, 2, NULL);
	gboolean ok = gst_pad_set_caps(self->srcpad, caps);

	gst_caps_unref(caps);
	self->negotiated = ok;
	return ok;
}

/* Give the encoder `frames` and push whatever carrier it produced. */
static GstFlowReturn gst_mqaenc_feed(GstMqaenc *self, const gint32 *in, gsize frames, int end)
{
	gsize done = 0;

	if (!self->negotiated && !gst_mqaenc_negotiate(self))
		return GST_FLOW_NOT_NEGOTIATED;
	for (;;) {
		gsize take = frames - done < CHUNK ? frames - done : CHUNK;
		size_t produced = 0;
		GstBuffer *buf;
		GstMapInfo map;
		int last = end && done + take == frames;

		if (mqae_encode_push(&self->enc, in + 2 * done, take, last,
				     self->out, CHUNK, &produced) < 0) {
			GST_ELEMENT_ERROR(self, STREAM, ENCODE, (NULL), ("encoding failed"));
			return GST_FLOW_ERROR;
		}
		done += take;
		if (produced) {
			buf = gst_buffer_new_allocate(NULL, produced * 8, NULL);
			if (!buf || !gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
				if (buf)
					gst_buffer_unref(buf);
				return GST_FLOW_ERROR;
			}
			memcpy(map.data, self->out, produced * 8);
			gst_buffer_unmap(buf, &map);
			GST_BUFFER_PTS(buf) = self->segment_start +
				gst_util_uint64_scale(self->out_frames, GST_SECOND,
						      (guint64)self->in_rate / 2);
			GST_BUFFER_DTS(buf) = GST_BUFFER_PTS(buf);
			GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(produced, GST_SECOND,
									 (guint64)self->in_rate / 2);
			GST_BUFFER_OFFSET(buf) = self->out_frames;
			GST_BUFFER_OFFSET_END(buf) = self->out_frames + produced;
			self->out_frames += produced;
			{
				GstFlowReturn ret = gst_pad_push(self->srcpad, buf);

				if (ret != GST_FLOW_OK)
					return ret;
			}
		}
		if (done == frames && !(last && produced))
			return GST_FLOW_OK;
		if (done == frames && !produced)
			return GST_FLOW_OK;
	}
}

static GstFlowReturn gst_mqaenc_chain(GstPad *pad, GstObject *parent, GstBuffer *buf)
{
	GstMqaenc *self = GST_MQAENC(parent);
	GstMapInfo map;
	gsize frames, i;
	GstFlowReturn ret;

	(void)pad;
	if (!self->started) {
		gst_buffer_unref(buf);
		GST_ELEMENT_ERROR(self, CORE, NEGOTIATION, (NULL), ("no input format"));
		return GST_FLOW_NOT_NEGOTIATED;
	}
	if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
		gst_buffer_unref(buf);
		return GST_FLOW_ERROR;
	}
	frames = map.size / (gsize)self->in_bpf;
	if (frames > self->scratch_frames) {
		g_free(self->scratch);
		self->scratch = g_malloc_n(frames * 2, sizeof *self->scratch);
		self->scratch_frames = frames;
	}
	/* the library works in 24-bit values at the top of a 32-bit word */
	if (self->in_shift == 0) {
		memcpy(self->scratch, map.data, frames * 8);
	} else if (self->in_bpf == 4) {
		const gint16 *p = (const gint16 *)(gconstpointer)map.data;

		for (i = 0; i < frames * 2; i++)
			self->scratch[i] = (gint32)p[i] << 16;
	} else {
		const gint32 *p = (const gint32 *)(gconstpointer)map.data;

		for (i = 0; i < frames * 2; i++)
			self->scratch[i] = p[i] << self->in_shift;
	}
	gst_buffer_unmap(buf, &map);
	gst_buffer_unref(buf);

	ret = gst_mqaenc_feed(self, self->scratch, frames, 0);
	return ret;
}

static gboolean gst_mqaenc_sink_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
	GstMqaenc *self = GST_MQAENC(parent);

	switch (GST_EVENT_TYPE(event)) {
	case GST_EVENT_CAPS: {
		GstCaps *caps;
		GstAudioInfo info;

		gst_event_parse_caps(event, &caps);
		if (!gst_audio_info_from_caps(&info, caps)) {
			gst_event_unref(event);
			return FALSE;
		}
		self->in_rate = GST_AUDIO_INFO_RATE(&info);
		self->in_bpf = GST_AUDIO_INFO_BPF(&info);
		switch (GST_AUDIO_INFO_FORMAT(&info)) {
		case GST_AUDIO_FORMAT_S16LE:    self->in_shift = 16; break;
		case GST_AUDIO_FORMAT_S24_32LE: self->in_shift = 8; break;
		default:                        self->in_shift = 0; break;
		}
		gst_event_unref(event);
		return gst_mqaenc_start(self);
	}
	case GST_EVENT_SEGMENT: {
		const GstSegment *seg;

		gst_event_parse_segment(event, &seg);
		self->segment_start = seg->format == GST_FORMAT_TIME ? seg->start : 0;
		self->out_frames = 0;
		if (!self->negotiated && !gst_mqaenc_negotiate(self)) {
			gst_event_unref(event);
			return FALSE;
		}
		return gst_pad_push_event(self->srcpad, event);
	}
	case GST_EVENT_EOS:
		if (self->started)
			gst_mqaenc_feed(self, NULL, 0, 1);
		return gst_pad_push_event(self->srcpad, event);
	case GST_EVENT_FLUSH_STOP:
		if (self->started)
			gst_mqaenc_start(self);
		return gst_pad_push_event(self->srcpad, event);
	default:
		return gst_pad_event_default(pad, parent, event);
	}
}

/* --- boilerplate ---------------------------------------------------------- */

static void gst_mqaenc_set_property(GObject *object, guint id, const GValue *value,
				    GParamSpec *spec)
{
	GstMqaenc *self = GST_MQAENC(object);

	switch (id) {
	case PROP_SCALE:     self->scale = g_value_get_uint(value); break;
	case PROP_LEVEL:     self->level = g_value_get_uint(value); break;
	case PROP_BIT:       self->xbit = g_value_get_uint(value); break;
	case PROP_ORIG_RATE: self->orig_rate = g_value_get_uint(value); break;
	case PROP_AUTH:      self->auth_level = g_value_get_uint(value); break;
	case PROP_VARIANT:   self->variant = g_value_get_uint(value); break;
	case PROP_SALT:      self->salt = g_value_get_uint(value); break;
	case PROP_FILTER:    self->filter = g_value_get_uint(value); break;
	case PROP_DEPTH:     self->depth = g_value_get_uint(value); break;
	case PROP_GAIN:      self->gain_index = g_value_get_uint(value); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec); break;
	}
}

static void gst_mqaenc_get_property(GObject *object, guint id, GValue *value,
				    GParamSpec *spec)
{
	GstMqaenc *self = GST_MQAENC(object);

	switch (id) {
	case PROP_SCALE:     g_value_set_uint(value, self->scale); break;
	case PROP_LEVEL:     g_value_set_uint(value, self->level); break;
	case PROP_BIT:       g_value_set_uint(value, self->xbit); break;
	case PROP_ORIG_RATE: g_value_set_uint(value, self->orig_rate); break;
	case PROP_AUTH:      g_value_set_uint(value, self->auth_level); break;
	case PROP_VARIANT:   g_value_set_uint(value, self->variant); break;
	case PROP_SALT:      g_value_set_uint(value, self->salt); break;
	case PROP_FILTER:    g_value_set_uint(value, self->filter); break;
	case PROP_DEPTH:     g_value_set_uint(value, self->depth); break;
	case PROP_GAIN:      g_value_set_uint(value, self->gain_index); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec); break;
	}
}

static GstStateChangeReturn gst_mqaenc_change_state(GstElement *element,
						    GstStateChange transition)
{
	GstMqaenc *self = GST_MQAENC(element);
	GstStateChangeReturn ret;

	ret = GST_ELEMENT_CLASS(gst_mqaenc_parent_class)->change_state(element, transition);
	if (transition == GST_STATE_CHANGE_PAUSED_TO_READY)
		gst_mqaenc_stop(self);
	return ret;
}

static void gst_mqaenc_finalize(GObject *object)
{
	gst_mqaenc_stop(GST_MQAENC(object));
	G_OBJECT_CLASS(gst_mqaenc_parent_class)->finalize(object);
}

static void gst_mqaenc_class_init(GstMqaencClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	GstElementClass *element = GST_ELEMENT_CLASS(klass);

	object->set_property = gst_mqaenc_set_property;
	object->get_property = gst_mqaenc_get_property;
	object->finalize = gst_mqaenc_finalize;
	element->change_state = gst_mqaenc_change_state;

#define UINT_PROP(id, name, blurb, lo, hi, def) \
	g_object_class_install_property(object, id, \
		g_param_spec_uint(name, name, blurb, lo, hi, def, \
				  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS))
	UINT_PROP(PROP_SCALE, "scale", "Residual scale index: the quantiser, and so the bitrate",
		  0, 63, 25);
	UINT_PROP(PROP_LEVEL, "level", "Refinement level", 0, 127, 15);
	UINT_PROP(PROP_BIT, "bit", "Which bit above bit 8 carries the control channel",
		  0, 7, 0);
	UINT_PROP(PROP_ORIG_RATE, "orig-rate", "The rate the stream claims to encode, 0 for the input's",
		  0, 768000, 0);
	UINT_PROP(PROP_AUTH, "auth-level", "The provenance the stream claims (it cannot prove it)",
		  0, 15, 0);
	UINT_PROP(PROP_VARIANT, "variant", "Reconstruction kernel: 1 the short filter, 0 the long one",
		  0, 1, 1);
	UINT_PROP(PROP_SALT, "salt", "Dither salt: 0 silence, 1 or 2", 0, 2, 1);
	UINT_PROP(PROP_FILTER, "render-filter", "What a renderer downstream should apply", 0, 31, 8);
	UINT_PROP(PROP_DEPTH, "render-depth", "Render bit depth", 0, 3, 2);
	/*
	 * A stream cannot be measured before it is encoded, so the element
	 * cannot take the headroom the command-line tool takes by looking
	 * at the file first. It defaults to a little instead: a full-scale
	 * source makes a full-scale carrier, whatever is above the source's
	 * own Nyquist pushes it over, and one clipped carrier sample is not
	 * a small error, because the filter's history carries it forward. The
	 * decoder puts the gain back exactly, so the only cost is that much
	 * less of the carrier's range.
	 */
	UINT_PROP(PROP_GAIN, "gain",
		  "Headroom: the decoder puts this back. 0 is none, 15 is 2.8 dB",
		  0, 15, 5);
#undef UINT_PROP

	gst_element_class_add_static_pad_template(element, &sink_template);
	gst_element_class_add_static_pad_template(element, &src_template);
	gst_element_class_set_static_metadata(element,
		"MQA encoder", "Codec/Encoder/Audio",
		"Encode 88.2/96 kHz stereo into an MQA carrier at half the rate "
		"(unauthenticated)",
		"mqa-decode");
}

static void gst_mqaenc_init(GstMqaenc *self)
{
	self->sinkpad = gst_pad_new_from_static_template(&sink_template, "sink");
	gst_pad_set_chain_function(self->sinkpad, gst_mqaenc_chain);
	gst_pad_set_event_function(self->sinkpad, gst_mqaenc_sink_event);
	GST_PAD_SET_PROXY_ALLOCATION(self->sinkpad);
	gst_element_add_pad(GST_ELEMENT(self), self->sinkpad);

	self->srcpad = gst_pad_new_from_static_template(&src_template, "src");
	gst_pad_use_fixed_caps(self->srcpad);
	gst_element_add_pad(GST_ELEMENT(self), self->srcpad);

	self->scale = 25;
	self->level = 15;
	self->variant = 1;
	self->salt = 1;
	self->filter = 8;
	self->depth = 2;
	self->gain_index = 5;
}

static gboolean plugin_init(GstPlugin *plugin)
{
	GST_DEBUG_CATEGORY_INIT(mqaenc_debug, "mqaenc", 0, "MQA encoder");
	return gst_element_register(plugin, "mqaenc", GST_RANK_NONE, GST_TYPE_MQAENC);
}

#ifndef VERSION
#define VERSION "0.1"
#endif
#ifndef PACKAGE
#define PACKAGE "mqa-decode"
#endif

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, mqaenc,
		  "MQA encoding (unauthenticated)", plugin_init, VERSION, "MIT/X11",
		  "mqa-decode", "https://github.com/audioburro/mqa")
