/*
 * mqa/render.c -- the renderer's polyphase interpolator, its dither and
 * its requantiser.
 *
 * Record selection follows the resampler's own map (resampler.h): for
 * input at twice the carrier rate the rows of mqa_resampler_group for
 * 2:1 and 4:1 hold the sixteen stream filters, and mqa_resampler_single
 * the generic ones.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/lcg.h"
#include "mqa/render.h"

/* The shaper's taps, Q24. At 4x the last two apply to its own outputs. */
static const int32_t shaping_2x[5] = { -47138474, 62756143, -47154415, 19670274, -3604311 };
static const int32_t shaping_4x[8] = { -35998940, 37275625, -13408452, -3191330, 685015, 759498, 10294885, -7743830 };

#define REQUANTISE_SEED 0x2346u

/* 5 * (rate code & 7) + selection, for input at twice the base rate */
static const struct mqa_resampler_spec *choose(unsigned ratio, int signalled, unsigned filter)
{
	unsigned pick = ratio == 4 ? 3 : 2;

	if (ratio == 1)
		return &mqa_resampler_identity;
	if (signalled)
		return mqa_resampler_group[5 + pick][filter & 15];
	return mqa_resampler_single[5 + pick];
}

void mqa_render_init(struct mqa_render *r, unsigned ratio)
{
	memset(r, 0, sizeof *r);
	r->ratio = ratio == 2 || ratio == 4 ? ratio : 1;
	mqa_crc24_init(&r->crc);
	r->lfsr = 1;
	r->lcg = 1;
	r->spec = choose(r->ratio, 0, 0);
	if (r->ratio == 4) {
		r->shaping = shaping_4x;
		r->taps = 8;
		r->errors = 6;
		r->shift = 10;
	} else {
		r->shaping = shaping_2x;
		r->taps = 5;
		r->errors = 5;
		r->shift = 8;
	}
}

void mqa_render_set_stream(struct mqa_render *r, int signalled, unsigned filter)
{
	if (signalled && !r->signalled) {
		memset(r->ring, 0, sizeof r->ring);
		r->head = 0;
		r->lfsr2 = REQUANTISE_SEED;
		r->lcg2 = REQUANTISE_SEED;
	}
	r->signalled = signalled;
	r->filter = filter & 15;
	r->spec = choose(r->ratio, signalled, r->filter);
}

void mqa_render_set_requantise(struct mqa_render *r, int on)
{
	r->requantise = on;
}

/* One frame through the shaper: `s` are the samples, replaced in place. */
static void requantise(struct mqa_render *r, int32_t s[2])
{
	int64_t acc[2] = { 0, 0 };
	int32_t t[2], e[2], d = (int32_t)r->lcg2 >> 8;
	unsigned k, ch;

	for (k = 0; k < r->taps; k++) {
		const int32_t *w = r->ring[(r->head + k) & 15];

		acc[0] += (int64_t)r->shaping[k] * w[0];
		acc[1] += (int64_t)r->shaping[k] * w[1];
	}
	for (ch = 0; ch < 2; ch++) {
		int32_t dither = (int32_t)(r->lfsr2 + (ch ? -(uint32_t)d : (uint32_t)d));
		int32_t p;
		int64_t sum;

		t[ch] = (int32_t)((uint32_t)(int32_t)(acc[ch] >> 32) << 8);
		p = (int32_t)((uint32_t)dither + (uint32_t)t[ch]);
		sum = ((int64_t)p << 8) + ((int64_t)s[ch] << (32 - r->shift));
		s[ch] = (int32_t)((uint32_t)(int32_t)(sum >> 32) << r->shift);
		e[ch] = (int32_t)((uint32_t)dither - ((uint32_t)sum >> 8));
	}
	r->head = (r->head - 1) & 15;
	r->ring[r->head][0] = e[0];
	r->ring[r->head][1] = e[1];
	r->ring[(r->head + r->errors) & 15][0] = t[0];
	r->ring[(r->head + r->errors) & 15][1] = t[1];
	r->lfsr2 = mqa_crc24_update(&r->crc, r->lfsr2, 0);
	r->lcg2 = mqa_nr_lcg_step(r->lcg2);
}

void mqa_render_run(struct mqa_render *r, const int32_t *l, const int32_t *rr, size_t n,
		    int32_t *ol, int32_t *orr)
{
	const struct mqa_resampler_spec *spec = r->spec;
	unsigned taps = spec->taps, L = r->ratio, p, k;
	int shape = r->requantise && r->signalled;
	size_t i;

	if (L == 1) {
		memcpy(ol, l, n * sizeof *ol);
		memcpy(orr, rr, n * sizeof *orr);
		return;
	}
	for (i = 0; i < n; i++) {
		memmove(r->hist + 2, r->hist, 2 * (MQA_RENDER_MAX_TAPS - 1) * sizeof *r->hist);
		r->hist[0] = l[i];
		r->hist[1] = rr[i];
		for (p = 0; p < L; p++) {
			const int32_t *row = spec->table + p * taps;
			int64_t acc_l = 0, acc_r = 0;
			int32_t d = (int32_t)r->lcg >> 8, s[2];

			for (k = 0; k < taps; k++) {
				acc_l += (int64_t)row[k] * r->hist[2 * k];
				acc_r += (int64_t)row[k] * r->hist[2 * k + 1];
			}
			s[0] = (int32_t)((acc_l + (int32_t)r->lfsr + d) >> 24);
			s[1] = (int32_t)((acc_r + (int32_t)r->lfsr - d) >> 24);
			r->lfsr = mqa_crc24_update(&r->crc, r->lfsr, 0);
			r->lcg = mqa_nr_lcg_step(r->lcg);
			if (shape)
				requantise(r, s);
			*ol++ = s[0];
			*orr++ = s[1];
		}
	}
}
