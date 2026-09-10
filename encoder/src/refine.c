/*
 * The carrier refinement run backwards -- see mqae/refine.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include "mqae/refine.h"
#include "mqa/lcg.h"

/* The decoder's state bound: every value it decodes from is a quotient
 * of one renormalised to at least sixteen radices, so it is under this
 * however small the radix is. Passing it is what makes it pull a byte. */
#define STATE_MAX 4096u

static int32_t hi32(int64_t p) { return (int32_t)(p >> 32); }

int mqae_refine_init(struct mqae_refine *r, int32_t gain, int32_t param,
		     unsigned set, uint32_t salt, int32_t step)
{
	memset(r, 0, sizeof *r);
	r->cap = 8192;
	r->op = malloc(r->cap * sizeof *r->op);
	r->bytes_cap = 8192;
	r->bytes = malloc(r->bytes_cap);
	if (!r->op || !r->bytes)
		return -1;
	r->st.ch[0].coef = r->st.ch[1].coef = mqa_refine_coefs_default;
	r->st.lcg[0] = mqa_nr_lcg_step(0);
	r->st.lcg[1] = mqa_nr_lcg_step(r->st.lcg[0]);
	mqa_refine_setup(&r->st, gain, param, set, salt);
	mqa_refine_set_step(&r->st, step);
	return 0;
}

void mqae_refine_free(struct mqae_refine *r)
{
	free(r->op);
	free(r->bytes);
	memset(r, 0, sizeof *r);
}

void mqae_refine_block_start(struct mqae_refine *r)
{
	r->n = 0;
	r->nbytes = 0;
	r->failed = 0;
	r->why = NULL;
}

static void record(struct mqae_refine *r, uint32_t radix, uint32_t sym)
{
	if (r->failed)
		return;
	if (r->n == r->cap) {
		struct mqae_refine_op *grown = realloc(r->op, r->cap * 2 * sizeof *grown);

		if (!grown) {
			r->failed = 1;
			r->why = "out of memory";
			return;
		}
		r->op = grown;
		r->cap *= 2;
	}
	r->op[r->n].radix = radix;
	r->op[r->n].sym = sym;
	r->n++;
}

/*
 * One channel of a group. The decoder's own arithmetic, with the symbol
 * chosen rather than read: it is whichever number of steps back from
 * the prediction lands nearest the sample the encoder wants.
 */
static void channel(struct mqae_refine *r, struct mqa_refine_channel *c,
		    int32_t *s, const double *want, const int32_t *dither,
		    unsigned count, unsigned coarse)
{
	struct mqa_refine *st = &r->st;
	unsigned i;

	for (i = 0; i < count; i++) {
		int32_t x = s[i], d = dither[2 * i], pred, err, y;
		uint32_t radix, sym, top;
		double target = want[i];
		int64_t k;

		if (i < coarse) {
			err = hi32((int64_t)~((st->bias_shifted + x - d) << 1) * st->recip) >> st->k;
			pred = err * st->step + d;
			radix = st->radix_b;
			top = st->radix_b << st->shift;
		} else {
			int32_t t = x + st->bias - (c->acc >> 11), resid;

			err = hi32((int64_t)~((t - d) << 1) * st->recip) >> st->k;
			pred = st->step * err + d;
			resid = t - pred;
			radix = st->threshold <= resid ? st->radix_a : st->radix_b;
			top = radix;
		}

		/* how many steps back from the prediction the target is */
		k = st->step ? (int64_t)((pred - target) / st->step + 0.5) : 0;
		if (k < 0) {
			k = 0;
			r->clipped++;
		} else if (top == 0 || k >= (int64_t)top) {
			k = top ? (int64_t)top - 1 : 0;
			r->clipped++;
		}
		sym = (uint32_t)k;

		if (i < coarse) {
			/* the coarse form: a shift-bit symbol, then a fine one */
			record(r, 1u << st->shift, sym / st->radix_b);
			record(r, st->radix_b, sym % st->radix_b);
		} else {
			record(r, radix, sym);
		}

		y = pred - st->step * (int32_t)sym;
		r->want_energy += target * target;
		r->error += (target - y) * (target - y);
		s[i] = y;
		mqa_refine_predictor_push(c, y - x);
	}
	memcpy(&c->history[32], &c->history[0], 18 * sizeof c->history[0]);
	c->hist_pos = 32;
	c->phase = 0;
}

void mqae_refine_group(struct mqae_refine *r, int32_t *a, int32_t *b,
		       const double *want_a, const double *want_b, unsigned count)
{
	struct mqa_refine *st = &r->st;
	int32_t dither[2 * 64];
	unsigned i, coarse = 0, block_start;

	for (i = 0; i < count; i++) {
		a[i] = hi32((int64_t)(a[i] << 4) * st->gain);
		b[i] = hi32((int64_t)(b[i] << 4) * st->gain);
	}
	block_start = (st->counter & 0xfff) == 0;
	st->counter += count;
	if (st->step == 0)
		return;
	if (block_start) {
		uint32_t block = st->counter >> 12;
		uint32_t seed = (block + st->salt) * (block + st->salt);

		st->lcg[0] = mqa_nr_lcg_step(seed);
		st->lcg[1] = mqa_nr_lcg_step(st->lcg[0]);
		st->value = 0;
		coarse = block ? (st->ch[0].taps + 1) * 2 : 0;
	}
	if (st->salt == 0) {
		memset(dither, 0, sizeof dither);
	} else {
		for (i = 0; i < count; i++) {
			dither[2 * i] = (int32_t)(uint32_t)(((uint64_t)st->lcg[0] *
							     (uint32_t)st->step) >> 32);
			dither[2 * i + 1] = (int32_t)(uint32_t)(((uint64_t)st->lcg[1] *
								 (uint32_t)st->step) >> 32);
			st->lcg[0] = mqa_lcg_step(st->lcg[0]);
			st->lcg[1] = mqa_lcg_step(st->lcg[1]);
		}
	}
	channel(r, &st->ch[0], a, want_a, dither, count, coarse);
	channel(r, &st->ch[1], b, want_b, dither + 1, count, coarse);
}

/*
 * The backward pass. Every step of the decoder's little coder is
 * value = quotient * radix + symbol, so running the block from its last
 * step to its first builds the value it must have started from; a byte
 * is shed wherever that value would pass the state bound, which is
 * exactly where the decoder pulled one. The block starts from nothing,
 * so the last thing to come out is the value emptied entirely.
 */
const uint8_t *mqae_refine_block_end(struct mqae_refine *r, size_t *len)
{
	uint32_t value = 16;         /* the smallest state the decoder can hold */
	size_t i = r->n;

	r->nbytes = 0;
	while (i-- > 0 && !r->failed) {
		uint32_t v = value * r->op[i].radix + r->op[i].sym;
		unsigned shed = 0;

		while (v >= STATE_MAX || (i == 0 && v)) {
			if (r->nbytes == r->bytes_cap) {
				uint8_t *grown = realloc(r->bytes, r->bytes_cap * 2);

				if (!grown) {
					r->failed = 1;
					r->why = "out of memory";
					break;
				}
				r->bytes = grown;
				r->bytes_cap *= 2;
			}
			r->bytes[r->nbytes++] = (uint8_t)v;
			v >>= 8;
			if (++shed == 4)
				break;
		}
		if (i == 0 && v) {
			r->failed = 1;
			r->why = "the block would not start from an empty coder";
		}
		value = v;
	}
	for (i = 0; i < r->nbytes / 2; i++) {
		uint8_t t = r->bytes[i];

		r->bytes[i] = r->bytes[r->nbytes - 1 - i];
		r->bytes[r->nbytes - 1 - i] = t;
	}
	*len = r->nbytes;
	return r->bytes;
}
