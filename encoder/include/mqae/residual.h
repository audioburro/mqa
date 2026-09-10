/*
 * mqae/residual.h -- the residual stage, run backwards.
 *
 * The decoder's residual stage (mqa/residual_stage.h) turns one byte
 * stream into the P and Q the reconstruction filter consumes, through
 * four entropy decoders (two per channel) sharing a range coder, and a
 * lifting predictor per channel. This is that path in reverse:
 *
 *   * the predictor is a lifting scheme, so the pair of symbols that
 *     produces a given residual pair follows from it: two lifting steps
 *     undone, then a two-by-two solve, since the symbols enter through a
 *     fixed mixing matrix;
 *   * the symbols go to the entropy encoder (mqae/entropy.h), which says
 *     exactly what the decoder will get back;
 *   * the predictor is then run forwards over what the decoder will
 *     really see, so the caller learns the residuals it will really get.
 *
 * The coder restarts every 4096 carrier samples. A block's bytes are
 * built by a backward pass over the block, so nothing can be written
 * until the block is complete, and a block is what the encoder buffers.
 *
 * The predictor's inverse also has to run backwards in time. Solved in
 * sample order it amplifies each rounding by about a factor of three per
 * step and diverges within a few dozen; solved from the block's end it
 * contracts, and a short overhang past the end lets it forget where it
 * started. So a whole block's symbols are solved for first and then
 * coded in the order the decoder reads them.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQAE_RESIDUAL_H
#define MQAE_RESIDUAL_H

#include <stddef.h>
#include <stdint.h>
#include "mqae/entropy.h"
#include "mqa/predictor.h"
#include "mqa/carrier.h"

#define MQAE_RESIDUAL_GROUP  32     /* carrier samples a group covers  */
#define MQAE_RESIDUAL_BLOCK  4096   /* samples between coder restarts  */

struct mqae_residual {
	struct mqae_coder coder;
	struct mqae_entropy rec[2][2];        /* [channel][record]        */
	struct mqa_predictor pred[2];
	struct mqa_gain_params record;        /* the stage's gain record  */
	unsigned scale_index;
	uint32_t counter;                     /* carrier samples so far   */
	int32_t level_lo, level_hi;           /* the class's step bounds  */
	unsigned long clipped;
	double symbol_energy, symbol_error;   /* what the coder itself cost */
};

/*
 * Set up for a stream with this scale index and carrier class. The
 * class fixes the step bounds the decoder will impose, so the encoder
 * has to know it to stay inside them.
 */
int mqae_residual_init(struct mqae_residual *r, unsigned scale_index,
		       const struct mqa_carrier_class *cls);
void mqae_residual_free(struct mqae_residual *r);

/*
 * One block. `p` and `q` hold `samples + 2 * MQAE_RESIDUAL_WARM`
 * residual targets per channel, the block's own, and an overhang the
 * backward solve starts from and discards. `got_p` and `got_q` receive
 * the `samples` residuals the decoder will really produce, and the
 * block's bytes come back through `bytes`/`len`.
 *
 * `samples` must be a multiple of MQAE_RESIDUAL_GROUP.
 */
const uint8_t *mqae_residual_block(struct mqae_residual *r,
				   const int32_t *p, const int32_t *q, unsigned samples,
				   int32_t *got_p, int32_t *got_q, size_t *len);

/* Pairs of overhang the backward solve needs past a block's end. */
#define MQAE_RESIDUAL_WARM 64

#endif
