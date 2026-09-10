/*
 * mqa/refine.h -- carrier refinement: the correction coded in the
 * auxiliary ring, applied to each group's carrier samples in place.
 *
 * Before a group is reconstructed its 32 carrier samples per channel are
 * scaled by the stage's gain and then corrected. Per sample a small range
 * coder reads a symbol from the auxiliary ring (the type-2 message
 * payloads, stream.h), and the sample becomes a dithered quantiser
 * reconstruction less that many steps. The correction deltas feed a
 * 20-tap FIR predictor per channel whose output enters the next
 * prediction.
 *
 * Section 6 of docs/mqa-stage1-spec.md specifies the parameters (6.1),
 * the group and block structure (6.2), the coder (6.3), the per-sample
 * arithmetic (6.4) and the predictor (6.5); the coefficient sets are in
 * A.4.
 *
 * The quantiser parameters are recomputed whenever a parameter record or
 * sync message announces a new step (mqa_refine_set_step), with the same
 * 64-bit division the reference uses. A step of 0 disables the
 * correction; the scaling still happens. mqa_refine_predictor_push is
 * exposed because an encoder choosing corrections has to keep the same
 * history.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_REFINE_H
#define MQA_DECODE_REFINE_H

#include <stdint.h>
#include "mqa/entropy.h"    /* struct mqa_byte_ring */

#define MQA_REFINE_HISTORY 50
#define MQA_REFINE_TAPS    20

struct mqa_refine_channel {
	int phase;                    /* toggles every sample; predictor runs at 0 */
	unsigned hist_pos;            /* next delta pair goes at hist_pos - 2      */
	const int32_t *coef;          /* the 2 * taps + 2 FIR coefficients         */
	unsigned taps;                /* FIR pair count (9 for the default set)    */
	int32_t acc;                  /* prediction for the next sample (Q11)      */
	int32_t last;                 /* the previous delta / shifted prediction   */
	int32_t history[MQA_REFINE_HISTORY];
};

struct mqa_refine {
	int pending;                  /* a new step was announced                  */
	int shift;                    /* coarse symbol width in bits               */
	int32_t param;                /* stream parameter feeding the bias         */
	int32_t gain;                 /* Q28 sample gain                           */
	int32_t step;                 /* quantiser step (0: refinement disabled)   */
	int32_t recip;                /* -(2^(32+k) / (2 step)) mod 2^32           */
	unsigned k;                   /* floor(log2 step)                          */
	int32_t bias;                 /* prediction bias, and it shifted:          */
	int32_t bias_shifted;
	int32_t threshold;            /* selects the radix per sample              */
	uint32_t value;               /* range coder value register                */
	struct mqa_byte_ring *ring;   /* the auxiliary ring                        */
	uint32_t counter;             /* samples seen; blocks of 4096              */
	uint32_t lcg[2];              /* dither LCG pair                           */
	uint32_t salt;                /* reseed salt (0: no dither)                */
	int32_t seek;                 /* ring read position to adopt, or -1        */
	int32_t step_request;         /* the announced step                        */
	uint32_t radix_a, recip_a;    /* coder radices and their reciprocals       */
	uint32_t radix_b, recip_b;
	struct mqa_refine_channel ch[2];
};

extern const int32_t mqa_refine_coefs_default[MQA_REFINE_TAPS];

/* The four coefficient sets a stream can select (pairs, shift, taps). */
struct mqa_refine_coef_set {
	unsigned taps;
	int shift;
	int32_t coef[MQA_REFINE_TAPS];
};
#define MQA_REFINE_COEF_SETS 4
extern const struct mqa_refine_coef_set mqa_refine_coef_sets[MQA_REFINE_COEF_SETS];

/*
 * The stream's gain for this stage, from the packet's level word (the
 * reference computes it as a fixed-point third-of-a-power: see refine.c).
 */
int32_t mqa_refine_gain_from_level(int32_t x);

/* (Re)start the stage from the stream parameters at a packet start. */
void mqa_refine_setup(struct mqa_refine *r, int32_t gain, int32_t param,
		      unsigned set_index, uint32_t salt);

/* Adopt a newly announced step: recompute the quantiser parameters. */
void mqa_refine_set_step(struct mqa_refine *r, int32_t step);

/* Refine one group of `count` samples per channel in place. */
void mqa_refine_group(struct mqa_refine *r, int32_t *a, int32_t *b, unsigned count);

/*
 * One correction delta into a channel's 20-tap predictor. It is the
 * decoder's own step, exposed because an encoder choosing corrections
 * has to keep the identical history: the prediction it subtracts is
 * what makes the next sample's quantiser land where it does.
 */
void mqa_refine_predictor_push(struct mqa_refine_channel *c, int32_t delta);

#endif
