/*
 * watermark.c -- the renderer signalling in the output's low bits
 * (see watermark.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/watermark.h"
#include "mqa/crc24.h"

/* The two shift registers: the message's checksum is a CRC-32, and the
 * frame walk is the same 24-bit register the decoder's LSB correction
 * uses (crc24.h), clocked with zero bytes. */
#define CRC32_POLY  0xedb88320u
#define WALK_POLY   MQA_CRC24_POLY_REFLECTED

/*
 * How a frame is nudged: sixteen (left, right) pairs, chosen by the
 * walk's register and by whether the frame's parity already carries the
 * message bit. The pairs come in twos, one for each value of that bit,
 * and differ in parity, so the frame always ends up carrying it.
 */
static const signed char nudge[32] = {
	 0,  0,   0,  1,   0,  0,   1,  0,
	 0,  0,   0, -1,   0,  0,  -1,  0,
	 1,  1,   0,  1,   1, -1,   1,  0,
	-1, -1,   0, -1,  -1,  1,  -1,  0,
};

static void build_table(uint32_t *t, uint32_t poly)
{
	unsigned i, k;

	for (i = 0; i < 256; i++) {
		uint32_t c = i;

		for (k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ poly : c >> 1;
		t[i] = c;
	}
}

/* The record's checksum: the CRC register is stepped four times and the
 * record's word folded in, as the decoder's checksums are taken, then
 * run out for three more steps. */
static uint8_t record_checksum(const struct mqa_watermark *w, const uint8_t *rec, unsigned n)
{
	uint32_t c = 0;
	unsigned i, k, words = n > 2 ? ((n - 3) & ~3u) + 4 : 0;

	if (words > n)
		words = n & ~3u;
	for (i = 0; i < words; i += 4) {
		uint32_t word = (uint32_t)rec[i] | (uint32_t)rec[i + 1] << 8 |
				(uint32_t)rec[i + 2] << 16 | (uint32_t)rec[i + 3] << 24;

		for (k = 0; k < 4; k++)
			c = w->crc[c & 0xff] ^ (c >> 8);
		c ^= word;
	}
	for (; i < n; i++)
		c = w->crc[c & 0xff] ^ ((c >> 8) | (uint32_t)rec[i] << 24);
	for (k = 0; k < 3; k++)
		c = w->crc[c & 0xff] ^ (c >> 8);
	return (uint8_t)c;
}

/* The next record of the cycle. */
static void next_record(struct mqa_watermark *w)
{
	static const uint8_t identifier[4] = { 0x8c, 0x49, 0xe0, 0xab };
	static const uint8_t profiles[3] = { 4, 8, 16 };
	uint8_t *r = w->rec;
	unsigned n;

	switch (w->index) {
	case 0:
		r[0] = 0;
		r[1] = 4;
		memcpy(r + 2, identifier, 4);
		break;
	case 1:
		r[0] = 1;
		r[1] = 4;
		r[2] = (uint8_t)w->config;
		r[3] = (uint8_t)(w->config >> 8);
		r[4] = (uint8_t)(w->config >> 16);
		r[5] = (uint8_t)(w->config >> 24);
		break;
	case 2:
		r[0] = 2;
		r[1] = 2;
		r[2] = (uint8_t)w->config2;
		r[3] = (uint8_t)(w->config2 >> 8);
		break;
	default:
		r[0] = 4;
		r[1] = 1;
		r[2] = profiles[w->index - 3];
		break;
	}
	n = r[1] + 2u;
	r[n] = record_checksum(w, r, n);
	w->index = w->index + 1 >= 6 ? 0 : w->index + 1;
	w->pos = 0;
}

void mqa_watermark_init(struct mqa_watermark *w)
{
	memset(w, 0, sizeof *w);
	build_table(w->crc, CRC32_POLY);
	build_table(w->walk, WALK_POLY);
	w->state = ~0u;
	w->config = 31u << 20;                    /* nothing known about the stream yet */
	next_record(w);
	w->ready = 1;
}

void mqa_watermark_set_stream(struct mqa_watermark *w, unsigned render_filter,
			      unsigned orig_rate_code, unsigned render_bitdepth,
			      int authenticated)
{
	if (!w->ready)
		mqa_watermark_init(w);
	/* bit 8 is set on every stream seen, all of them authenticated */
	w->config = (render_filter & 0x1f) | (authenticated ? 1u << 8 : 0) |
		    (orig_rate_code & 0x1f) << 20 | (render_bitdepth & 3) << 25;
}

void mqa_watermark_apply(struct mqa_watermark *w, int32_t *l, int32_t *r, unsigned n)
{
	unsigned i;

	if (!w->ready)
		mqa_watermark_init(w);
	for (i = 0; i < n; i++) {
		unsigned carried, at;

		if (w->bits == 0) {
			if (w->pos == (unsigned)w->rec[1] + 3u)
				next_record(w);
			w->reg = (uint32_t)w->rec[w->pos++] << 24;
			w->bits = 8;
		}
		/* the frame's parity against the bit it must carry */
		carried = ((uint32_t)(l[i] ^ r[i]) ^ (w->reg >> 31)) & 1;
		at = ((w->state & 14) + carried) * 2;
		w->state = w->walk[w->state & 0xff] ^ (w->state >> 8);
		l[i] += nudge[at];
		r[i] += nudge[at + 1];
		w->reg <<= 1;
		w->bits--;
	}
}
