/*
 * The encoder end to end, measured by the decoder.
 *
 * A signal at twice the carrier rate is encoded; the carrier that comes
 * out is decoded by the library; the result is compared with what went
 * in. Three things are checked: that the stream decodes at all (every
 * group unfolded, nothing passed through), that the encoder's model of
 * the decoder's front end is right (the carrier it worked back from is
 * the carrier the decoder really reconstructs from), and how much of
 * the signal survived.
 */
#include <math.h>
#include "mqae/encode.h"
#include "mqa/stream_decoder.h"
#include "util.h"

#define FRAMES   (16 * 4096)          /* carrier frames             */
#define SOURCE   (2 * FRAMES)

static int32_t src[2 * SOURCE], carrier[2 * FRAMES];
static int32_t out[2 * (SOURCE + MQA_STREAM_DECODER_GROUP_MAX)];

/* What the decoder really reconstructs from, taken from its own hook,
 * and what the encoder's model said it would be. */
static int32_t seen_a[FRAMES], model_a[FRAMES];
static int32_t seen_a0[FRAMES], model_a0[FRAMES];
static unsigned seen, modelled;

static void layer(void *user, const int32_t *a0, const int32_t *b0,
		  const int32_t *a, const int32_t *b,
		  const int32_t *p, const int32_t *q, unsigned n)
{
	unsigned i;

	(void)user; (void)b0; (void)b; (void)p; (void)q;
	for (i = 0; i < n && seen < FRAMES; i++) {
		seen_a0[seen] = a0[i];
		seen_a[seen++] = a[i];
	}
}

static void modelled_carrier(void *user, const int32_t *a0, const int32_t *b0,
			     const int32_t *a, const int32_t *b, unsigned n)
{
	unsigned i;

	(void)user; (void)b0; (void)b;
	for (i = 0; i < n && modelled < FRAMES; i++) {
		model_a0[modelled] = a0[i];
		model_a[modelled++] = a[i];
	}
}

int main(void)
{
	static struct mqae_encode e;
	static struct mqa_stream_decoder sd;
	struct mqae_config cfg;
	size_t produced = 0, fed = 0, written = 0;
	unsigned i;
	double err = 0, sig = 0;

	/* a signal with real content above the carrier's Nyquist */
	for (i = 0; i < SOURCE; i++) {
		double t = i;
		/* the shape real material has: most of the energy well below
		 * the carrier's Nyquist, a little above it */
		double v = 0.45 * sin(0.011 * t) + 0.25 * sin(0.37 * t + 0.7)
			 + 0.0002 * sin(1.90 * t + 1.9);

		src[2 * i] = (int32_t)(3000000.0 * v) << 8;
		src[2 * i + 1] = (int32_t)(2500000.0 * v * 0.8) << 8;
	}

	mqae_config_default(&cfg);
	cfg.src_rate = 48000;
	cfg.orig_rate = 96000;
	if (mqae_encode_open(&e, &cfg, FRAMES) < 0)
		return 1;
	e.on_carrier = modelled_carrier;
	if (mqae_encode_push(&e, src, SOURCE, 1, carrier, FRAMES, &produced) < 0)
		printf("  encode failed\n");
	CHECK_EQ("the encoder ran", e.failed, 0);
	CHECK_EQ("it produced a carrier frame per source pair", (int)produced, FRAMES);
	printf("  %llu bytes of residual data (%.1f bits a frame of the 16 there are),"
	       " backlog at most %u bytes, %lu symbols clipped\n",
	       (unsigned long long)e.bytes, e.bytes * 8.0 / produced,
	       (unsigned)e.chan_peak, e.clipped);
	printf("  the residuals themselves came back %.1f dB down\n",
	       10 * log10(e.residual_energy / (e.coding_error > 0 ? e.coding_error : 1e-9)));
	CHECK_EQ("the data channel kept up", (int)(e.chan_peak < 960), 1);

	/* decode it */
	mqa_stream_decoder_init(&sd, 48000);
	sd.dec.on_layer = layer;
	while (fed < produced) {
		unsigned space = mqa_stream_decoder_space(&sd);
		size_t take = produced - fed < space ? produced - fed : space;

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
	/* the model against the decoder itself */
	{
		unsigned bad0 = 0, bad = 0, first0 = 0, first = 0;

		for (i = 0; i < seen && i < modelled; i++) {
			if (seen_a0[i] != model_a0[i]) {
				if (!bad0)
					first0 = i;
				bad0++;
			}
			if (seen_a[i] != model_a[i]) {
				if (!bad)
					first = i;
				bad++;
			}
		}
		if (bad0)
			printf("  the conditioner differs at %u of %u samples,"
			       " first at %u (%d against %d)\n",
			       bad0, seen, first0, model_a0[first0], seen_a0[first0]);
		if (bad)
			printf("  the refinement differs at %u of %u samples,"
			       " first at %u (%d against %d)\n",
			       bad, seen, first, model_a[first], seen_a[first]);
		if (bad0) {
			/* if they were configured differently, that is why */
			printf("    model:   gain %d gain2 %d shift %u dither %u seed2 %08x"
			       " lag %d/%d six %d full %d\n",
			       e.cond.gain, e.cond.gain2, e.cond.shift, e.cond.dither_mode,
			       e.cond.seed2, e.cond.lag_out, e.cond.lag_prev, e.cond.six_taps,
			       e.cond.full);
			printf("    decoder: gain %d gain2 %d shift %u dither %u seed2 %08x"
			       " lag %d/%d six %d full %d\n",
			       sd.in.cond.gain, sd.in.cond.gain2, sd.in.cond.shift,
			       sd.in.cond.dither_mode, sd.in.cond.seed2, sd.in.cond.lag_out,
			       sd.in.cond.lag_prev, sd.in.cond.six_taps, sd.in.cond.full);
			printf("    model taps %p decoder taps %p; counts %u/%u vs %u/%u\n",
			       (const void *)e.cond.taps, (const void *)sd.in.cond.taps,
			       e.cond.count1, e.cond.count2,
			       sd.in.cond.count1, sd.in.cond.count2);
		}
		CHECK_EQ("the model's conditioner is the decoder's", bad0, 0);
		CHECK_EQ("the model's carrier is the decoder's", bad, 0);
	}
	if (sd.dec.passed)
		printf("  %lu frames passed through; stream found at %llu, ends at %u,"
		       " position %u, groups %lu, parser %s after %u messages, mode %d\n",
		       sd.dec.passed, (unsigned long long)sd.in.bs.sync_frame, sd.in.end_pos,
		       sd.in.stream_pos, sd.groups, sd.dec.parser.started ? "on" : "off",
		       sd.dec.parser.items, sd.dec.mode);
	CHECK_EQ("nothing passed through", (int)sd.dec.passed, 0);
	CHECK_EQ("no group declined", (int)sd.declined, 0);
	CHECK_EQ("two output frames per carrier frame", (int)written, (int)(2 * produced));

	/* how close did it come? the first taps are the filter's empty
	 * start, which no carrier can do anything about */
	for (i = 2 * 64; i < 2 * (unsigned)produced && i < SOURCE; i++) {
		double d = (double)(out[2 * i] >> 8) - (src[2 * i] >> 8);

		err += d * d;
		sig += (double)(src[2 * i] >> 8) * (src[2 * i] >> 8);
	}
	printf("  reconstruction: %.1f dB SNR (rms error %.0f of %.0f)\n",
	       10 * log10(sig / (err > 0 ? err : 1e-9)), sqrt(err / SOURCE), sqrt(sig / SOURCE));
	CHECK_EQ("the source came back", (int)(err < sig * 0.01), 1);

	mqae_encode_close(&e);
	printf("encoder end to end: %s\n", test_fails ? "FAILED" : "ok");
	return test_fails != 0;
}
