/*
 * mqa/lifting.h -- the fixed-point "gain step" shared by the two lifting
 * stages of MQA stage-1 decoding.
 *
 * Both the reconstruction filter and the P/Q predictor turn a pair of
 * dither samples (d0, d1) into a pair of output samples with the same
 * two-step butterfly:
 *
 *     y1 = d0 + d1 + gain_step(x1)          (sum term)
 *     y2 = d1      + gain_step(x2)          (second sample)
 *     out0 = y1 - y2, out1 = y2
 *
 * where x1/x2 are stage-specific prediction terms and
 *
 *     gain_step(x) = scale * ((gain * x) >> (32 + shift))
 *
 * i.e. a Q31 gain applied to x, shifted down, then scaled by the same
 * small integer the dither generator uses. This header holds just that
 * common step; the stages themselves live in reconstruct.h/predictor.h.
 *
 * The butterfly is a lifting step in the sense of Sweldens (1996): a
 * reversible ladder of predict-and-update stages, the same construction
 * as the integer-to-integer transforms of Calderbank, Daubechies,
 * Sweldens and Yeo. See docs/prior-art.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_LIFTING_H
#define MQA_DECODE_LIFTING_H

#include <stdint.h>

struct mqa_gain_params {
	uint32_t scale;   /* dither scale; 203 observed                       */
	int32_t  gain;    /* Q31 stage-2 gain; 0x50b59897 (~0.63) observed     */
	int      shift;   /* post-gain arithmetic shift; 7 observed            */
};

/* High 32 bits of a signed 64-bit product, i.e. (a * b) >> 32. */
static inline int32_t mqa_mulhi32(int32_t a, int32_t b)
{
	return (int32_t)(((int64_t)a * b) >> 32);
}

/*
 * scale * ((gain * x) >> (32 + shift)), evaluated exactly as the decoder
 * does: a 32x32->64 signed multiply, an arithmetic shift, then a wrapping
 * 32-bit multiply by `scale`.
 */
static inline int32_t mqa_gain_step(const struct mqa_gain_params *p, int32_t x)
{
	int32_t g = mqa_mulhi32(p->gain, x) >> p->shift;

	return (int32_t)((uint32_t)g * p->scale);
}

/*
 * The prediction terms fed to mqa_gain_step() all have the shape
 * `scale + 2 * inner`; this helper keeps that visible at the call sites.
 */
static inline int32_t mqa_gain_input(const struct mqa_gain_params *p, int32_t inner)
{
	return (int32_t)(p->scale + 2u * (uint32_t)inner);
}

#endif
