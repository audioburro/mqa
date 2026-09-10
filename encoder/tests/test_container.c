/*
 * The container, end to end: encode a carrier, then decode it with the
 * library and check that the decoder found the stream, read its
 * parameters, joined the data channel and unfolded rather than passed
 * through. The audio is not checked here (test_encode does that); this
 * is the container alone.
 */
#include "mqae/encoder.h"
#include "mqa/stream_decoder.h"
#include <math.h>
#include "util.h"

#define FRAMES 8192

static int32_t carrier[2 * FRAMES];
/* the decoder's mode while it runs: 2 is unfolding */
static int decoding_groups, params_seen;

static void note_mode(void *user, struct mqa_stream_decoder *s)
{
	(void)user;
	if (s->dec.mode == 2)
		decoding_groups++;
	if (s->in.have_params)
		params_seen = 1;
}

static int32_t out[2 * (2 * FRAMES + MQA_STREAM_DECODER_GROUP_MAX)];

/* Something with content in every sample, so the embedding has to round
 * rather than fill zeros. */
static void make_carrier(void)
{
	unsigned i;

	for (i = 0; i < FRAMES; i++) {
		double t = i / 48000.0;
		int32_t l = (int32_t)(2000000.0 * sin(6.283185307 * 440.0 * t));
		int32_t r = (int32_t)(1500000.0 * sin(6.283185307 * 660.0 * t + 1.0));

		carrier[2 * i] = l << 8;
		carrier[2 * i + 1] = r << 8;
	}
}

int main(void)
{
	static struct mqa_stream_decoder sd;
	struct mqae_encoder e;
	struct mqae_config cfg;
	size_t fed = 0, written = 0;

	make_carrier();
	mqae_config_default(&cfg);
	cfg.src_rate = 48000;
	cfg.orig_rate = 96000;
	if (mqae_encoder_open(&e, &cfg, FRAMES) < 0)
		return 1;
	mqae_encoder_write(&e, carrier, FRAMES, 0);
	CHECK_EQ("encoded", e.failed, 0);
	CHECK_EQ("the stream says where it ends", e.terminated, 1);

	/* the carrier still sounds like the carrier */
	{
		unsigned i;
		long worst = 0;

		for (i = 0; i < FRAMES; i++) {
			double t = i / 48000.0;
			long want = (long)(2000000.0 * sin(6.283185307 * 440.0 * t));
			long got = carrier[2 * i] >> 8;

			if (labs(got - want) > worst)
				worst = labs(got - want);
		}
		CHECK_EQ("embedding stays within a step", worst <= 256, 1);
	}

	mqa_stream_decoder_init(&sd, 48000);
	sd.before_group = note_mode;
	while (fed < FRAMES) {
		unsigned space = mqa_stream_decoder_space(&sd);
		size_t take = FRAMES - fed < space ? FRAMES - fed : space;

		if (take)
			fed += mqa_stream_decoder_feed(&sd, carrier + 2 * fed, take);
		written += mqa_stream_decoder_run(&sd, out + 2 * written,
						  sizeof out / (2 * sizeof out[0]) - written);
		if (!take && !space)
			break;
	}
	mqa_stream_decoder_finish(&sd);
	for (;;) {
		size_t n = mqa_stream_decoder_run(&sd, out + 2 * written,
						  sizeof out / (2 * sizeof out[0]) - written);
		if (!n)
			break;
		written += n;
	}

	CHECK_EQ("the stream was found", sd.in.xbit, (int)cfg.xbit);
	CHECK_EQ("at frame 0", (int)sd.in.bs.sync_frame, 0);
	CHECK_EQ("its parameters arrived", params_seen, 1);
	CHECK_EQ("it aligned", sd.in.aligned, 1);
	CHECK_EQ("the datasync parsed", sd.in.bs.errors, 0);
	CHECK_EQ("the decoder unfolded every group it could", decoding_groups, (int)sd.groups - 1);
	CHECK_EQ("nothing unsupported", sd.dec.unsupported, 0);
	CHECK_EQ("no group declined", (int)sd.declined, 0);
	CHECK_EQ("the data channel started", sd.dec.parser.started, 1);
	CHECK_EQ("its messages parsed", (int)(sd.dec.parser.items > 0), 1);
	CHECK_EQ("the parameter record installed the scale",
		 (int)sd.dec.params.scale_index, (int)cfg.scale_index);
	CHECK_EQ("nothing passed through", (int)sd.dec.passed, 0);
	CHECK_EQ("two output frames per carrier frame", (int)written, 2 * FRAMES);
	if (sd.dec.unsupported_why)
		printf("  (unsupported: %s)\n", sd.dec.unsupported_why);

	mqae_encoder_close(&e);
	printf("encoder container: %s\n", test_fails ? "FAILED" : "ok");
	return test_fails != 0;
}
