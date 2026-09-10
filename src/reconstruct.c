/*
 * MQA stage-1 reconstruction filter -- see include/mqa/reconstruct.h for
 * the algorithm description.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/reconstruct.h"

/*
 * Extracted from the decoder's data section: 26 Q31 fractions (all with
 * their low 16 bits clear, i.e. 16-bit precision). Only entries 0..3 have
 * been seen in use.
 */
const uint32_t mqa_recon_coeff_table[26] = {
	0x01540000u, 0x2ea50000u, 0x0bf00000u, 0x6b140000u, 0x53510000u,
	0x08520000u, 0xf9690000u, 0x005a0000u, 0xffce0000u, 0x011a0000u,
	0xfcc60000u, 0x06ee0000u, 0xf33c0000u, 0x15ed0000u, 0xd9e90000u,
	0x4e550000u, 0x73ff0000u, 0xbfe80000u, 0x1d900000u, 0xf6800000u,
	0x02550000u, 0xffb10000u, 0x47500000u, 0x2a100000u, 0x15f00000u,
	0xf8b00000u,
};

void mqa_recon_init(struct mqa_recon_state *st)
{
	memset(st, 0, sizeof *st);
	st->sign = 1;
}

void mqa_recon_coeffs_default(struct mqa_recon_coeffs *c)
{
	int i;

	for (i = 0; i < 4; i++)
		c->c[i] = (int64_t)(int32_t)mqa_recon_coeff_table[i];
}

/* Sum of two such products, high word only (carries from the low words
 * included, as the decoder's 64-bit adds do). */
static int32_t acc_hi(int64_t x0, int64_t c0, int64_t x1, int64_t c1)
{
	uint64_t lo = (uint64_t)(uint32_t)x0 * (uint32_t)c0
		    + (uint64_t)(uint32_t)x1 * (uint32_t)c1;
	uint32_t hi = (uint32_t)(lo >> 32)
		    + (uint32_t)(x0 >> 32) * (uint32_t)c0 + (uint32_t)x0 * (uint32_t)(c0 >> 32)
		    + (uint32_t)(x1 >> 32) * (uint32_t)c1 + (uint32_t)x1 * (uint32_t)(c1 >> 32);

	return (int32_t)hi;
}

static void recon_channel(struct mqa_recon_channel *ch,
			  const struct mqa_gain_params *gp,
			  const struct mqa_recon_coeffs *k,
			  int32_t sign, int32_t a, int32_t p, int32_t d[2])
{
	int32_t d0 = d[0], d1 = d[1];
	int32_t t, n, h, x1, x2, y1, y2, hsum, a2;

	/* 1. split the carrier sample into prediction and correction */
	t = (int32_t)((uint32_t)sign * (uint32_t)p + (uint32_t)(a >> 1));
	n = a - t;

	/*
	 * 2. the two accumulators over the last two taps' history. The
	 *    differences are formed in 64 bits, not wrapped to 32 first:
	 *    the decoder keeps them as (low, sign) register pairs, which
	 *    only matters once outputs grow past +-2^31.
	 */
	h  = acc_hi((int64_t)n - ch->out0[1], k->c[0],
		    (int64_t)ch->pred[0] - ch->out0[0], k->c[1]);
	hsum = h;
	h  = acc_hi((int64_t)t - ch->out1[1], k->c[2],
		    (int64_t)ch->corr[0] - ch->out1[0], k->c[3]);
	hsum += h;

	/* 3. lift the dither pair into the output pair */
	a2 = ch->pred[1] + ch->corr[1];              /* carrier sample two taps ago */
	x1 = mqa_gain_input(gp, a2 - d0 - d1 + 2 * hsum);
	y1 = d0 + d1 + mqa_gain_step(gp, x1);

	/* (a2 + 2*hsum) >> 1, but as the decoder evaluates it: the halving
	 * is done before the sum can wrap. */
	x2 = mqa_gain_input(gp, ch->corr[1] - d1 + (y1 >> 1)
				- ((a2 >> 1) + hsum) + 2 * h);
	y2 = d1 + mqa_gain_step(gp, x2);

	d[0] = y1 - y2;
	d[1] = y2;

	/* shift the history */
	ch->pred[1] = ch->pred[0]; ch->pred[0] = n;
	ch->corr[1] = ch->corr[0]; ch->corr[0] = t;
	ch->out0[1] = ch->out0[0]; ch->out0[0] = d[0];
	ch->out1[1] = ch->out1[0]; ch->out1[0] = d[1];
}

static void replay_channel(struct mqa_recon_channel *ch, const struct mqa_recon_coeffs *k, int32_t x)
{
	int32_t h1 = acc_hi((int64_t)x - ch->out0[1], k->c[0], (int64_t)ch->pred[0] - ch->out0[0], k->c[1]);
	int32_t h2 = acc_hi((int64_t)x - ch->out1[1], k->c[2], (int64_t)ch->corr[0] - ch->out1[0], k->c[3]);
	int32_t o0 = ch->pred[1] + 2 * h1, o1 = ch->corr[1] + 2 * h2;

	ch->pred[1] = ch->pred[0];
	ch->pred[0] = x;
	ch->corr[1] = ch->corr[0];
	ch->corr[0] = x;
	ch->out0[1] = ch->out0[0];
	ch->out0[0] = o0;
	ch->out1[1] = ch->out1[0];
	ch->out1[0] = o1;
}

void mqa_recon_replay_tap(struct mqa_recon_state *st,
			  const struct mqa_recon_coeffs *coeffs,
			  const int32_t pair[2])
{
	replay_channel(&st->ch[0], coeffs, pair[0]);
	replay_channel(&st->ch[1], coeffs, pair[1]);
}

void mqa_recon_tap(struct mqa_recon_state *st,
		   const struct mqa_gain_params *gp,
		   const struct mqa_recon_coeffs *coeffs,
		   const int32_t carrier[2], const int32_t residual[2],
		   int32_t dither_l[2], int32_t dither_r[2])
{
	st->sign = -st->sign;

	recon_channel(&st->ch[0], gp, coeffs, st->sign, carrier[0], residual[0], dither_l);
	recon_channel(&st->ch[1], gp, coeffs, st->sign, carrier[1], residual[1], dither_r);
}
