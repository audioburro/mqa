/*
 * gstmqadec -- an MQA first-unfold element for GStreamer.
 *
 *     ... ! flacparse ! flacdec ! mqadec ! audioconvert ! autoaudiosink
 *
 * The element takes stereo PCM at 44.1 or 48 kHz and, when the carrier
 * turns out to hold an MQA stream, emits twice as many frames per second
 * (S32LE, 24 bits of resolution). When it does not, the frames pass
 * through unchanged at their own rate.
 *
 * Nothing in a file says whether it is MQA: the stream is recognised
 * only when its magic appears in the carrier's low bits, which can be a
 * second or two in. The element therefore holds its output and does not
 * negotiate downstream until it knows -- see integration/common's
 * adapter, which is where that decision and the two-for-one bookkeeping
 * live.
 *
 * Properties:
 *   signalling   embed the renderer signalling in the output (default off)
 *   passthrough  what to do with frames the decoder passes through:
 *                hold (repeat them, keeping time), drop, or raw
 *   sniff-time   how long to look for a stream before giving up, in ms
 *
 * SPDX-License-Identifier: MIT
 */
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <string.h>
#include "mqa_adapter.h"

GST_DEBUG_CATEGORY_STATIC(mqadec_debug);
#define GST_CAT_DEFAULT mqadec_debug

#define GST_TYPE_MQADEC (gst_mqadec_get_type())
G_DECLARE_FINAL_TYPE(GstMqadec, gst_mqadec, GST, MQADEC, GstElement)

struct _GstMqadec {
	GstElement element;
	GstPad *sinkpad, *srcpad;

	struct mqa_adapter adapter;
	gboolean started;              /* the adapter has an input format   */
	gboolean negotiated;           /* the output caps are set           */
	gint in_rate;
	gint in_shift;                 /* to left-justify the input samples */
	gint in_bpf;                   /* bytes per input frame             */
	guint64 out_frames;            /* frames pushed, for timestamps     */
	GstClockTime segment_start;
	GstEvent *pending_segment;     /* held until the caps are known     */
	gint32 *scratch;               /* input converted for the adapter   */
	gsize scratch_frames;

	/* properties */
	gboolean signalling;
	gint passthrough;
	guint sniff_ms;
};

G_DEFINE_TYPE(GstMqadec, gst_mqadec, GST_TYPE_ELEMENT)

enum { PROP_0, PROP_SIGNALLING, PROP_PASSTHROUGH, PROP_SNIFF_TIME };

#define GST_TYPE_MQADEC_PASSTHROUGH (gst_mqadec_passthrough_get_type())
static GType gst_mqadec_passthrough_get_type(void)
{
	static gsize id = 0;
	static const GEnumValue values[] = {
		{ MQA_ADAPTER_HOLD, "Repeat them, so the output stays in time", "hold" },
		{ MQA_ADAPTER_DROP, "Drop them", "drop" },
		{ MQA_ADAPTER_RAW,  "Emit them once, as the reference decoder does", "raw" },
		{ 0, NULL, NULL }
	};

	if (g_once_init_enter(&id)) {
		GType t = g_enum_register_static("GstMqadecPassthrough", values);

		g_once_init_leave(&id, t);
	}
	return (GType)id;
}

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
	"sink", GST_PAD_SINK, GST_PAD_ALWAYS,
	GST_STATIC_CAPS("audio/x-raw, "
			"format = (string) { S16LE, S24_32LE, S32LE }, "
			"layout = (string) interleaved, "
			"rate = (int) { 44100, 48000 }, "
			"channels = (int) 2"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
	"src", GST_PAD_SRC, GST_PAD_ALWAYS,
	GST_STATIC_CAPS("audio/x-raw, "
			"format = (string) S32LE, "
			"layout = (string) interleaved, "
			"rate = (int) { 44100, 48000, 88200, 96000 }, "
			"channels = (int) 2"));

/* --- the stream ---------------------------------------------------------- */

static void gst_mqadec_stop(GstMqadec *self)
{
	if (self->started)
		mqa_adapter_clear(&self->adapter);
	self->started = FALSE;
	self->negotiated = FALSE;
	self->out_frames = 0;
	gst_event_replace(&self->pending_segment, NULL);
	g_free(self->scratch);
	self->scratch = NULL;
	self->scratch_frames = 0;
}

static gboolean gst_mqadec_start(GstMqadec *self, gint rate)
{
	unsigned sniff = (unsigned)((guint64)self->sniff_ms * (guint)rate / 1000);

	if (self->started)
		mqa_adapter_clear(&self->adapter);
	if (mqa_adapter_init(&self->adapter, (unsigned)rate, sniff) < 0)
		return FALSE;
	mqa_adapter_set_signalling(&self->adapter, self->signalling);
	mqa_adapter_set_passthrough(&self->adapter, (enum mqa_adapter_passthrough)self->passthrough);
	self->started = TRUE;
	self->negotiated = FALSE;
	self->out_frames = 0;
	return TRUE;
}

static gboolean gst_mqadec_negotiate(GstMqadec *self)
{
	GstCaps *caps;
	gboolean ok;

	caps = gst_caps_new_simple("audio/x-raw",
				   "format", G_TYPE_STRING, "S32LE",
				   "layout", G_TYPE_STRING, "interleaved",
				   "rate", G_TYPE_INT, (gint)mqa_adapter_output_rate(&self->adapter),
				   "channels", G_TYPE_INT, 2, NULL);
	GST_INFO_OBJECT(self, "%s: output %u Hz",
			mqa_adapter_state(&self->adapter) == MQA_ADAPTER_DECODING
				? "MQA stream found" : "no MQA stream",
			mqa_adapter_output_rate(&self->adapter));
	ok = gst_pad_set_caps(self->srcpad, caps);
	gst_caps_unref(caps);
	self->negotiated = ok;
	/* the segment waited for the caps: downstream wants them first */
	if (ok && self->pending_segment) {
		GstEvent *seg = self->pending_segment;

		self->pending_segment = NULL;
		gst_pad_push_event(self->srcpad, seg);
	}
	return ok;
}

/* Everything the adapter has ready, as buffers on the source pad. */
static GstFlowReturn gst_mqadec_push_ready(GstMqadec *self)
{
	GstFlowReturn ret = GST_FLOW_OK;
	guint rate;

	if (!self->started || mqa_adapter_state(&self->adapter) == MQA_ADAPTER_SNIFFING)
		return GST_FLOW_OK;
	if (!self->negotiated && !gst_mqadec_negotiate(self))
		return GST_FLOW_NOT_NEGOTIATED;
	rate = mqa_adapter_output_rate(&self->adapter);

	for (;;) {
		size_t have = mqa_adapter_available(&self->adapter), got;
		GstBuffer *out;
		GstMapInfo map;

		if (have == 0)
			return ret;
		out = gst_buffer_new_allocate(NULL, have * 8, NULL);
		if (!out)
			return GST_FLOW_ERROR;
		if (!gst_buffer_map(out, &map, GST_MAP_WRITE)) {
			gst_buffer_unref(out);
			return GST_FLOW_ERROR;
		}
		got = mqa_adapter_pull(&self->adapter, (gint32 *)(gpointer)map.data, have);
		gst_buffer_unmap(out, &map);
		if (got == 0) {
			gst_buffer_unref(out);
			return ret;
		}
		gst_buffer_set_size(out, (gssize)(got * 8));
		GST_BUFFER_PTS(out) = self->segment_start +
			gst_util_uint64_scale(self->out_frames, GST_SECOND, rate);
		GST_BUFFER_DTS(out) = GST_BUFFER_PTS(out);
		GST_BUFFER_DURATION(out) = gst_util_uint64_scale(got, GST_SECOND, rate);
		GST_BUFFER_OFFSET(out) = self->out_frames;
		GST_BUFFER_OFFSET_END(out) = self->out_frames + got;
		self->out_frames += got;

		ret = gst_pad_push(self->srcpad, out);
		if (ret != GST_FLOW_OK)
			return ret;
	}
}

/* --- pad functions -------------------------------------------------------- */

static GstFlowReturn gst_mqadec_chain(GstPad *pad, GstObject *parent, GstBuffer *buf)
{
	GstMqadec *self = GST_MQADEC(parent);
	GstMapInfo map;
	gsize frames, i;

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
	/*
	 * The library works in 24-bit values held in the top of a 32-bit
	 * word. A 16-bit carrier (an MQA CD) is aligned the same way, which
	 * leaves its data channel empty; those streams carry their
	 * residuals in the carrier's own digits instead.
	 */
	if (self->in_shift == 0) {
		memcpy(self->scratch, map.data, frames * 8);
	} else if (self->in_bpf == 4) {
		const gint16 *in = (const gint16 *)(gconstpointer)map.data;

		for (i = 0; i < frames * 2; i++)
			self->scratch[i] = (gint32)in[i] << 16;
	} else {
		const gint32 *in = (const gint32 *)(gconstpointer)map.data;

		for (i = 0; i < frames * 2; i++)
			self->scratch[i] = in[i] << self->in_shift;
	}
	gst_buffer_unmap(buf, &map);
	gst_buffer_unref(buf);

	if (mqa_adapter_push(&self->adapter, self->scratch, frames) < 0) {
		GST_ELEMENT_ERROR(self, STREAM, DECODE, (NULL),
				  ("the decoder met a stream it cannot decode"));
		return GST_FLOW_ERROR;
	}
	return gst_mqadec_push_ready(self);
}

static gboolean gst_mqadec_sink_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
	GstMqadec *self = GST_MQADEC(parent);

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
		case GST_AUDIO_FORMAT_S16LE:   self->in_shift = 16; break;
		case GST_AUDIO_FORMAT_S24_32LE: self->in_shift = 8; break;
		default:                        self->in_shift = 0; break;   /* S32LE */
		}
		gst_event_unref(event);
		return gst_mqadec_start(self, self->in_rate);
	}
	case GST_EVENT_SEGMENT: {
		const GstSegment *seg;

		gst_event_parse_segment(event, &seg);
		self->segment_start = seg->format == GST_FORMAT_TIME ? seg->start : 0;
		self->out_frames = 0;
		if (self->negotiated)
			return gst_pad_push_event(self->srcpad, event);
		gst_event_replace(&self->pending_segment, event);
		gst_event_unref(event);
		return TRUE;
	}
	case GST_EVENT_EOS: {
		GstFlowReturn ret;

		if (self->started) {
			mqa_adapter_drain(&self->adapter);
			ret = gst_mqadec_push_ready(self);
			if (ret != GST_FLOW_OK)
				GST_WARNING_OBJECT(self, "flow %s while draining",
						   gst_flow_get_name(ret));
		}
		return gst_pad_push_event(self->srcpad, event);
	}
	case GST_EVENT_FLUSH_STOP:
		if (self->started) {
			enum mqa_adapter_state was = mqa_adapter_state(&self->adapter);

			mqa_adapter_reset(&self->adapter);
			/* a seek stays in the same file: keep the format we
			 * negotiated rather than sniffing our way to another */
			if (was != MQA_ADAPTER_SNIFFING)
				mqa_adapter_force(&self->adapter, was);
		}
		self->out_frames = 0;
		return gst_pad_push_event(self->srcpad, event);
	default:
		return gst_pad_event_default(pad, parent, event);
	}
}

static gboolean gst_mqadec_src_query(GstPad *pad, GstObject *parent, GstQuery *query)
{
	GstMqadec *self = GST_MQADEC(parent);

	if (GST_QUERY_TYPE(query) == GST_QUERY_LATENCY) {
		gboolean live;
		GstClockTime min, max, ours;

		if (!gst_pad_peer_query(self->sinkpad, query))
			return FALSE;
		gst_query_parse_latency(query, &live, &min, &max);
		ours = self->in_rate > 0
			? gst_util_uint64_scale(mqa_adapter_latency_frames(&self->adapter),
						GST_SECOND, (guint64)self->in_rate)
			: 0;
		min += ours;
		if (max != GST_CLOCK_TIME_NONE)
			max += ours;
		gst_query_set_latency(query, live, min, max);
		return TRUE;
	}
	return gst_pad_query_default(pad, parent, query);
}

/* --- object plumbing ------------------------------------------------------ */

static void gst_mqadec_set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	GstMqadec *self = GST_MQADEC(object);

	switch (id) {
	case PROP_SIGNALLING:
		self->signalling = g_value_get_boolean(value);
		if (self->started)
			mqa_adapter_set_signalling(&self->adapter, self->signalling);
		break;
	case PROP_PASSTHROUGH:
		self->passthrough = g_value_get_enum(value);
		if (self->started)
			mqa_adapter_set_passthrough(&self->adapter,
						    (enum mqa_adapter_passthrough)self->passthrough);
		break;
	case PROP_SNIFF_TIME:
		self->sniff_ms = g_value_get_uint(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static void gst_mqadec_get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	GstMqadec *self = GST_MQADEC(object);

	switch (id) {
	case PROP_SIGNALLING: g_value_set_boolean(value, self->signalling); break;
	case PROP_PASSTHROUGH: g_value_set_enum(value, self->passthrough); break;
	case PROP_SNIFF_TIME: g_value_set_uint(value, self->sniff_ms); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static GstStateChangeReturn gst_mqadec_change_state(GstElement *element, GstStateChange transition)
{
	GstMqadec *self = GST_MQADEC(element);
	GstStateChangeReturn ret;

	ret = GST_ELEMENT_CLASS(gst_mqadec_parent_class)->change_state(element, transition);
	if (transition == GST_STATE_CHANGE_PAUSED_TO_READY)
		gst_mqadec_stop(self);
	return ret;
}

static void gst_mqadec_finalize(GObject *object)
{
	gst_mqadec_stop(GST_MQADEC(object));
	G_OBJECT_CLASS(gst_mqadec_parent_class)->finalize(object);
}

static void gst_mqadec_class_init(GstMqadecClass *klass)
{
	GObjectClass *gobject = G_OBJECT_CLASS(klass);
	GstElementClass *element = GST_ELEMENT_CLASS(klass);

	gobject->set_property = gst_mqadec_set_property;
	gobject->get_property = gst_mqadec_get_property;
	gobject->finalize = gst_mqadec_finalize;
	element->change_state = gst_mqadec_change_state;

	g_object_class_install_property(gobject, PROP_SIGNALLING,
		g_param_spec_boolean("signalling", "Renderer signalling",
				     "Embed the signalling an MQA renderer downstream looks for",
				     FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject, PROP_PASSTHROUGH,
		g_param_spec_enum("passthrough", "Passed-through frames",
				  "What to do with frames the decoder passes through",
				  GST_TYPE_MQADEC_PASSTHROUGH, MQA_ADAPTER_HOLD,
				  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject, PROP_SNIFF_TIME,
		g_param_spec_uint("sniff-time", "Sniff time",
				  "How long to look for a stream before deciding there is none, in ms",
				  100, 60000, 5000, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	gst_element_class_add_static_pad_template(element, &sink_template);
	gst_element_class_add_static_pad_template(element, &src_template);
	gst_element_class_set_static_metadata(element,
		"MQA first unfold", "Filter/Converter/Audio",
		"Unfolds an MQA stream hidden in stereo PCM, doubling the sample rate",
		"the mqa-decode project");
}

static void gst_mqadec_init(GstMqadec *self)
{
	self->sinkpad = gst_pad_new_from_static_template(&sink_template, "sink");
	gst_pad_set_chain_function(self->sinkpad, gst_mqadec_chain);
	gst_pad_set_event_function(self->sinkpad, gst_mqadec_sink_event);
	GST_PAD_SET_PROXY_CAPS(self->sinkpad);
	gst_element_add_pad(GST_ELEMENT(self), self->sinkpad);

	self->srcpad = gst_pad_new_from_static_template(&src_template, "src");
	gst_pad_set_query_function(self->srcpad, gst_mqadec_src_query);
	gst_pad_use_fixed_caps(self->srcpad);
	gst_element_add_pad(GST_ELEMENT(self), self->srcpad);

	self->sniff_ms = 5000;
	self->passthrough = MQA_ADAPTER_HOLD;
}

static gboolean plugin_init(GstPlugin *plugin)
{
	GST_DEBUG_CATEGORY_INIT(mqadec_debug, "mqadec", 0, "MQA first unfold");
	return gst_element_register(plugin, "mqadec", GST_RANK_NONE, GST_TYPE_MQADEC);
}

#ifndef PACKAGE
#define PACKAGE "mqa-decode"
#endif
#ifndef VERSION
#define VERSION "1.0"
#endif

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, mqadec,
		  "MQA first unfold", plugin_init, VERSION, "MIT/X11",
		  "mqa-decode", "https://github.com/audioburro/mqa")
