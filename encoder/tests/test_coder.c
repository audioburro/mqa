/*
 * The range coder run backwards, checked against the decoder's own.
 *
 * A random sequence of the four things a decoder does to the coder is
 * encoded, and then the library's range decoder is driven through the
 * same sequence over the bytes that came out: every field, every symbol
 * interval and every residual has to come back exactly, and the byte
 * stream has to run out at the same moment.
 */
#include "mqae/coder.h"
#include "mqa/entropy.h"
#include "util.h"

#define OPS 4000

static uint32_t rnd_state = 12345;

static uint32_t rnd(uint32_t n)
{
	rnd_state = rnd_state * 1664525u + 1013904223u;
	return (uint32_t)(((uint64_t)(rnd_state >> 8) * n) >> 24);
}

/* What the decoder does for one recorded step, and what it produced. */
static uint32_t replay(struct mqa_range_decoder *rc, const struct mqae_op *op, int *bad)
{
	switch (op->kind) {
	case MQAE_OP_FIELD: {
		uint32_t v = rc->value & ((1u << op->bits) - 1);

		rc->range >>= op->bits;
		rc->value >>= op->bits;
		return v;
	}
	case MQAE_OP_NARROW: {
		uint32_t low = rc->value & 0x7ffffu;
		uint32_t span = op->b - op->a;

		if (low < op->a || low >= op->b)
			*bad = 1;              /* outside the interval it coded */
		rc->value = (rc->value >> 19) * span + (low - op->a);
		rc->range = (((rc->range - op->b) >> 19) + 1u) * span;
		/* a symbol carries no value of its own: what it has to get
		 * right is that the code word landed inside its interval */
		return low;
	}
	case MQAE_OP_DIVIDE: {
		uint32_t recip = 0xffffffffu / op->a;
		uint32_t quot = (uint32_t)(((uint64_t)rc->value * recip) >> 32);
		uint32_t rem = rc->value - quot * op->a;
		uint32_t top = (uint32_t)(((uint64_t)recip * (rc->range - 1u)) >> 32);

		if (rem >= op->a)
			rem -= op->a;
		rc->range = top + 1u;
		rc->value = quot;
		return rem;
	}
	default:
		return 0;
	}
}

int main(void)
{
	static struct mqae_coder c;
	static struct mqa_range_decoder rc;
	static struct mqa_byte_ring ring;
	static uint8_t ringbuf[1 << 20];
	static struct mqae_op ops[OPS];
	static uint32_t want[OPS];
	unsigned i, nops = 0;
	const uint8_t *bytes;
	size_t len;
	int bad = 0, trial;

	for (trial = 0; trial < 8; trial++) {
		if (mqae_coder_init(&c) < 0)
			return 1;
		mqae_coder_start(&c);
		nops = 0;
		for (i = 0; i < OPS && !c.failed; i++) {
			unsigned pick = rnd(3);

			mqae_coder_normalize(&c);
			if (pick == 0) {
				unsigned bits = 1 + rnd(11);
				uint32_t v = rnd(1u << bits);

				want[nops] = v;
				ops[nops].kind = MQAE_OP_FIELD;
				ops[nops].bits = (uint8_t)bits;
				nops++;
				mqae_coder_field(&c, bits, v);
			} else if (pick == 1) {
				/* an interval inside the 19-bit code space */
				uint32_t lo = 32 + rnd(1u << 18);
				uint32_t hi = lo + 1 + rnd(1u << 17);

				want[nops] = ~0u;      /* checked by the interval */
				ops[nops].kind = MQAE_OP_NARROW;
				ops[nops].a = lo;
				ops[nops].b = hi;
				nops++;
				mqae_coder_narrow(&c, lo, hi);
			} else {
				uint32_t n = 2 + rnd(4096);
				uint32_t rem = rnd(n);

				want[nops] = rem;
				ops[nops].kind = MQAE_OP_DIVIDE;
				ops[nops].a = n;
				ops[nops].b = rem;
				nops++;
				mqae_coder_divide(&c, n, rem);
			}
		}
		CHECK_EQ("the forward pass held up", c.failed, 0);
		bytes = mqae_coder_finish(&c, &len);
		if (c.failed && c.why)
			printf("  backward: %s at step %u of %u\n", c.why,
			       (unsigned)c.where, (unsigned)c.n);
		CHECK_EQ("the backward pass held up", c.failed, 0);
		CHECK_EQ("it produced bytes", (int)(len > 0), 1);

		/* now decode them */
		memcpy(ringbuf, bytes, len);
		ring.data = ringbuf;
		ring.size = sizeof ringbuf;
		ring.cursor = 0;
		ring.wpos = (unsigned)len;
		rc.src = &ring;
		rc.base = 256;
		rc.value = 0;
		rc.range = 1;
		for (i = 0; i < nops; i++) {
			uint32_t got;

			mqa_range_normalize(&rc);
			got = replay(&rc, &ops[i], &bad);
			if (ops[i].kind == MQAE_OP_NARROW)
				got = want[i];
			if (got != want[i]) {
				printf("  step %u: got %u want %u\n", i, got, want[i]);
				bad = 1;
				break;
			}
		}
		CHECK_EQ("every step decoded as it was coded", bad, 0);
		CHECK_EQ("the bytes ran out exactly", (int)ring.cursor, (int)len);
		mqae_coder_free(&c);
		if (test_fails)
			break;
	}
	printf("encoder coder: %s\n", test_fails ? "FAILED" : "ok");
	return test_fails != 0;
}
