/*
 * Carrier refinement -- see include/mqa/refine.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/refine.h"
#include "mqa/lcg.h"

const int32_t mqa_refine_coefs_default[MQA_REFINE_TAPS] = {
	3190, 1617, -177, -230, -645, -1940, -1856, -474, 191, 139,
	425, 735, 503, 148, 42, -18, -135, -166, -95, -27,
};

const struct mqa_refine_coef_set mqa_refine_coef_sets[MQA_REFINE_COEF_SETS] = {
	{ 9, 3, { 2887, 956, -296, -202, -1247, -1975, -826, 129, 118, 369,
		  667, 405, 117, 62, -42, -140, -114, -76, -59, -24 } },
	{ 9, 3, { 3190, 1617, -177, -230, -645, -1940, -1856, -474, 191, 139,
		  425, 735, 503, 148, 42, -18, -135, -166, -95, -27 } },
	{ 3, 4, { 5282, 6571, 3875, -439, -2659, -2134, -822, -134, 0, 0,
		  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
	{ 3, 4, { 5144, 7493, 7151, 4495, 1625, 99, -180, -59, 0, 0,
		  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
};

static int32_t hi32(int64_t p) { return (int32_t)(p >> 32); }
static uint32_t uhi32(uint64_t p) { return (uint32_t)(p >> 32); }

int32_t mqa_refine_gain_from_level(int32_t x)
{
	uint32_t f = (uint32_t)x << 16;
	uint32_t third = (uint32_t)(((uint64_t)0x55555555u * f) >> 32);
	uint32_t sq = (uint32_t)(((uint64_t)third * f) >> 32);
	int32_t v = (int32_t)(third + 0x80000000u - (sq >> 2));

	return v >> (x >> 16);
}

void mqa_refine_setup(struct mqa_refine *r, int32_t gain, int32_t param,
		      unsigned set_index, uint32_t salt)
{
	const struct mqa_refine_coef_set *set = &mqa_refine_coef_sets[set_index & 3];
	unsigned c;

	r->gain = gain;
	r->param = param;
	r->salt = salt;
	r->pending = 0;
	r->shift = set->shift;
	r->step = 0;
	for (c = 0; c < 2; c++) {
		struct mqa_refine_channel *ch = &r->ch[c];

		ch->coef = set->coef;
		ch->taps = set->taps;
		ch->acc = 0;
		ch->last = 0;
		ch->phase = 0;
		memset(ch->history, 0, sizeof ch->history);
		ch->hist_pos = 32;
	}
}

void mqa_refine_set_step(struct mqa_refine *r, int32_t step)
{
	uint64_t num;
	uint32_t q;
	int32_t t;

	r->step = step;
	if (step < 0) {
		r->k = 31;
		num = (uint64_t)0x80000000u << 32;
	} else {
		unsigned k = 0;

		while ((uint32_t)step >> (k + 1))
			k++;
		r->k = k;
		num = (uint64_t)1 << (32 + k);
	}
	q = (uint32_t)(num / (uint64_t)(uint32_t)(step * 2));

	/* bias from gain and the stream parameter */
	t = -hi32((int64_t)-r->gain * (int64_t)(r->param << 11));
	r->bias = (t << 1) >> 1;
	r->bias_shifted = r->bias << r->shift;
	r->recip = (int32_t)-q;

	r->radix_a = (uint32_t)(hi32((int64_t)r->recip * (int64_t)~(t << 2)) >> r->k);
	r->threshold = (t << 1) - step * (int32_t)r->radix_a;
	/* (a zero radix cannot decode anything; the reference's division
	 * helper yields 0 for it and so do we) */
	r->recip_a = (r->radix_a ? 0x7fffffffu / r->radix_a : 0) + 1;
	r->radix_b = r->radix_a + 1;
	r->recip_b = (r->radix_b ? 0x7fffffffu / r->radix_b : 0) + 1;
}

static uint8_t ring_byte(struct mqa_byte_ring *ring)
{
	uint8_t b = ring->data[ring->cursor];

	ring->cursor = ring->cursor + 1 >= ring->size ? 0 : ring->cursor + 1;
	return b;
}

/*
 * Pull bytes until the value reaches 16 x radix. Four bytes is the most
 * that can ever help: by then everything the value started with has been
 * shifted out of it, so a fifth cannot make it any larger. On real data
 * the loop runs once or twice; the bound is what stops a ring of zeros
 * (a stream whose refinement payloads never arrived) spinning here.
 */
static uint32_t normalize(struct mqa_refine *r, uint32_t value, uint32_t radix)
{
	unsigned i;

	for (i = 0; i < 4 && radix << 4 > value; i++)
		value = (value << 8) + ring_byte(r->ring);
	return value;
}

/* Decode a symbol with the given radix; returns it, updates the value. */
static uint32_t decode(struct mqa_refine *r, uint32_t radix, uint32_t recip)
{
	uint32_t value = normalize(r, r->value, radix);
	uint32_t q = uhi32((uint64_t)(value << 1) * recip);

	r->value = q;
	return value - radix * q;
}

/* Feed one correction delta to the channel's FIR predictor. */
void mqa_refine_predictor_push(struct mqa_refine_channel *c, int32_t delta)
{
	const int32_t *coef = c->coef;
	const int32_t *d;
	int32_t acc, next;
	unsigned j;

	c->phase = !c->phase;
	if (c->phase) {
		/* odd sample: complete the prediction made for it */
		c->acc = coef[0] * delta + c->last;
		c->last = delta;
		return;
	}
	/* even sample: push the pair, evaluate the 20-tap FIR for the next
	 * sample and its 19-tap tail for the one after */
	c->hist_pos -= 2;
	c->history[c->hist_pos] = delta;
	c->history[c->hist_pos + 1] = c->last;
	d = &c->history[c->hist_pos];          /* d[j]: j-th most recent delta */
	acc = 0;
	next = 0;
	for (j = 0; j < 2 * c->taps + 2; j++) {
		acc += coef[j] * d[j];
		if (j < 2 * c->taps + 1)
			next += coef[j + 1] * d[j];
	}
	c->acc = acc;
	c->last = next;
}

static void refine_channel(struct mqa_refine *r, struct mqa_refine_channel *c,
			   int32_t *s, const int32_t *dither, unsigned count, unsigned coarse)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		int32_t x = s[i], d = dither[2 * i], pred, err, y;
		uint32_t radix, recip, sym;

		if (i < coarse) {
			/* a coarse shift-bit symbol first, straight from the value */
			uint32_t value = normalize(r, r->value, 1u << r->shift), rem;

			r->value = value >> r->shift;
			rem = value - (r->value << r->shift);
			err = hi32((int64_t)~((r->bias_shifted + x - d) << 1) * r->recip) >> r->k;
			pred = err * r->step + d;
			radix = r->radix_b;
			recip = r->recip_b;
			sym = r->radix_b * rem;
		} else {
			int32_t t = x + r->bias - (c->acc >> 11), resid;

			err = hi32((int64_t)~((t - d) << 1) * r->recip) >> r->k;
			pred = r->step * err + d;
			resid = t - pred;
			if (r->threshold <= resid) {
				radix = r->radix_a;
				recip = r->recip_a;
			} else {
				radix = r->radix_b;
				recip = r->recip_b;
			}
			sym = 0;
		}
		sym += decode(r, radix, recip);
		y = pred - r->step * (int32_t)sym;
		s[i] = y;
		mqa_refine_predictor_push(c, y - x);
	}
	/* fold the newest history to the top for the next group */
	memcpy(&c->history[32], &c->history[0], 18 * sizeof c->history[0]);
	c->hist_pos = 32;
	c->phase = 0;
}

void mqa_refine_group(struct mqa_refine *r, int32_t *a, int32_t *b, unsigned count)
{
	int32_t dither[2 * 64];
	unsigned i, coarse = 0, block_start;

	for (i = 0; i < count; i++) {
		a[i] = hi32((int64_t)(a[i] << 4) * r->gain);
		b[i] = hi32((int64_t)(b[i] << 4) * r->gain);
	}

	block_start = (r->counter & 0xfff) == 0;
	r->counter += count;
	if (r->pending && block_start) {
		r->pending = 0;
		if (r->seek >= 0)
			r->ring->cursor = (unsigned)r->seek;
		if (r->step_request != r->step)
			mqa_refine_set_step(r, r->step_request);
	}
	if (r->step == 0)
		return;

	if (block_start) {
		uint32_t block = r->counter >> 12;
		uint32_t seed = (block + r->salt) * (block + r->salt);

		r->lcg[0] = mqa_nr_lcg_step(seed);
		r->lcg[1] = mqa_nr_lcg_step(r->lcg[0]);
		r->value = 0;
		coarse = block ? (r->ch[0].taps + 1) * 2 : 0;
	}
	if (r->salt == 0) {
		memset(dither, 0, sizeof dither);
	} else {
		for (i = 0; i < count; i++) {
			dither[2 * i] = (int32_t)uhi32((uint64_t)r->lcg[0] * (uint32_t)r->step);
			dither[2 * i + 1] = (int32_t)uhi32((uint64_t)r->lcg[1] * (uint32_t)r->step);
			r->lcg[0] = mqa_lcg_step(r->lcg[0]);
			r->lcg[1] = mqa_lcg_step(r->lcg[1]);
		}
	}
	refine_channel(r, &r->ch[0], a, dither, count, coarse);
	refine_channel(r, &r->ch[1], b, dither + 1, count, coarse);
}
