/*
 * mqa/entropy.h -- the adaptive arithmetic decoder behind the residuals.
 *
 * One instance per record, two records per channel. Each call produces
 * `count` signed symbols in blocks of four from a byte-fed range coder,
 * whose source is the symbol ring (stream.h) or, for carrier classes that
 * keep residual data in the carrier, the digit FIFO (residual_stage.h).
 *
 * The arithmetic is specified in docs/mqa-stage1-spec.md: the coder in
 * section 7.2, the symbol (dither draws, the escape and table forms, the
 * quantiser, the AR stages, the range-coded residual) in 7.5, the
 * per-block LMS update in 7.6, and the tables in A.6.
 *
 * Three structs split what the reference keeps in one object, so the
 * tests can check them separately: the range coder, the distribution
 * table and the decoder state. Arithmetic wraps at 32 bits where the
 * reference wraps; 64-bit products keep their high words.
 *
 * The step adaptation divides by a magnitude measure that can be zero.
 * The reference's platform helper returns 0 for that, this code does the
 * same, and `zero_divides` counts how often it happened.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_ENTROPY_H
#define MQA_DECODE_ENTROPY_H

#include <stdint.h>
#include "mqa/lifting.h"

/* --- byte source and range coder -------------------------------------- */

struct mqa_byte_ring {
	uint8_t *data;
	unsigned size;       /* ring length in bytes (2048 observed)      */
	unsigned cursor;     /* next byte to read; wraps to 0 at size     */
	unsigned wpos;       /* next byte to write (stream.h fills it)    */
};

struct mqa_range_decoder {
	uint32_t range;
	uint32_t value;
	uint32_t base;       /* symbols the source carries: 256 for a byte ring,
			      * or the carrier-digit FIFO's radix (residual_stage.h) */
	struct mqa_byte_ring *src;
};

/* Pull source symbols until range > 0xffffff (range and value grow by the
 * source's base each time). */
void mqa_range_normalize(struct mqa_range_decoder *rc);

/* --- the cumulative-distribution table --------------------------------- */

struct mqa_entropy_record {
	int32_t  base;        /* first value in the bin                       */
	uint32_t threshold;   /* cumulative code-space start of the bin       */
	uint32_t recip;       /* 2^32 / width, for dividing without dividing  */
	int16_t  width;       /* code-space units per value in the bin        */
	int16_t  weight;      /* magnitude weight used for step adaptation    */
};

#define MQA_ENTROPY_RECORDS 32
extern const struct mqa_entropy_record mqa_entropy_records[MQA_ENTROPY_RECORDS];

/*
 * Which record a signed magnitude belongs to, and where a value sits in
 * the code space within it. Together they are the distribution itself:
 * the interval [position(a), position(b)) is the code space a symbol
 * whose two magnitudes are a and b occupies, which is what the decoder
 * narrows to and what an encoder has to aim at.
 */
const struct mqa_entropy_record *mqa_entropy_bin(int32_t x);
uint32_t mqa_entropy_bin_position(const struct mqa_entropy_record *rec, int32_t x);

/* --- decoder state ------------------------------------------------------ */

#define MQA_ENTROPY_HISTORY 8
#define MQA_ENTROPY_TAPS    4
#define MQA_ENTROPY_BLOCK   4    /* symbols per adaptation block */

struct mqa_entropy_decoder {
	struct mqa_range_decoder *coder;      /* symbol coder                  */
	struct mqa_range_decoder *residual;   /* residual coder (same object   */
	                                      /* as `coder` in all streams     */
	                                      /* seen)                         */
	uint32_t rng;                         /* NR LCG state, dither source   */

	/* adaptive quantiser step */
	int32_t level, level_min, level_max, level_rate;
	int32_t variance;

	/* output scaling */
	struct mqa_gain_params gain;          /* scale, gain, shift            */
	uint32_t scale2;                      /* G() is applied iff == scale   */

	/* AR predictor state: taps x[k] are refreshed once per block; the
	 * pred_* slots are written at 3, 2, 1, 0 through a block, so a call
	 * always starts a fresh block */
	int32_t taps[MQA_ENTROPY_TAPS];
	int32_t pred_a[MQA_ENTROPY_TAPS];     /* last four stage-1 predictions */
	int32_t pred_b[MQA_ENTROPY_TAPS];     /* last four stage-2 predictions */

	/* LMS filter state: an 8-entry ring, newest at `head`. Blocks of
	 * four push four entries, so `head` is 0 or 4 between calls. */
	int16_t history[MQA_ENTROPY_HISTORY];
	unsigned head;
	int16_t coef[MQA_ENTROPY_TAPS];
	uint16_t decay[MQA_ENTROPY_TAPS];

	/* diagnostics: the reference decoder divides by zero here for some
	 * inputs (a symbol whose magnitude measure is 0); libgcc returns 0
	 * for that, which is reproduced, and the event counted. */
	unsigned zero_divides;
};

/*
 * Decode `count` symbols into `out`. `with_residual` selects whether the
 * range-coded residual is decoded and added (the decoder's fourth
 * argument, masked to its low bit when scale != 0); every real call seen
 * passes 1.
 */
void mqa_entropy_decode(struct mqa_entropy_decoder *d, int32_t *out, unsigned count,
			int with_residual);

#endif
