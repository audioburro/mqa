/*
 * The range coder run backwards -- see mqae/coder.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include "mqae/coder.h"

int mqae_coder_init(struct mqae_coder *c)
{
	memset(c, 0, sizeof *c);
	c->cap = 65536;
	c->op = malloc(c->cap * sizeof *c->op);
	c->bytes_cap = 65536;
	c->bytes = malloc(c->bytes_cap);
	return c->op && c->bytes ? 0 : -1;
}

void mqae_coder_free(struct mqae_coder *c)
{
	free(c->op);
	free(c->bytes);
	memset(c, 0, sizeof *c);
}

static struct mqae_op *push(struct mqae_coder *c)
{
	if (c->failed)
		return NULL;
	if (c->n == c->cap) {
		struct mqae_op *grown = realloc(c->op, c->cap * 2 * sizeof *c->op);

		if (!grown) {
			c->failed = 1;
			return NULL;
		}
		c->op = grown;
		c->cap *= 2;
	}
	memset(&c->op[c->n], 0, sizeof c->op[c->n]);
	return &c->op[c->n++];
}

void mqae_coder_start(struct mqae_coder *c)
{
	c->n = 0;
	c->nbytes = 0;
	c->range = 1;
	c->failed = 0;
	c->why = NULL;
	c->where = 0;
}

void mqae_coder_normalize(struct mqae_coder *c)
{
	while (c->range <= 0xffffffu) {
		struct mqae_op *op = push(c);

		if (!op)
			return;
		op->kind = MQAE_OP_REFILL;
		c->range *= 256;
	}
}

void mqae_coder_field(struct mqae_coder *c, unsigned bits, uint32_t value)
{
	struct mqae_op *op = push(c);

	if (!op)
		return;
	op->kind = MQAE_OP_FIELD;
	op->bits = (uint8_t)bits;
	op->a = value & ((1u << bits) - 1);
	c->range >>= bits;
}

void mqae_coder_narrow(struct mqae_coder *c, uint32_t lo, uint32_t hi)
{
	struct mqae_op *op = push(c);

	if (!op)
		return;
	if (hi <= lo || c->range < hi) {
		c->failed = 1;                 /* not a codeable interval */
		c->why = "interval does not fit the range";
		c->where = c->n;
		return;
	}
	op->kind = MQAE_OP_NARROW;
	op->a = lo;
	op->b = hi;
	op->c = hi - lo;
	c->range = (((c->range - hi) >> 19) + 1u) * op->c;
}

void mqae_coder_divide(struct mqae_coder *c, uint32_t n, uint32_t rem)
{
	struct mqae_op *op;
	uint32_t recip, top;

	if (n <= 1)
		return;                        /* the decoder reads nothing */
	op = push(c);
	if (!op)
		return;
	op->kind = MQAE_OP_DIVIDE;
	op->a = n;
	op->b = rem;
	recip = 0xffffffffu / n;
	top = (uint32_t)(((uint64_t)recip * (c->range - 1u)) >> 32);
	c->range = top + 1u;
}

/*
 * One step of the decoder's divide, so the backward pass can check that
 * the value it built really does come apart the way it must: the
 * decoder's quotient is a multiply-by-reciprocal that can land a step
 * low, and it keeps that quotient even where it corrects the remainder.
 */
static int divide_gives(uint32_t value, uint32_t n, uint32_t want_quot, uint32_t want_rem)
{
	uint32_t recip = 0xffffffffu / n;
	uint32_t quot = (uint32_t)(((uint64_t)value * recip) >> 32);
	uint32_t rem = value - quot * n;

	if (rem >= n)
		rem -= n;
	return quot == want_quot && rem == want_rem;
}

const uint8_t *mqae_coder_finish(struct mqae_coder *c, size_t *len)
{
	uint32_t value = 0;
	size_t i = c->n;

	c->nbytes = 0;
	while (i-- > 0 && !c->failed) {
		const struct mqae_op *op = &c->op[i];

		switch (op->kind) {
		case MQAE_OP_REFILL:
			if (c->nbytes == c->bytes_cap) {
				uint8_t *grown = realloc(c->bytes, c->bytes_cap * 2);

				if (!grown) {
					c->failed = 1;
					break;
				}
				c->bytes = grown;
				c->bytes_cap *= 2;
			}
			c->bytes[c->nbytes++] = (uint8_t)value;
			value >>= 8;
			break;
		case MQAE_OP_FIELD:
			value = (value << op->bits) | op->a;
			break;
		case MQAE_OP_NARROW:
			if (value / op->c >= (1u << 13)) {
				/* the state has outgrown the interval it has to
				 * fit through: the coder was asked to carry
				 * more than its 19-bit code space holds */
				c->failed = 1;
				c->why = "state too large for the interval";
				c->where = i;
				break;
			}
			value = ((value / op->c) << 19) | (op->a + value % op->c);
			break;
		case MQAE_OP_DIVIDE: {
			/*
			 * The decoder divides by multiplying by a reciprocal,
			 * which for a large quotient lands a step low, and
			 * it keeps that quotient while correcting the
			 * remainder by adding the divisor back. So the value
			 * that comes apart into (quotient, remainder) is not
			 * always quotient * n + remainder: it can be a whole
			 * divisor further on, which is the case that
			 * correction exists for.
			 */
			unsigned j;

			for (j = 0; j < 3; j++)
				if (divide_gives((value + j) * op->a + op->b, op->a, value, op->b))
					break;
			if (j == 3) {
				c->failed = 1;
				c->why = "no value divides as the decoder will";
				c->where = i;
				break;
			}
			value = (value + j) * op->a + op->b;
			break;
		}
		default:
			c->failed = 1;
			break;
		}
	}
	if (value != 0 && !c->failed) {
		c->failed = 1;         /* the block does not start from nothing */
		c->why = "the block would not start from an empty coder";
	}
	/* the bytes came out last first */
	for (i = 0; i < c->nbytes / 2; i++) {
		uint8_t t = c->bytes[i];

		c->bytes[i] = c->bytes[c->nbytes - 1 - i];
		c->bytes[c->nbytes - 1 - i] = t;
	}
	*len = c->nbytes;
	return c->bytes;
}
