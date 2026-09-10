/*
 * mqae/entropy.h -- choosing what the entropy decoder will produce.
 *
 * The decoder (mqa/entropy.h) turns a code word into a symbol through an
 * adaptive quantiser, a variance, a two-stage AR predictor and a
 * range-coded fine residual. As an encoder that is a choice of two
 * numbers per symbol:
 *
 *   * a step index k. The quantised magnitude is level * (dither + 256k),
 *     because the decoder snaps it so that its low byte equals the dither
 *     byte it just drew, and every code word in the interval the
 *     distribution assigns that magnitude decodes to the same k. Choosing
 *     k chooses the interval, and so the symbol's cost in bits;
 *
 *   * a residual r inside that interval, which the coder carries exactly.
 *     The symbol the decoder produces is
 *
 *         out = scale * r + dscaled + pv
 *
 *     where dscaled and pv depend on the dither and the predictor's
 *     history but not on k, so the reachable symbols are an arithmetic
 *     progression of step `scale` over the interval k selected.
 *
 * Encoding a symbol is finding the k whose interval reaches the target
 * and then the r that lands nearest. The error is at most half of
 * `scale`; that is the encoder's quantisation noise.
 *
 * The distribution's table runs out at magnitudes near 2^19, so what a
 * symbol can carry depends on the current step. `level` adapts on its
 * own, as in the decoder, because the encoder keeps the decoder's state
 * and runs its updates. The decoder's escape form (a code word below 32,
 * carrying a magnitude directly) is not written: the table reaches
 * everything within its range.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQAE_ENTROPY_H
#define MQAE_ENTROPY_H

#include <stdint.h>
#include "mqa/entropy.h"
#include "mqae/coder.h"
#include "mqa/residual_stage.h"

struct mqae_entropy {
	struct mqa_entropy_decoder d;   /* the decoder's state, kept in step */
	struct mqae_coder *coder;
	unsigned long clipped;          /* symbols the table could not reach */
};

/* Wire an encoder to a coder; the state is the caller's to set up the
 * same way the decoder's is (level bounds, gain, variance). */
void mqae_entropy_attach(struct mqae_entropy *e, struct mqae_coder *coder);

/*
 * Encode `count` symbols aiming at `target`, in the decoder's blocks of
 * four. `got` receives what the decoder will actually produce, which is
 * what the caller must feed its own model of the stream.
 */
void mqae_entropy_encode(struct mqae_entropy *e, const int32_t *target, int32_t *got,
			 unsigned count);

/* One block header, as mqa_residual_decoder_block_init reads it: the
 * chosen level and variance fields, written through the coder. */
void mqae_entropy_block_init(struct mqae_entropy *e, uint32_t block, int32_t rate,
			     unsigned level_hi, unsigned variance_low);

#endif
