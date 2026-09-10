/*
 * Reflected CRC-24 table generation -- see include/mqa/crc24.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mqa/crc24.h"

void mqa_crc24_init(struct mqa_crc24 *c)
{
	unsigned i, bit;

	for (i = 0; i < 256; i++) {
		uint32_t r = i;

		for (bit = 0; bit < 8; bit++)
			r = (r & 1) ? (r >> 1) ^ MQA_CRC24_POLY_REFLECTED : r >> 1;

		c->table[i] = r;
	}
}
