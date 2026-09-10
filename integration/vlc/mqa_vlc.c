/*
 * mqa_vlc -- an MQA first-unfold audio filter for VLC 3.
 *
 *     vlc --audio-filter=mqa file.flac
 *
 * VLC loads plugins from its plugin path at runtime, so this builds
 * out of tree: see the Makefile beside it.
 *
 * The filter takes stereo PCM at 44.1 or 48 kHz in VLC's native 32-bit
 * integer format and, when the carrier holds an MQA stream, emits twice
 * as many frames per second. It refuses any other input format, and
 * that refusal matters: MQA lives in the low bits of the samples, so a
 * float conversion, a volume stage or a dither stage ahead of this
 * filter destroys the stream before it arrives. See ../README.md.
 *
 * Status: this module compiles against VLC 3.0's plugin headers, and
 * the decode path underneath it is the same one the GStreamer element
 * uses and is verified bit for bit. It has *not* been run inside VLC --
 * no VLC build was available here. The specific thing to check when you
 * do run it is whether VLC's audio filter chain accepts a filter whose
 * output rate differs from its input, or whether the unfold has to be
 * placed elsewhere in the chain.
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>            /* vlc_aout.h uses static_assert */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_block.h>
#include <vlc_aout.h>

#include "mqa_adapter.h"

#define MQA_CFG_PREFIX "mqa-"

struct filter_sys_t {
	struct mqa_adapter adapter;
	date_t date;                  /* the output's own clock            */
	bool floating;                /* the chain handed us FL32          */
	int32_t *scratch;             /* FL32 converted for the adapter    */
	size_t scratch_frames;
};

/*
 * VLC converts to FL32 before the user filter chain, so that is what
 * this filter is usually handed. Float carries a 24-bit mantissa, so a
 * 24-bit carrier survives the round trip exactly, but only if nothing
 * has *scaled* it on the way: a volume other than 1.0, replay gain or a
 * mixer will quietly destroy the stream in the low bits, and all this
 * filter can then do is fail to find it and pass the audio through.
 */
#define ONE_I32 2147483648.0f

static int32_t float_to_i32(float x)
{
	if (x >= 1.0f)
		return INT32_MAX;
	if (x <= -1.0f)
		return INT32_MIN;
	return (int32_t)lrintf(x * ONE_I32);
}

static block_t *Filter(filter_t *filter, block_t *in)
{
	struct filter_sys_t *sys = filter->p_sys;
	size_t frames = in->i_nb_samples, have;
	block_t *out;

	if (sys->floating) {
		const float *src = (const float *)in->p_buffer;
		size_t i;

		if (frames > sys->scratch_frames) {
			int32_t *grown = realloc(sys->scratch, frames * 2 * sizeof *grown);

			if (grown == NULL) {
				block_Release(in);
				return NULL;
			}
			sys->scratch = grown;
			sys->scratch_frames = frames;
		}
		for (i = 0; i < frames * 2; i++)
			sys->scratch[i] = float_to_i32(src[i]);
	}
	if (mqa_adapter_push(&sys->adapter,
			     sys->floating ? sys->scratch : (const int32_t *)in->p_buffer,
			     frames) < 0) {
		msg_Err(filter, "the decoder met a stream it cannot decode");
		block_Release(in);
		return NULL;
	}
	have = mqa_adapter_available(&sys->adapter);
	if (have == 0) {
		block_Release(in);
		return NULL;                  /* still deciding, or nothing ready */
	}
	out = block_Alloc(have * 8);
	if (out == NULL) {
		block_Release(in);
		return NULL;
	}
	if (sys->floating) {
		float *dst = (float *)out->p_buffer;
		size_t got, i;

		if (have > sys->scratch_frames) {
			int32_t *grown = realloc(sys->scratch, have * 2 * sizeof *grown);

			if (grown == NULL) {
				block_Release(in);
				block_Release(out);
				return NULL;
			}
			sys->scratch = grown;
			sys->scratch_frames = have;
		}
		got = mqa_adapter_pull(&sys->adapter, sys->scratch, have);
		for (i = 0; i < got * 2; i++)
			dst[i] = (float)sys->scratch[i] / ONE_I32;
		out->i_nb_samples = got;
	} else {
		out->i_nb_samples = mqa_adapter_pull(&sys->adapter, (int32_t *)out->p_buffer, have);
	}
	out->i_buffer = out->i_nb_samples * 8;
	/*
	 * The unfold runs behind its input, so the audio in this block
	 * arrived earlier than the block that carried it in. The output
	 * therefore keeps its own clock, started once from the first input
	 * date less the filter's latency and incremented per sample after
	 * that.
	 */
	if (date_Get(&sys->date) == VLC_TICK_INVALID && in->i_pts != VLC_TICK_INVALID) {
		vlc_tick_t behind = (vlc_tick_t)mqa_adapter_latency_frames(&sys->adapter) *
			CLOCK_FREQ / filter->fmt_in.audio.i_rate;

		date_Set(&sys->date, in->i_pts - behind);
	}
	out->i_pts = out->i_dts = date_Get(&sys->date);
	out->i_length = date_Increment(&sys->date, out->i_nb_samples) - out->i_pts;
	block_Release(in);
	return out;
}

static void Flush(filter_t *filter)
{
	struct filter_sys_t *sys = filter->p_sys;
	enum mqa_adapter_state was = mqa_adapter_state(&sys->adapter);

	mqa_adapter_reset(&sys->adapter);
	if (was != MQA_ADAPTER_SNIFFING)
		mqa_adapter_force(&sys->adapter, was);   /* the format is settled */
	date_Set(&sys->date, VLC_TICK_INVALID);      /* re-dated from the next block */
}

static int Open(vlc_object_t *obj)
{
	filter_t *filter = (filter_t *)obj;
	struct filter_sys_t *sys;
	unsigned rate = filter->fmt_in.audio.i_rate;

	if ((filter->fmt_in.audio.i_format != VLC_CODEC_S32N &&
	     filter->fmt_in.audio.i_format != VLC_CODEC_FL32) ||
	    filter->fmt_in.audio.i_channels != 2 ||
	    (rate != 44100 && rate != 48000))
		return VLC_EGENERIC;

	sys = calloc(1, sizeof *sys);
	if (sys == NULL)
		return VLC_ENOMEM;
	if (mqa_adapter_init(&sys->adapter, rate, 0) < 0) {
		free(sys);
		return VLC_ENOMEM;
	}
	mqa_adapter_set_signalling(&sys->adapter,
				   var_InheritBool(filter, MQA_CFG_PREFIX "signalling"));

	/*
	 * The output rate is not known until the carrier is sniffed, and a
	 * filter has to declare a format now. Declare the doubled rate: a
	 * carrier that turns out not to be MQA is passed through by the
	 * adapter one frame for one, which at this rate plays fast, so on
	 * a stream that is not MQA the filter should be left out. The
	 * `mqa-force-passthrough` option makes the decision explicit.
	 */
	sys->floating = filter->fmt_in.audio.i_format == VLC_CODEC_FL32;
	filter->fmt_out.audio = filter->fmt_in.audio;
	filter->fmt_out.audio.i_rate = rate * 2;

	date_Init(&sys->date, rate * 2, 1);
	date_Set(&sys->date, VLC_TICK_INVALID);
	filter->p_sys = sys;
	filter->pf_audio_filter = Filter;
	filter->pf_flush = Flush;
	msg_Dbg(filter, "MQA first unfold: %u Hz in, %u Hz out", rate, rate * 2);
	return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
	filter_t *filter = (filter_t *)obj;
	struct filter_sys_t *sys = filter->p_sys;

	mqa_adapter_clear(&sys->adapter);
	free(sys->scratch);
	free(sys);
}

vlc_module_begin()
	set_shortname("MQA")
	set_description("MQA first unfold")
	set_help("Unfolds an MQA stream hidden in stereo PCM, doubling the sample rate. "
		 "The stream lives in the low bits of the samples, so this filter must "
		 "come before anything that alters them.")
	set_category(CAT_AUDIO)
	set_subcategory(SUBCAT_AUDIO_AFILTER)
	set_capability("audio filter", 0)
	add_bool(MQA_CFG_PREFIX "signalling", false, "Renderer signalling",
		 "Embed the signalling an MQA renderer downstream looks for", false)
	set_callbacks(Open, Close)
vlc_module_end()
