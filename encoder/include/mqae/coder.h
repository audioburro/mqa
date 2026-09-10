/*
 * mqae/coder.h -- the range coder, run backwards.
 *
 * The decoder's coder (mqa/entropy.h) is an interval that only shrinks,
 * renormalised by pulling bytes:
 *
 *     refill      value = value * 256 + byte,   range *= 256
 *     field(n)    field = value & (2^n - 1),    value >>= n, range >>= n
 *     narrow      value = (value >> 19) * span + ((value & 0x7ffff) - lo)
 *                 range = (((range - hi) >> 19) + 1) * span
 *     divide(n)   quotient and remainder of value by n, value = quotient
 *
 * Each of these is a bijection on the value once the caller has chosen
 * what it should produce, and only `range` decides when a byte is
 * pulled. So the encoder runs two passes: forwards, choosing what each
 * step decodes to and tracking `range`; then backwards from the last
 * step to the first, building the value the decoder must have held and
 * shedding a byte wherever the forward pass pulled one. The bytes come
 * out reversed and are turned round at the end.
 *
 * Read this way the decoder is an asymmetric numeral coder: a state that
 * absorbs symbols by multiplication and gives them back by division,
 * which is why its encoder has to work from the end of the block. See
 * docs/prior-art.md.
 *
 * A run covers one 4096-sample block, which is where the decoder resets
 * the coder to (value 0, range 1) and lets the first renormalisation
 * fill it. Blocks are independent of each other.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQAE_CODER_H
#define MQAE_CODER_H

#include <stddef.h>
#include <stdint.h>

enum mqae_op_kind {
	MQAE_OP_REFILL,     /* the decoder pulled a byte here          */
	MQAE_OP_FIELD,      /* it took `n` bits from the value's bottom */
	MQAE_OP_NARROW,     /* a symbol: the interval [lo, hi) of 2^19  */
	MQAE_OP_DIVIDE      /* a residual: `rem` out of `n`             */
};

struct mqae_op {
	uint8_t kind;
	uint8_t bits;                 /* FIELD: how many                 */
	uint32_t a, b, c;             /* FIELD: value; NARROW: lo, hi, span;
				       * DIVIDE: n, rem                  */
};

struct mqae_coder {
	struct mqae_op *op;
	size_t n, cap;
	uint32_t range;               /* the decoder's, tracked forward  */
	int failed;

	uint8_t *bytes;               /* what the backward pass produced */
	size_t nbytes, bytes_cap;
	const char *why;              /* what went wrong, when it did    */
	size_t where;                 /* the step it went wrong at       */
};

int mqae_coder_init(struct mqae_coder *c);
void mqae_coder_free(struct mqae_coder *c);

/* Start a block: the decoder's (value 0, range 1). */
void mqae_coder_start(struct mqae_coder *c);

/*
 * The forward pass. Each call records what the decoder will do and
 * advances `range` exactly as it will. `mqae_coder_normalize` is the
 * renormalisation the decoder does before a read; the reads themselves
 * are the three below it.
 */
void mqae_coder_normalize(struct mqae_coder *c);
void mqae_coder_field(struct mqae_coder *c, unsigned bits, uint32_t value);
void mqae_coder_narrow(struct mqae_coder *c, uint32_t lo, uint32_t hi);
void mqae_coder_divide(struct mqae_coder *c, uint32_t n, uint32_t rem);

/*
 * The backward pass: build the bytes the decoder has to be given. The
 * result is `*len` bytes valid until the next call; the coder is left
 * ready for another block after mqae_coder_start().
 */
const uint8_t *mqae_coder_finish(struct mqae_coder *c, size_t *len);

#endif
