/*
 * mqa/reconstruct.h -- the short reconstruction filter (variant 1).
 *
 * The first unfold: each carrier sample pair and residual pair becomes
 * two output samples per channel. A tap splits the carrier sample into a
 * prediction and a correction using the residual, mixes them with the
 * previous two taps' history through two 4-coefficient accumulators, and
 * lifts the tap's dither pair into the output pair through the shared
 * gain step (lifting.h).
 *
 * Section 8.2 of docs/mqa-stage1-spec.md specifies the tap exactly; 8.1
 * the gain step; 8.4 the warm-up a restart runs. The coefficients are
 * the first four entries of mqa_recon_coeff_table (A.7); the other
 * twenty-two belong to the alternative kernel (recon2.h).
 *
 * The accumulators' differences are formed in 64 bits, not wrapped to 32
 * first, because the reference keeps them as register pairs. Everything
 * else wraps at 32 bits where the reference does.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_RECONSTRUCT_H
#define MQA_DECODE_RECONSTRUCT_H

#include <stdint.h>
#include "mqa/lifting.h"

/* The decoder's 26-entry coefficient table, as Q31 fractions. */
extern const uint32_t mqa_recon_coeff_table[26];

struct mqa_recon_coeffs {
	int64_t c[4];   /* {c0, c1, c2, c3}; low 32 bits Q31, high 32 bits normally 0 */
};

/* Per-channel filter history: the last two taps' prediction, correction
 * and outputs. Zero-initialise for a fresh stream. */
struct mqa_recon_channel {
	int32_t pred[2];   /* n at tap-1, tap-2   */
	int32_t corr[2];   /* t at tap-1, tap-2   */
	int32_t out0[2];   /* out0 at tap-1, tap-2 */
	int32_t out1[2];   /* out1 at tap-1, tap-2 */
};

struct mqa_recon_state {
	struct mqa_recon_channel ch[2];  /* 0 = left, 1 = right */
	int32_t sign;                    /* +1 or -1, flipped before every tap */
};

/* Initialise for a fresh stream (zero history, sign such that the first
 * tap uses s = -1, matching the reference decoder). */
void mqa_recon_init(struct mqa_recon_state *st);

/* Build the default coefficient set (table entries 0..3, zero high words). */
void mqa_recon_coeffs_default(struct mqa_recon_coeffs *c);

/*
 * Run one tap for both channels.
 *   carrier[2]   : this tap's carrier samples (L, R)
 *   residual[2]  : this tap's entropy-decoded residuals (P, Q)
 *   dither_l[2]  : in: this tap's two left dither samples; out: left output pair
 *   dither_r[2]  : likewise for the right channel
 */
/*
 * The linear half of a tap, used to warm the filter from stored carrier
 * pairs when a stream is joined: both prediction and correction are the
 * pair itself and the outputs are the ideal predictions
 *     out0 = n[t-2] + 2 h1,  out1 = t[t-2] + 2 h2
 * with no dither and no gain step. The sign is not advanced.
 */
void mqa_recon_replay_tap(struct mqa_recon_state *st,
			  const struct mqa_recon_coeffs *coeffs,
			  const int32_t pair[2]);

void mqa_recon_tap(struct mqa_recon_state *st,
		   const struct mqa_gain_params *gp,
		   const struct mqa_recon_coeffs *coeffs,
		   const int32_t carrier[2], const int32_t residual[2],
		   int32_t dither_l[2], int32_t dither_r[2]);

#endif
