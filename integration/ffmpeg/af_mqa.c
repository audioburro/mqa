/*
 * MQA first unfold: an audio filter for libavfilter.
 *
 * SPDX-License-Identifier: MIT
 *
 * FFmpeg has no runtime plugin interface, so this filter has to be built
 * into the tree: see ../README.md for the three lines that register it
 * and the configure flags that point at the decoding library.
 *
 *     ffmpeg -i in.flac -af mqa -c:a flac out.flac
 *
 * The filter takes stereo S32 at 44.1 or 48 kHz and emits twice as many
 * frames per second. Because a filter graph settles its formats before
 * a single sample flows, the doubled rate is declared up front and the
 * adapter is told to keep two output frames per input frame whatever the
 * carrier turns out to hold, so applying `mqa` to audio that is not
 * MQA gives a plain sample-and-hold upsample rather than a stream that
 * runs fast.
 *
 * MQA lives in the low bits of the samples: put this filter before
 * anything that scales them, and keep the sample format integral. A
 * volume, a resample or a float round trip through a mixer ahead of it
 * will destroy the stream.
 */

#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "audio.h"
#include "filters.h"
#include "formats.h"

#include "mqa_adapter.h"

typedef struct MQAContext {
    const AVClass *class;
    struct mqa_adapter adapter;
    int started;
    int signalling;
    int unfolds;
    int ratio;
    int requantise;
    int eof;
    int64_t next_pts;
} MQAContext;

#define OFFSET(x) offsetof(MQAContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM

static const AVOption mqa_options[] = {
    { "signalling", "embed the signalling an MQA renderer looks for",
      OFFSET(signalling), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "unfolds", "1: the first unfold only; 2: the second as well",
      OFFSET(unfolds), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, 2, FLAGS },
    { "ratio", "the second unfold's ratio on top of the doubling: 2 or 4",
      OFFSET(ratio), AV_OPT_TYPE_INT, { .i64 = 2 }, 2, 4, FLAGS },
    { "requantise", "with two unfolds, requantise the output as an MQA renderer does",
      OFFSET(requantise), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(mqa);

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    static const enum AVSampleFormat formats[] = {
        AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_NONE
    };
    static const AVChannelLayout layouts[] = {
        AV_CHANNEL_LAYOUT_STEREO, { .nb_channels = 0 }
    };
    static const int in_rates[] = { 44100, 48000, -1 };
    static const int out_rates[] = { 88200, 96000, 176400, 192000, 352800, 384000, -1 };
    int ret;

    if ((ret = ff_set_common_formats_from_list2(ctx, cfg_in, cfg_out, formats)) < 0)
        return ret;
    if ((ret = ff_set_common_channel_layouts_from_list2(ctx, cfg_in, cfg_out, layouts)) < 0)
        return ret;
    if ((ret = ff_formats_ref(ff_make_format_list(in_rates), &cfg_in[0]->samplerates)) < 0)
        return ret;
    /*
     * The output rate is twice the input's (times the render ratio with
     * two unfolds), but a filter cannot say that directly: it offers the
     * possible rates and lets negotiation pair them, which it does by
     * preferring the output nearest the input. config_output then
     * insists on the pairing being the right one.
     */
    return ff_formats_ref(ff_make_format_list(out_rates), &cfg_out[0]->samplerates);
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    MQAContext *s = ctx->priv;
    int ratio = s->unfolds == 2 ? s->ratio : 1;

    if (outlink->sample_rate != inlink->sample_rate * 2 * ratio) {
        av_log(ctx, AV_LOG_ERROR,
               "the unfold multiplies the rate by %d: %d Hz in wants %d Hz out, not %d\n",
               2 * ratio, inlink->sample_rate, inlink->sample_rate * 2 * ratio, outlink->sample_rate);
        return AVERROR(EINVAL);
    }
    outlink->time_base = (AVRational){ 1, outlink->sample_rate };

    if (mqa_adapter_init(&s->adapter, inlink->sample_rate, 0) < 0)
        return AVERROR(ENOMEM);
    mqa_adapter_set_signalling(&s->adapter, s->signalling);
    if (ratio > 1)
        mqa_adapter_set_render(&s->adapter, (unsigned)ratio, s->requantise);
    /* the graph is committed to the output rate already */
    mqa_adapter_force(&s->adapter, MQA_ADAPTER_DECODING);
    s->started = 1;
    return 0;
}

/* Everything the adapter has ready, as frames on the output link. */
static int emit(AVFilterContext *ctx)
{
    MQAContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    size_t have;

    while ((have = mqa_adapter_available(&s->adapter)) > 0) {
        AVFrame *out;
        size_t got;
        int ret;

        if (have > 4096)
            have = 4096;
        out = ff_get_audio_buffer(outlink, (int)have);
        if (!out)
            return AVERROR(ENOMEM);
        got = mqa_adapter_pull(&s->adapter, (int32_t *)out->data[0], have);
        if (!got) {
            av_frame_free(&out);
            break;
        }
        out->nb_samples = (int)got;
        out->pts = s->next_pts;
        s->next_pts += got;
        ret = ff_filter_frame(outlink, out);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    MQAContext *s = ctx->priv;
    int64_t pts;
    int status, ret;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    if (!s->eof && ff_inlink_queued_frames(inlink)) {
        AVFrame *in = NULL;

        ret = ff_inlink_consume_frame(inlink, &in);
        if (ret < 0)
            return ret;
        if (ret > 0) {
            ret = mqa_adapter_push(&s->adapter, (const int32_t *)in->data[0],
                                   (size_t)in->nb_samples);
            av_frame_free(&in);
            if (ret < 0)
                return AVERROR_INVALIDDATA;
            return emit(ctx);
        }
    }

    if (!s->eof && ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        if (status == AVERROR_EOF) {
            s->eof = 1;
            mqa_adapter_drain(&s->adapter);
            ret = emit(ctx);
            if (ret < 0)
                return ret;
            ff_outlink_set_status(outlink, AVERROR_EOF, s->next_pts);
            return 0;
        }
        ff_outlink_set_status(outlink, status, pts);
        return 0;
    }

    FF_FILTER_FORWARD_WANTED(outlink, inlink);
    return FFERROR_NOT_READY;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    MQAContext *s = ctx->priv;

    if (s->started)
        mqa_adapter_clear(&s->adapter);
    s->started = 0;
}

static const AVFilterPad mqa_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_output,
    },
};

const AVFilter ff_af_mqa = {
    .name          = "mqa",
    .description   = NULL_IF_CONFIG_SMALL("Unfold an MQA stream hidden in stereo PCM."),
    .priv_size     = sizeof(MQAContext),
    .priv_class    = &mqa_class,
    .uninit        = uninit,
    .activate      = activate,
    FILTER_INPUTS(ff_audio_default_filterpad),
    FILTER_OUTPUTS(mqa_outputs),
    FILTER_QUERY_FUNC2(query_formats),
};
