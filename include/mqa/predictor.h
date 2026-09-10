/*
 * mqa/predictor.h -- the P/Q residual predictor.
 *
 * The reconstruction filter needs one residual per channel per tap: P
 * for the left channel, Q for the right. They are not read from the
 * stream. Each channel's two entropy decoders (entropy.h) produce a
 * 16-symbol record each, and this predictor turns the two records into
 * 32 residuals with the same two-step lifting butterfly as the filter,
 * driven by its own pair of dither generators.
 *
 * Section 7.7 of docs/mqa-stage1-spec.md specifies the sixteen
 * iterations and the three fixed mixing constants.
 *
 * The state carried between calls is the previous symbol pair and the
 * previous lifted pair. The second of those is `y2h`, the second output
 * minus half the first, because that is what the reference carries; it
 * is kept as is rather than as the more obvious output value. The
 * decoder draws a whole group's sixteen dither pairs before running
 * however many symbols the call asks for, and so does this.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_PREDICTOR_H
#define MQA_DECODE_PREDICTOR_H

#include <stdint.h>
#include "mqa/lcg.h"
#include "mqa/lifting.h"

#define MQA_PREDICTOR_SYMBOLS   16   /* symbols consumed per record per call */
#define MQA_PREDICTOR_OUTPUTS   32   /* residuals produced per call          */

struct mqa_predictor {
	struct mqa_dither noise[2];   /* private dither streams (dx, dy) */
	int32_t sym_prev[2];          /* last iteration's symbols (wa, wb)    */
	int32_t y1_prev;              /* last iteration's first lifted value  */
	int32_t y2h_prev;             /* last iteration's second value minus y1>>1 */
	struct mqa_gain_params gain;
};

/* Seed the two noise streams; history starts at zero. */
void mqa_predictor_init(struct mqa_predictor *p, uint32_t seed_x, uint32_t seed_y,
			const struct mqa_gain_params *gain);

/*
 * Produce 32 residuals from two 16-symbol records (rec_a, rec_b), which
 * are the outputs of two consecutive entropy-decoder calls.
 */
/* Run `count` symbols (16 for a full group, fewer at a packet's end),
 * producing 2 * count residuals. */
void mqa_predictor_run(struct mqa_predictor *p,
		       const int32_t rec_a[MQA_PREDICTOR_SYMBOLS],
		       const int32_t rec_b[MQA_PREDICTOR_SYMBOLS],
		       int32_t out[MQA_PREDICTOR_OUTPUTS], unsigned count);

#endif
