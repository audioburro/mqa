/*
 * mqa/crc32.c -- reflected CRC-32 over 32-bit words.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mqa/crc32.h"

static uint32_t table[256];
static int ready;

static void build(void)
{
	unsigned i, k;

	for (i = 0; i < 256; i++) {
		uint32_t c = i;

		for (k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ 0xedb88320u : c >> 1;
		table[i] = c;
	}
	ready = 1;
}

uint32_t mqa_crc32_word(uint32_t crc, uint32_t word)
{
	unsigned k;

	if (!ready)
		build();
	/* the reference's form: the register is stepped four times, then
	 * the whole word is folded in (so a word takes effect one word late) */
	for (k = 0; k < 4; k++)
		crc = table[crc & 0xff] ^ (crc >> 8);
	return crc ^ word;
}
