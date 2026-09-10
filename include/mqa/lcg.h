/*
 * mqa/lcg.h -- the 32-bit linear congruential generator used throughout
 * MQA stage-1 decoding.
 *
 * The same recurrence appears in several places in the decoder: as the
 * per-channel dither/noise source that seeds the reconstruction filter,
 * as a private noise pair inside the P/Q predictor, and as the scrambler
 * of the data channel that carries the entropy coder's input. All share one
 * multiplier/increment pair and the same output rule ("take the high 32
 * bits of state x scale"), so they are modelled once here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_LCG_H
#define MQA_DECODE_LCG_H

#include <stdint.h>

#define MQA_LCG_MULTIPLIER 0x17385ca9u
#define MQA_LCG_INCREMENT  0x47502932u

/* Advance one LCG state word. */
static inline uint32_t mqa_lcg_step(uint32_t state)
{
	return state * MQA_LCG_MULTIPLIER + MQA_LCG_INCREMENT;
}

/* Advance the dither LCG by n steps at once (exact modular arithmetic:
 * the reference uses a table of the same 2^k-step constants). */
static inline uint32_t mqa_lcg_jump(uint32_t state, uint32_t n)
{
	uint32_t a = MQA_LCG_MULTIPLIER, c = MQA_LCG_INCREMENT;

	while (n) {
		if (n & 1)
			state = state * a + c;
		c = c * a + c;
		a = a * a;
		n >>= 1;
	}
	return state;
}

/*
 * A dither generator: an LCG state plus the per-stream scale it is
 * multiplied by on output. `scale` is a small integer (203 has been the
 * only value observed so far); the output is the high 32 bits of the
 * 64-bit product state x scale, so it ranges over [0, scale).
 */
struct mqa_dither {
	uint32_t state;
	uint32_t scale;
};

static inline int32_t mqa_dither_next(struct mqa_dither *d)
{
	int32_t out = (int32_t)(((uint64_t)d->state * d->scale) >> 32);

	d->state = mqa_lcg_step(d->state);

	return out;
}

/*
 * The classic Numerical Recipes LCG (1664525 / 1013904223) is used by the
 * decoder for two things: re-seeding the descrambler, and as the
 * random source inside the entropy decoder. It is *not* the same
 * generator as the dither LCG above.
 */
#define MQA_NR_LCG_MULTIPLIER 0x19660du
#define MQA_NR_LCG_INCREMENT  0x3c6ef35fu

static inline uint32_t mqa_nr_lcg_step(uint32_t state)
{
	return state * MQA_NR_LCG_MULTIPLIER + MQA_NR_LCG_INCREMENT;
}

#endif
