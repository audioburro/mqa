/*
 * The alternative reconstruction kernel -- see include/mqa/recon2.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <string.h>
#include "mqa/recon2.h"
#include "mqa/reconstruct.h"

/* The kernel's coefficients are the part of the decoder's table the
 * short filter does not use: c[i] here is table entry 4 + i. */
#define COEFF(i) ((int32_t)mqa_recon_coeff_table[4 + (i)])

const int32_t mqa_recon2_shape_table[MQA_RECON2_SHAPES][3] = {
	{ 2402,    8, -614 },
	{ 2564,  484, -412 },
	{ 3067, 1898,  406 },
	{ 3307, 2964, 1300 },
};

/* High word of a 64-bit accumulation, as the decoder keeps it. */
static int32_t hi32(uint64_t acc)
{
	return (int32_t)(uint32_t)(acc >> 32);
}

static uint64_t mac(uint64_t acc, int32_t x, int32_t c)
{
	return acc + (uint64_t)((int64_t)x * c);
}

static void push(int32_t *hist, unsigned n, int32_t v)
{
	memmove(hist + 1, hist, (n - 1) * sizeof *hist);
	hist[0] = v;
}

/* Stage 1: the current sample at unit gain plus four filtered ones. */
static int32_t interpolate(const struct mqa_recon2_channel *c, int32_t x)
{
	uint64_t acc = (uint64_t)(uint32_t)x << 32;
	unsigned i;

	for (i = 0; i < 4; i++)
		acc = mac(acc, c->x[i], COEFF(i));
	return hi32(acc);
}

/* Stage 2: fourteen taps of the interpolator's own output. */
static int32_t shape(const struct mqa_recon2_channel *c, int32_t u)
{
	uint64_t acc = (uint64_t)((int64_t)u * COEFF(4));
	unsigned i;

	for (i = 0; i < 13; i++)
		acc = mac(acc, c->u[i], COEFF(5 + i));
	return hi32(acc);
}

void mqa_recon2_init(struct mqa_recon2_state *st, unsigned shape_select)
{
	memset(st, 0, sizeof *st);
	st->sign = -1;                            /* the first tap uses +1 */
	st->shape = shape_select < MQA_RECON2_SHAPES ? shape_select : 0;
}

static void prime_channel(struct mqa_recon2_channel *c, int32_t x, int32_t p, int scale)
{
	int32_t u;

	if (scale)
		x = (int32_t)((uint32_t)x * 3) >> 1;
	u = interpolate(c, x);
	push(c->x, 4, x);
	/* the warm-up runs the shaping filter too: its output takes the
	 * place of the output sample the tap would have produced */
	push(c->y, 3, shape(c, u));
	push(c->u, 13, u);
	push(c->z, 3, 0);
	push(c->p, 8, p);
}

void mqa_recon2_prime(struct mqa_recon2_state *st,
		      const int32_t *a, const int32_t *b,
		      const int32_t *p, const int32_t *q, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		prime_channel(&st->ch[0], a[i], p ? p[i] : 0, p == NULL);
		prime_channel(&st->ch[1], b[i], q ? q[i] : 0, p == NULL);
	}
}

void mqa_recon2_settle(struct mqa_recon2_state *st)
{
	unsigned c;

	for (c = 0; c < 2; c++) {
		memset(st->ch[c].y, 0, sizeof st->ch[c].y);
		memset(st->ch[c].z, 0, sizeof st->ch[c].z);
	}
}

static void tap_channel(struct mqa_recon2_channel *c, const struct mqa_gain_params *gp,
			const int32_t *m, int32_t sign, int32_t x, int32_t p, int32_t d[2])
{
	int32_t u = interpolate(c, x), v = shape(c, u);
	int32_t d0 = d[0], d1 = d[1], y1, y2, g, base, corr;
	uint64_t acc;
	unsigned i;

	/* the second output sample: the shaped carrier, corrected by the
	 * residual of eight taps back with an alternating sign */
	y2 = (int32_t)((uint32_t)d1 + (uint32_t)mqa_gain_step(gp,
		mqa_gain_input(gp, (int32_t)((uint32_t)sign * (uint32_t)c->p[7]
					     + (uint32_t)v - (uint32_t)d1))));

	/* the first output sample: the interpolated sample of eight taps
	 * back, less a filter over the outputs since and a short filter
	 * over the residues they left */
	acc = (uint64_t)((int64_t)y2 * COEFF(18));
	for (i = 0; i < 3; i++)
		acc = mac(acc, c->y[i], COEFF(19 + i));
	g = hi32(acc << 1);
	corr = 0;
	for (i = 0; i < 3; i++)
		corr = (int32_t)((uint32_t)corr + (uint32_t)c->z[i] * (uint32_t)m[i]);
	base = (int32_t)((uint32_t)c->u[7] - (uint32_t)g);
	y1 = (int32_t)((uint32_t)d0 + (uint32_t)mqa_gain_step(gp,
		mqa_gain_input(gp, (int32_t)((uint32_t)base - (uint32_t)d0 + (uint32_t)(corr >> 11)))));

	push(c->x, 4, x);
	push(c->z, 3, (int32_t)((uint32_t)base - (uint32_t)y1));
	push(c->y, 3, y2);
	push(c->u, 13, u);
	push(c->p, 8, p);
	d[0] = y1;
	d[1] = y2;
}

void mqa_recon2_group(struct mqa_recon2_state *st, const struct mqa_gain_params *gp,
		      const int32_t *a, const int32_t *b,
		      const int32_t *p, const int32_t *q,
		      int32_t *l, int32_t *r, unsigned n)
{
	const int32_t *m = mqa_recon2_shape_table[st->shape];
	unsigned i;

	for (i = 0; i < n; i++) {
		st->sign = -st->sign;
		tap_channel(&st->ch[0], gp, m, st->sign, a[i], p[i], l + 2 * i);
		tap_channel(&st->ch[1], gp, m, st->sign, b[i], q[i], r + 2 * i);
	}
}
