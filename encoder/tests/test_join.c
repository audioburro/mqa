/*
 * Joining a stream at one of its resync points.
 *
 * A stream with resync points is encoded and decoded whole; then the
 * carrier is cut a little before one of the points and decoded from
 * there. The joined decode has to converge on the continuous one. It
 * never becomes identical to it (the entropy decoders' adaptive state
 * is not reseeded, see encoder/README.md), so what is checked is that
 * the two agree to well under the encoder's own error from the first
 * block after the join onwards, and that the joiner unfolded the rest
 * of the file rather than passing it through.
 */
#include <math.h>
#include <stdlib.h>
#include "mqae/encode.h"
#include "mqa/stream_decoder.h"
#include "util.h"

/* long enough for resync points: the encoder writes none in the last
 * 65536 frames, where the terminate packet already is */
#define BLOCKS   40
#define FRAMES   (BLOCKS * 4096)      /* carrier frames             */
#define SOURCE   (2 * FRAMES)
#define RESYNC   4                    /* a point every four blocks  */
#define JOIN     (9 * 4096)           /* the block boundary joined at */
#define CUT      (JOIN - MQAE_RESYNC_LEAD - 64)  /* the file starts here */

static int32_t src[2 * SOURCE], carrier[2 * FRAMES];
static int32_t whole[2 * (SOURCE + MQA_STREAM_DECODER_GROUP_MAX)];
static int32_t part[2 * (SOURCE + MQA_STREAM_DECODER_GROUP_MAX)];

static size_t decode(struct mqa_stream_decoder *sd, const int32_t *in, size_t frames,
		     int32_t *out, size_t cap)
{
	size_t fed = 0, written = 0;

	mqa_stream_decoder_init(sd, 48000);
	while (fed < frames) {
		unsigned space = mqa_stream_decoder_space(sd);
		size_t take = frames - fed < space ? frames - fed : space;

		if (take)
			fed += mqa_stream_decoder_feed(sd, in + 2 * fed, take);
		written += mqa_stream_decoder_run(sd, out + 2 * written, cap - written);
		if (!take && !space)
			break;
	}
	mqa_stream_decoder_finish(sd);
	for (;;) {
		size_t n = mqa_stream_decoder_run(sd, out + 2 * written, cap - written);

		if (!n)
			break;
		written += n;
	}
	return written;
}

/* The error between the two decodes over `n` output frames, relative
 * to the signal, in dB (negative is good). */
static double error_db(const int32_t *a, const int32_t *b, size_t n)
{
	double err = 0, sig = 0;
	size_t i;

	for (i = 0; i < 2 * n; i++) {
		double d = (double)(a[i] >> 8) - (b[i] >> 8);

		err += d * d;
		sig += (double)(b[i] >> 8) * (b[i] >> 8);
	}
	return 10 * log10((err > 0 ? err : 1e-9) / (sig > 0 ? sig : 1e-9));
}

int main(void)
{
	static struct mqae_encode e;
	static struct mqa_stream_decoder sd;
	struct mqae_config cfg;
	size_t produced = 0, nwhole, npart, probe, n;
	long lag = 0, best = 0, l;
	double best_err = 0, first, rest;
	unsigned i;

	for (i = 0; i < SOURCE; i++) {
		double t = i;
		double v = 0.45 * sin(0.011 * t) + 0.25 * sin(0.37 * t + 0.7)
			 + 0.0002 * sin(1.90 * t + 1.9);

		src[2 * i] = (int32_t)(3000000.0 * v) << 8;
		src[2 * i + 1] = (int32_t)(2500000.0 * v * 0.8) << 8;
	}

	mqae_config_default(&cfg);
	cfg.src_rate = 48000;
	cfg.orig_rate = 96000;
	cfg.resync_blocks = RESYNC;
	if (mqae_encode_open(&e, &cfg, FRAMES) < 0)
		return 1;
	if (mqae_encode_push(&e, src, SOURCE, 1, carrier, FRAMES, &produced) < 0)
		printf("  encode failed\n");
	CHECK_EQ("the encoder ran", e.failed, 0);
	CHECK_EQ("it produced every frame", (int)produced, FRAMES);
	mqae_encode_close(&e);

	/* the whole file, then the file from the cut */
	nwhole = decode(&sd, carrier, produced, whole, sizeof whole / (2 * sizeof whole[0]));
	CHECK_EQ("the whole file unfolded", (int)nwhole, 2 * FRAMES);
	CHECK_EQ("nothing of it passed through", (int)sd.dec.passed, 0);
	npart = decode(&sd, carrier + 2 * CUT, produced - CUT, part, sizeof part / (2 * sizeof part[0]));
	printf("  cut at %u: %zu frames out of %u in, %lu passed through\n",
	       CUT, npart, FRAMES - CUT, sd.dec.passed);
	/* the frames before the datasync, and up to the next multiple of 32
	 * of the stream's own count, pass through; the rest unfolds */
	CHECK_EQ("the joiner passed through only the frames before the stream",
		 (int)(sd.dec.passed >= 64 && sd.dec.passed < 64 + 16 + 32), 1);
	CHECK_EQ("and unfolded the rest", (int)npart, (int)(2 * (FRAMES - CUT) - sd.dec.passed));

	/*
	 * Line the two up: the joined decode's frames sit at some lag in
	 * the whole one, near twice the cut. Find it where the two have
	 * had a block to converge.
	 */
	probe = 2 * (JOIN + 2 * 4096 - CUT);
	for (l = 2 * CUT - 512; l <= 2 * CUT + 512; l++) {
		double d = error_db(part + 2 * probe, whole + 2 * (probe + l), 4096);

		if (l == 2 * CUT - 512 || d < best_err) {
			best_err = d;
			best = l;
		}
	}
	lag = best;
	printf("  joined decode sits %ld frames into the whole one (%.1f dB apart there)\n",
	       lag, best_err);
	/* the passed-through frames come out one each, the rest two each */
	CHECK_EQ("the lag is where the cut put it", (int)labs(lag - (2 * CUT + (long)sd.dec.passed)), 0);

	/* the block after the join, then everything after that */
	n = 2 * (JOIN - CUT);                              /* the boundary, in output frames */
	first = error_db(part + 2 * n, whole + 2 * (n + lag), 2 * 4096);
	rest = error_db(part + 2 * (n + 2 * 4096), whole + 2 * (n + 2 * 4096 + lag),
			npart - (n + 2 * 4096) - 64);
	printf("  against the continuous decode: %.1f dB in the first block after the join,"
	       " %.1f dB after that\n", first, rest);
	CHECK_EQ("the first block after the join is close", (int)(first < -30), 1);
	CHECK_EQ("the rest is closer than the encoder's own error", (int)(rest < -50), 1);

	printf("joining at a resync point: %s\n", test_fails ? "FAILED" : "ok");
	return test_fails != 0;
}
