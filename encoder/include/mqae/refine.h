/*
 * mqae/refine.h -- the carrier refinement, run backwards.
 *
 * The refinement is the channel that undoes what the data channel did.
 * Giving the low byte of every sample away costs the carrier about eight
 * bits, and the conditioner fills the hole with shaped dither; the
 * refinement then tells the decoder, sample by sample, how far the
 * carrier it holds is from the one the encoder meant.
 *
 * The decoder's side (mqa/refine.h) forms a prediction from the sample,
 * a bias and a 20-tap filter over the corrections so far, quantised
 * around a dither draw, then reads a symbol from a small range coder
 * saying how many steps to come back. An encoder that knows the sample
 * it wants picks the symbol whose step lands nearest and codes it;
 * everything else is the decoder's own arithmetic, run forwards.
 *
 * The coder is encoded the same way as the residual path's
 * (mqae/coder.h): forwards to choose the symbols, backwards to build the
 * value they came from, shedding a byte wherever the decoder pulled one.
 * Its state stays in [16, 4096), a quotient of a value renormalised to at
 * least sixteen radices, which is what determines where the bytes fall.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQAE_REFINE_H
#define MQAE_REFINE_H

#include <stddef.h>
#include <stdint.h>
#include "mqa/refine.h"
#include "mqa/lcg.h"

struct mqae_refine_op {
	uint32_t radix, sym;
};

struct mqae_refine {
	struct mqa_refine st;          /* the decoder's own state              */

	struct mqae_refine_op *op;     /* the block, for the backward pass     */
	size_t n, cap;
	uint8_t *bytes;
	size_t nbytes, bytes_cap;
	int failed;
	const char *why;

	/* what it managed: the correction wanted against what was carried */
	double want_energy, error;
	unsigned long clipped;         /* corrections the radix could not reach */
};

/*
 * Set up as a decoder's packet start does: `gain` and `param` as
 * mqa_refine_setup takes them, `set` the coefficient set, `salt` the
 * dither salt, and `step` the one the parameter record announces.
 */
int mqae_refine_init(struct mqae_refine *r, int32_t gain, int32_t param,
		     unsigned set, uint32_t salt, int32_t step);
void mqae_refine_free(struct mqae_refine *r);

/* Start a block (at a multiple of 4096 samples). */
void mqae_refine_block_start(struct mqae_refine *r);

/*
 * One group: `a`/`b` are the carrier as the conditioner left it, scaled
 * and corrected in place exactly as a decoder will; `want_a`/`want_b`
 * are the carrier the encoder is aiming at, on the scale the refinement
 * produces.
 */
void mqae_refine_group(struct mqae_refine *r, int32_t *a, int32_t *b,
		       const double *want_a, const double *want_b, unsigned count);

/* The block's bytes, for the data channel's type-2 messages. */
const uint8_t *mqae_refine_block_end(struct mqae_refine *r, size_t *len);

#endif
