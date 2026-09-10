/*
 * P/Q residual predictor -- see include/mqa/predictor.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mqa/predictor.h"

/* Q31 accumulator coefficients, fixed in the decoder. */
#define K0 0x51147576   /* ~0.634 */
#define K1 0x6c1b4748   /* ~0.845 */
#define K2 0x1b06d1d2   /* ~0.211 */

void mqa_predictor_init(struct mqa_predictor *p, uint32_t seed_x, uint32_t seed_y,
			const struct mqa_gain_params *gain)
{
	p->noise[0].state = seed_x;
	p->noise[0].scale = gain->scale;
	p->noise[1].state = seed_y;
	p->noise[1].scale = gain->scale;
	p->sym_prev[0] = 0;
	p->sym_prev[1] = 0;
	p->y1_prev = 0;
	p->y2h_prev = 0;
	p->gain = *gain;
}

/* High word of a two-term 64-bit signed accumulation. */
static int32_t acc_hi(int32_t x0, int32_t k0, int32_t x1, int32_t k1)
{
	return (int32_t)(((int64_t)x0 * k0 + (int64_t)x1 * k1) >> 32);
}

void mqa_predictor_run(struct mqa_predictor *p,
		       const int32_t rec_a[MQA_PREDICTOR_SYMBOLS],
		       const int32_t rec_b[MQA_PREDICTOR_SYMBOLS],
		       int32_t out[MQA_PREDICTOR_OUTPUTS], unsigned count)
{
	const struct mqa_gain_params *gp = &p->gain;
	int32_t dx[MQA_PREDICTOR_SYMBOLS], dy[MQA_PREDICTOR_SYMBOLS];
	unsigned i;

	/* The decoder draws a whole group's 16 noise pairs up front however
	 * many symbols it then runs (a packet's last group may be shorter). */
	for (i = 0; i < MQA_PREDICTOR_SYMBOLS; i++) {
		dx[i] = mqa_dither_next(&p->noise[0]);
		dy[i] = mqa_dither_next(&p->noise[1]);
	}

	for (i = 0; i < count; i++) {
		int32_t ea = rec_a[i] - p->y1_prev;
		int32_t eb = rec_b[i] - p->y2h_prev;
		int32_t h1, h2, y1, y2h, y2;

		h1 = p->sym_prev[0] + acc_hi(eb, K1, ea, K0);
		y1 = dx[i] + mqa_gain_step(gp, mqa_gain_input(gp, h1 - dx[i]));

		h2 = p->sym_prev[1] + acc_hi(ea, K2, eb, K0);
		y2h = dy[i] - (y1 >> 1)
		    + mqa_gain_step(gp, mqa_gain_input(gp, h2 - dy[i] + (y1 >> 1)));

		y2 = y2h + (y1 >> 1);
		out[2 * i]     = y1 - y2;
		out[2 * i + 1] = y2;

		p->sym_prev[0] = rec_a[i];
		p->sym_prev[1] = rec_b[i];
		p->y1_prev = y1;
		p->y2h_prev = y2h;
	}
}
