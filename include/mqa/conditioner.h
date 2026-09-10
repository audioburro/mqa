/*
 * mqa/conditioner.h -- conditioning the carrier after the data channel
 * has been taken out of it.
 *
 * Capturing a group's data channel empties every sample's low byte. The
 * reference decoder does not send such samples on; before the group
 * reaches the decoder proper it runs two per-sample stages over it, and
 * since the output is built from these samples the stages are audible in
 * it:
 *
 *   1. requantisation: the pair goes to mid/side, each part is
 *      requantised to its top bits with subtractive dither and error
 *      feedback through a short noise-shaping filter, and comes back to
 *      left/right;
 *   2. signalling: the stream's level gain is applied, a leaky feedback
 *      term subtracted, and the sample dithered and requantised once
 *      more, choosing the rounding direction of the larger error so that
 *      the pair carries one bit of the control bitstream the intake
 *      extracted 480 frames earlier. Samples near full scale take a
 *      special short code.
 *
 * At a resync both stages write a marker: a few samples coarsely
 * quantised with bits of the datasync packet, from which the bitstream's
 * read position is set.
 *
 * Section 10 of docs/mqa-stage1-spec.md describes the stages and names
 * this module as their normative statement: the per-sample arithmetic
 * runs to several hundred lines of wrapping 32-bit operations and is
 * verified against the reference over every group of the test material.
 * The constants are in A.8. The lineage (Gerzon-Craven noise shaping) is
 * in docs/prior-art.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_CONDITIONER_H
#define MQA_DECODE_CONDITIONER_H

#include <stdint.h>

/* The control bits extracted from the stream, one per frame from its
 * start, kept for the signalling stage to re-embed: a 32768-bit ring. */
#define MQA_BITRING_BITS 32768
struct mqa_bitring {
	uint32_t word[MQA_BITRING_BITS / 32];
	uint32_t wpos, rpos;                      /* bit positions, mod 2^15  */
};

void mqa_bitring_reset(struct mqa_bitring *r);
void mqa_bitring_append(struct mqa_bitring *r, unsigned bit);

#define MQA_CONDITIONER_TAPS 6
#define MQA_CONDITIONER_MARKER_WORDS 8

struct mqa_conditioner {
	/* configuration */
	const int32_t *taps;                      /* noise-shaping filter, Q11 */
	int six_taps;                             /* 6 taps, else 3            */
	unsigned shift;                           /* the channel bit's position: samples are shifted down by it */
	int32_t gain2;                            /* 8.8 gain restoring the shift, or the datasync's */
	int32_t full, limit, limit_hi;            /* full scale and the special-coding thresholds */
	unsigned dither_mode;                     /* 0: stage 2 undithered     */
	uint32_t seed2;                           /* stage 2's LCG seed        */
	int32_t gain;                             /* the level gain, Q31 (negative: the product is of -2x) */
	int32_t lag_out, lag_prev;                /* the leaky feedback's weights */

	/* stage 1 */
	uint32_t count1;                          /* samples done              */
	uint32_t lcg1[2];
	int32_t acc1[2];                          /* the filter's last outputs */
	int32_t err1[MQA_CONDITIONER_TAPS][2];    /* error history, newest at h1 */
	unsigned h1;

	/* stage 2 */
	uint32_t count2;
	uint32_t lcg2[2];
	int32_t acc2[2];
	int32_t err2[MQA_CONDITIONER_TAPS][2];
	unsigned h2;
	int32_t prev[2];                          /* last outputs              */
	int32_t lag[2];                           /* the leaky feedback terms  */
	int steady;

	/* the marker */
	uint32_t marker_pos;
	uint32_t marker[MQA_CONDITIONER_MARKER_WORDS];

	int unsupported;                          /* a path not implemented ran */
};

/*
 * Configure both stages, resetting their state. `rate_code` picks the
 * noise-shaping filter; `dither_mode` and `shift` come from the stream;
 * `gain2` is 8.8; `ctx` is the level record: ctx[2] the level (units of
 * 1/256 octave of attenuation), ctx[3] the leaky feedback's strength.
 */
void mqa_conditioner_configure(struct mqa_conditioner *c, unsigned rate_code, unsigned dither_mode,
			       unsigned shift, int32_t gain2, const uint32_t ctx[4]);

/* Install a resync marker: written when the sample counters reach `at`. */
void mqa_conditioner_set_marker(struct mqa_conditioner *c, uint32_t at, const uint32_t *words, unsigned n);

/* Run both stages over a group's n frames in place. */
void mqa_conditioner_run(struct mqa_conditioner *c, int32_t *a, int32_t *b, unsigned n, struct mqa_bitring *ring);

/* The level gain as the reference computes it (Q31, from 256ths of an octave). */
int32_t mqa_conditioner_gain(uint32_t level);

/* The 8.8 gain of a datasync gain index (16 steps of 1/32 octave), before the shift. */
uint32_t mqa_conditioner_gain_index(unsigned index);

/* The reference's helper for the level record's feedback strength. */
uint32_t mqa_conditioner_lag_strength(uint32_t v);

#endif
