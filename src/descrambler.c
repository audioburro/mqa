/*
 * Data-channel descrambler -- see include/mqa/descrambler.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/descrambler.h"
#include "mqa/lcg.h"

/* Constant mixed into the reseed; a compiled-in literal in the decoder. */
#define RESEED_SALT 0x2082352cu

void mqa_descrambler_init(struct mqa_descrambler *ks)
{
	memset(ks, 0, sizeof *ks);
}

/*
 * Both states are derived from (salt + reseed_count)^2 pushed through two
 * steps of the Numerical Recipes LCG: the first step's output seeds
 * stream A, the second stream B.
 */
void mqa_descrambler_reseed(struct mqa_descrambler *ks)
{
	uint32_t s = RESEED_SALT + ks->reseeds;

	s *= s;
	ks->lcg[0] = mqa_nr_lcg_step(s);
	ks->lcg[1] = mqa_nr_lcg_step(ks->lcg[0]);
	ks->reseeds++;
}

static unsigned min_u(unsigned x, unsigned y)
{
	return x < y ? x : y;
}

void mqa_descrambler_start(struct mqa_descrambler *ks, uint32_t position)
{
	uint32_t within = position & 0xffe;
	uint32_t s = RESEED_SALT + (position >> 12);

	ks->reseeds = (position >> 12) + 1;
	s *= s;
	ks->lcg[0] = mqa_nr_lcg_step(s);
	if (within)
		ks->lcg[0] = mqa_lcg_jump(ks->lcg[0], within / 2);
	ks->lcg[1] = mqa_nr_lcg_step(ks->lcg[0]);
	ks->budget = (0x1000 - within) >> 1;
}

void mqa_descrambler_fill_bytes(struct mqa_descrambler *ks, uint8_t *out,
				const uint8_t *src, unsigned start, unsigned nbytes)
{
	unsigned i = start, pairs = nbytes >> 1;

	out -= start;                      /* output index runs from 0 */
	if (nbytes & 1) {
		out[i] = (uint8_t)(src[i] ^ (ks->lcg[1] >> 24));
		ks->lcg[0] = mqa_lcg_step(ks->lcg[0]);
		ks->lcg[1] = mqa_lcg_step(ks->lcg[1]);
		ks->budget--;
		i++;
	}
	while (pairs) {
		unsigned n, k;

		if (ks->budget == 0) {
			mqa_descrambler_reseed(ks);
			ks->budget = MQA_DESCRAMBLER_BATCH;
		}
		n = min_u(pairs, ks->budget);
		ks->budget -= n;
		pairs -= n;
		for (k = 0; k < n; k++, i += 2) {
			out[i] = (uint8_t)(src[i] ^ (ks->lcg[0] >> 24));
			out[i + 1] = (uint8_t)(src[i + 1] ^ (ks->lcg[1] >> 24));
			ks->lcg[0] = mqa_lcg_step(ks->lcg[0]);
			ks->lcg[1] = mqa_lcg_step(ks->lcg[1]);
		}
	}
}

unsigned mqa_descrambler_fill(struct mqa_descrambler *ks, uint8_t *out,
			    const int32_t *a, const int32_t *b,
			    unsigned pos, unsigned count)
{
	while (count) {
		unsigned n = min_u(MQA_DESCRAMBLER_RING_SIZE - pos, count);
		unsigned i;

		if (ks->budget == 0) {
			mqa_descrambler_reseed(ks);
			ks->budget = MQA_DESCRAMBLER_BATCH;
		}
		n = min_u(n, ks->budget);
		ks->budget -= n;
		count -= n;

		for (i = 0; i < n; i++) {
			*out++ = (uint8_t)((uint32_t)a[pos + i] ^ (ks->lcg[0] >> 24));
			*out++ = (uint8_t)((uint32_t)b[pos + i] ^ (ks->lcg[1] >> 24));
			ks->lcg[0] = mqa_lcg_step(ks->lcg[0]);
			ks->lcg[1] = mqa_lcg_step(ks->lcg[1]);
		}

		pos += n;
		if (pos == MQA_DESCRAMBLER_RING_SIZE)
			pos = 0;
	}

	return pos;
}
