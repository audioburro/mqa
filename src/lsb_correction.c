/*
 * Stage-1 LSB correction -- see include/mqa/lsb_correction.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/lsb_correction.h"

const int8_t mqa_lsb_correction_table[16][2] = {
	{ 0,  0}, { 0,  1}, { 0,  0}, { 1,  0},
	{ 0,  0}, { 0, -1}, { 0,  0}, {-1,  0},
	{ 1,  1}, { 0,  1}, { 1, -1}, { 1,  0},
	{-1, -1}, { 0, -1}, {-1,  1}, {-1,  0},
};

/*
 * Built by the decoder from immediate operands: five payload bytes after
 * a length byte, then a CRC-32 byte over the header. Byte 1 is a length
 * field: the segment is exhausted once segment[1] + 3 bytes have been read.
 */
const uint8_t mqa_lsb_initial_segment[MQA_LSB_INITIAL_SEGMENT_LEN] = {
	0x00, 0x04, 0x8c, 0x49, 0xe0, 0xab, 0x2c,
};

void mqa_lsb_init(struct mqa_lsb_corrector *c, mqa_lsb_refill_fn refill, void *user)
{
	memset(c, 0, sizeof *c);
	mqa_crc24_init(&c->crc);
	c->scrambler = 0xffffffffu;
	c->refill = refill;
	c->user = user;
	mqa_lsb_set_segment(c, mqa_lsb_initial_segment, MQA_LSB_INITIAL_SEGMENT_LEN);
}

void mqa_lsb_set_segment(struct mqa_lsb_corrector *c, const uint8_t *segment, unsigned len)
{
	c->segment = segment;
	c->segment_len = len;
	c->cursor = 0;
}

static unsigned segment_end(const struct mqa_lsb_corrector *c)
{
	unsigned end = (unsigned)c->segment[1] + 3;

	return end < c->segment_len ? end : c->segment_len;
}

static void fetch_priming_byte(struct mqa_lsb_corrector *c)
{
	if (c->cursor == segment_end(c)) {
		if (c->refill)
			c->refill(c, c->user);
		else
			c->cursor = 0;
	}

	c->shift = (uint32_t)c->segment[c->cursor++] << 24;
}

void mqa_lsb_correct(struct mqa_lsb_corrector *c, int32_t *l, int32_t *r)
{
	unsigned bit, index;

	if (c->samples_seen++ % 8 == 0)
		fetch_priming_byte(c);

	bit = ((uint32_t)*l ^ (uint32_t)*r ^ (c->shift >> 31)) & 1;
	index = (c->scrambler & 0xe) | bit;

	*l += mqa_lsb_correction_table[index][0];
	*r += mqa_lsb_correction_table[index][1];

	c->scrambler = mqa_crc24_update(&c->crc, c->scrambler, 0);
	c->shift <<= 1;
}
