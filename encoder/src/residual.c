/*
 * The residual stage run backwards -- see mqae/residual.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "mqae/residual.h"
#include "mqa/residual_stage.h"
#include "mqa/lcg.h"

/* The predictor's mixing constants, as fractions (mqa/predictor.c). */
#define K0 0x51147576
#define K1 0x6c1b4748
#define K2 0x1b06d1d2

/* The salts the stage seeds each channel's block from. */
static const uint32_t channel_salt[2] = { 0xa1e24bbau, 0xa1e24cbau };

static void decoder_initial(struct mqa_entropy_decoder *d, uint32_t scale2)
{
	static const uint16_t decay[MQA_ENTROPY_TAPS] = { 0xf000, 0xe100, 0xd2f0, 0xc5c1 };

	memset(d, 0, sizeof *d);
	d->level = 0xe6;
	d->level_max = 0xe6;
	d->level_min = 0x39;
	d->level_rate = 0x108f;
	d->variance = 0x10000000;
	d->scale2 = scale2;
	d->head = 4;
	memcpy(d->decay, decay, sizeof d->decay);
}

int mqae_residual_init(struct mqae_residual *r, unsigned scale_index,
		       const struct mqa_carrier_class *cls)
{
	int32_t base;
	unsigned c, i;

	memset(r, 0, sizeof *r);
	if (mqae_coder_init(&r->coder) < 0)
		return -1;
	r->scale_index = scale_index;
	mqa_gain_from_scale(&r->record, mqa_residual_scale_table[scale_index & 63]);
	/* the class fixes the step bounds; nothing here asks for the spread */
	base = mqa_residual_class_level(cls, 0);
	r->level_lo = base;
	r->level_hi = base;
	for (c = 0; c < 2; c++) {
		for (i = 0; i < 2; i++) {
			mqae_entropy_attach(&r->rec[c][i], &r->coder);
			decoder_initial(&r->rec[c][i].d, r->record.scale);
			r->rec[c][i].d.gain = r->record;
		}
		mqa_predictor_init(&r->pred[c], 0, 0, &r->record);
	}
	return 0;
}

void mqae_residual_free(struct mqae_residual *r)
{
	mqae_coder_free(&r->coder);
}

/*
 * The variance index a block should open at.
 *
 * The decoder starts every block by reading a field that sets the
 * record's variance, and the variance is what scales the quantised
 * magnitudes into symbols: the reach of a symbol is about eight times
 * it, and a symbol costs least when it needs the smallest lattice
 * index. So the right opening value is a fraction of the size of the
 * symbols about to be coded, which the encoder knows, having already
 * solved for them.
 *
 * Three things bound the choice. The field's own scale is
 * table[low & 3] << (low >> 2) >> 8, a quarter-octave ladder; a decoder
 * treats a jump to twice the variance it already has as a stream fault,
 * so a block can only ever open a little above where the last one
 * ended; and it cannot go too low, because the variance is what opens
 * the interval between the two magnitudes: below about
 * 256 * scale / level that interval closes and no symbol is codeable
 * at all.
 */
static unsigned variance_index(int32_t want, int32_t have)
{
	unsigned low, best = 0;
	int32_t best_var = 0;

	if (want < 1)
		want = 1;
	for (low = 0; low <= 0x5b; low++) {
		int32_t var = (int32_t)((uint32_t)mqa_residual_level_table[low & 3] << (low >> 2)) >> 8;

		if (var >= 2 * have || var > want)
			continue;
		if (var > best_var) {
			best_var = var;
			best = low;
		}
	}
	return best;
}

static void block_start(struct mqae_residual *r, const int32_t *const *wa,
			const int32_t *const *wb, unsigned pairs)
{
	uint32_t block = r->counter >> 12;
	int32_t rate = mqa_residual_rate_table[0];
	unsigned c, i;

	mqae_coder_start(&r->coder);
	/* the stage's own three bits: no spread, the first adaptation rate */
	mqae_coder_normalize(&r->coder);
	mqae_coder_field(&r->coder, 3, 0);
	for (c = 0; c < 2; c++) {
		uint32_t s = channel_salt[c] + block;

		r->pred[c].noise[0].state = mqa_nr_lcg_step(s * s);
		r->pred[c].noise[1].state = mqa_nr_lcg_step(r->pred[c].noise[0].state);
		for (i = 0; i < 2; i++) {
			uint32_t seed = s + 32 * (i + 1);

			const int32_t *sym = i == 0 ? wa[c] : wb[c];
			double sum = 0;
			unsigned k;
			unsigned low;

			for (k = 0; k < pairs; k++)
				sum += (double)sym[k] * sym[k];
			/* the floor: several times the point where the two
			 * magnitudes stop straddling anything */
			int32_t floor_v = 8 * 256 * (int32_t)r->record.scale /
					  (r->level_lo > 0 ? r->level_lo : 1);
			int32_t want = (int32_t)(sqrt(sum / (pairs ? pairs : 1)) / 2 + 1);

			if (want < floor_v)
				want = floor_v;
			low = variance_index(want, r->rec[c][i].d.variance);
			r->rec[c][i].d.rng = seed * seed;
			mqae_entropy_block_init(&r->rec[c][i], block, rate, 0, low);
			r->rec[c][i].d.level_max = i == 0 ? r->level_lo : r->level_hi;
			r->rec[c][i].d.level_min = (int32_t)((uint32_t)r->rec[c][i].d.level_max >> 2);
			if (r->counter == 0)
				r->rec[c][i].d.level = r->rec[c][i].d.level_max;
		}
	}
}

/* G(v): what a lifting step's gain does to a prediction term. */
static int32_t lift(const struct mqa_gain_params *gp, int32_t v)
{
	return mqa_gain_step(gp, mqa_gain_input(gp, v));
}

/* The v that makes lift(v) come out as close to `o` as it can. */
static int32_t unlift(const struct mqa_gain_params *gp, int32_t o)
{
	int32_t s = (int32_t)gp->scale;
	int32_t n = (int32_t)((o >= 0 ? (int64_t)o + s / 2 : (int64_t)o - s / 2) / s);

	return (int32_t)((int64_t)n * s);
}

/*
 * One channel's symbols for a whole block, solved backwards.
 *
 * Undoing the two lifting steps gives, for every pair, the prediction
 * terms h1 and h2 the predictor must have formed. Those say
 *
 *     h1[i] = wa[i-1] + k1*(wb[i] - y2h[i-1]) + k0*(wa[i] - y1[i-1])
 *     h2[i] = wb[i-1] + k2*(wa[i] - y1[i-1]) + k0*(wb[i] - y2h[i-1])
 *
 * which read backwards is a recursion for the earlier symbol pair from
 * the later one, with everything else known from the targets. Run that
 * way it contracts; run forwards it does not.
 */
static void invert(struct mqae_residual *r, unsigned c, const int32_t *want,
		   unsigned pairs, unsigned total, int32_t *wa, int32_t *wb)
{
	const struct mqa_gain_params *gp = &r->pred[c].gain;
	struct mqa_dither nx = r->pred[c].noise[0], ny = r->pred[c].noise[1];
	double k0 = (double)K0 / 4294967296.0;
	double k1 = (double)K1 / 4294967296.0;
	double k2 = (double)K2 / 4294967296.0;
	double *h1 = malloc(total * sizeof *h1), *h2 = malloc(total * sizeof *h2);
	double *y1 = malloc(total * sizeof *y1), *y2h = malloc(total * sizeof *y2h);
	double la, lb;
	unsigned i;

	if (!h1 || !h2 || !y1 || !y2h)
		goto out;
	for (i = 0; i < total; i++) {
		int32_t dx = mqa_dither_next(&nx), dy = mqa_dither_next(&ny);
		int32_t want_y1 = want[2 * i] + want[2 * i + 1];
		int32_t got_y1 = dx + lift(gp, unlift(gp, want_y1 - dx));

		y1[i] = got_y1;
		h1[i] = dx + unlift(gp, want_y1 - dx);
		h2[i] = dy - (got_y1 >> 1) + unlift(gp, want[2 * i + 1] - dy);
		y2h[i] = dy - (got_y1 >> 1)
		       + lift(gp, (int32_t)h2[i] - dy + (got_y1 >> 1));
	}
	/* the pair the recursion starts from: the block's overhang covers
	 * the few steps it takes for the guess to stop mattering */
	la = h1[total - 1];
	lb = h2[total - 1];
	for (i = total; i-- > 1; ) {
		double ea = la - y1[i - 1], eb = lb - y2h[i - 1];
		double pa = h1[i] - k1 * eb - k0 * ea;
		double pb = h2[i] - k2 * ea - k0 * eb;

		if (i - 1 < pairs) {
			wa[i - 1] = (int32_t)pa;
			wb[i - 1] = (int32_t)pb;
		}
		la = pa;
		lb = pb;
	}
out:
	free(h1);
	free(h2);
	free(y1);
	free(y2h);
}

const uint8_t *mqae_residual_block(struct mqae_residual *r,
				   const int32_t *p, const int32_t *q, unsigned samples,
				   int32_t *got_p, int32_t *got_q, size_t *len)
{
	unsigned pairs = samples / 2, total = pairs + MQAE_RESIDUAL_WARM;
	int32_t *wa[2], *wb[2];
	unsigned c, g;

	for (c = 0; c < 2; c++) {
		wa[c] = malloc(total * sizeof *wa[c]);
		wb[c] = malloc(total * sizeof *wb[c]);
		if (!wa[c] || !wb[c]) {
			r->coder.failed = 1;
			r->coder.why = "out of memory";
		}
	}
	if (!r->coder.failed) {
		/* the symbols first: the block's header is chosen from them */
		for (c = 0; c < 2; c++)
			invert(r, c, c == 0 ? p : q, pairs, total, wa[c], wb[c]);
		block_start(r, (const int32_t *const *)wa, (const int32_t *const *)wb, pairs);
	}

	for (g = 0; g * MQAE_RESIDUAL_GROUP < samples && !r->coder.failed; g++) {
		unsigned at = g * MQAE_RESIDUAL_GROUP / 2;
		int32_t ga[MQA_PREDICTOR_SYMBOLS], gb[MQA_PREDICTOR_SYMBOLS];

		for (c = 0; c < 2; c++) {
			/* a channel's two records go into the coder one after
			 * the other, which is the order the decoder reads them */
			mqae_entropy_encode(&r->rec[c][0], wa[c] + at, ga, MQA_PREDICTOR_SYMBOLS);
			mqae_entropy_encode(&r->rec[c][1], wb[c] + at, gb, MQA_PREDICTOR_SYMBOLS);
			{
				unsigned k;

				/* what the symbols cost, which is the coder's
				 * own accuracy rather than the format's */
				for (k = 0; k < MQA_PREDICTOR_SYMBOLS; k++) {
					double da = (double)ga[k] - wa[c][at + k];
					double db = (double)gb[k] - wb[c][at + k];

					r->symbol_energy += (double)wa[c][at + k] * wa[c][at + k]
							  + (double)wb[c][at + k] * wb[c][at + k];
					r->symbol_error += da * da + db * db;
				}
			}
			mqa_predictor_run(&r->pred[c], ga, gb,
					  (c == 0 ? got_p : got_q) + g * MQAE_RESIDUAL_GROUP,
					  MQA_PREDICTOR_SYMBOLS);
			r->clipped += r->rec[c][0].clipped + r->rec[c][1].clipped;
			r->rec[c][0].clipped = r->rec[c][1].clipped = 0;
		}
		r->counter += MQAE_RESIDUAL_GROUP;
	}
	for (c = 0; c < 2; c++) {
		free(wa[c]);
		free(wb[c]);
	}
	return mqae_coder_finish(&r->coder, len);
}
