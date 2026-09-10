/*
 * The data channel -- see mqae/chan.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include "mqae/chan.h"
#include "mqa/stream.h"
#include "mqa/descrambler.h"

int mqae_chan_init(struct mqae_chan *c)
{
	memset(c, 0, sizeof *c);
	c->cap = 65536;
	c->data = malloc(c->cap);
	mqa_descrambler_init(&c->ks);
	mqa_descrambler_start(&c->ks, 0);
	return c->data ? 0 : -1;
}

void mqae_chan_free(struct mqae_chan *c)
{
	free(c->data);
	c->data = NULL;
	c->cap = c->len = 0;
}

static uint8_t *room(struct mqae_chan *c, size_t n)
{
	uint8_t *grown;
	uint8_t *at;

	if (c->failed)
		return NULL;
	while (c->len + n > c->cap) {
		grown = realloc(c->data, c->cap * 2);
		if (!grown) {
			c->failed = 1;
			return NULL;
		}
		c->data = grown;
		c->cap *= 2;
	}
	at = c->data + c->len;
	c->len += n;
	return at;
}

unsigned mqae_chan_check(size_t offset, const uint8_t *after, size_t n)
{
	uint32_t reg = (uint32_t)offset;
	size_t i;

	for (i = 0; i < n; i++)
		reg = mqa_stream_check_update(reg, after[i]);
	return reg & 0xf;
}

/* Write a message whose bytes after the first are already in place. */
static void finish(struct mqae_chan *c, uint8_t *msg, unsigned type, size_t n)
{
	msg[0] = (uint8_t)((type & 0xf) | mqae_chan_check((size_t)(msg - c->data), msg + 1, n) << 4);
}

void mqae_chan_record(struct mqae_chan *c, unsigned scale_index,
		      const uint8_t seeds[8], const uint8_t state[32])
{
	uint32_t id = (uint32_t)(scale_index & 63) * (1u << 6 | 1u << 12 | 1u << 18);
	uint8_t *m = room(c, 6 + 8 + 32);

	if (!m)
		return;
	m[1] = (uint8_t)id;
	m[2] = (uint8_t)(id >> 8);
	m[3] = (uint8_t)(id >> 16);
	m[4] = 8;                       /* part A: the predictor seeds     */
	m[5] = 32;                      /* part B: the filter's history    */
	if (seeds)
		memcpy(m + 6, seeds, 8);
	else
		memset(m + 6, 0, 8);
	if (state)
		memcpy(m + 14, state, 32);
	else
		memset(m + 14, 0, 32);
	finish(c, m, 4, 5 + 40);
}

void mqae_chan_message(struct mqae_chan *c, unsigned type, const uint8_t *payload, unsigned size)
{
	uint8_t *m;

	if (size > 255)
		size = 255;
	m = room(c, 2 + size);
	if (!m)
		return;
	m[1] = (uint8_t)size;
	if (payload)
		memcpy(m + 2, payload, size);
	else
		memset(m + 2, 0, size);
	finish(c, m, type, 1 + size);
}

void mqae_chan_sync(struct mqae_chan *c, unsigned kind, unsigned scale_index, const uint32_t *crc)
{
	/*
	 * The header's second byte says how much follows: bit 0 adds the
	 * stream id (which carries the scale indices), bit 1 a 32-bit word.
	 * That byte is also the id's own low byte: a reader takes the id
	 * from these three bytes and masks the low six bits off again, so
	 * the byte carries the id's bits 6 and 7 alongside the flags and
	 * the kind. A decoder checks the id against the one the parameter
	 * record installed, so it repeats the same fields.
	 */
	uint32_t id = (uint32_t)(scale_index & 63) * (1u << 6 | 1u << 12 | 1u << 18);
	unsigned extra = crc ? 7 : 3;
	uint8_t *m = room(c, 1 + extra);

	if (!m)
		return;
	m[1] = (uint8_t)((id & 0xc0) | 1u | (crc ? 2u : 0u) | (kind & 0xf) << 2);
	m[2] = (uint8_t)(id >> 8);
	m[3] = (uint8_t)(id >> 16);
	if (crc) {
		m[4] = (uint8_t)*crc;
		m[5] = (uint8_t)(*crc >> 8);
		m[6] = (uint8_t)(*crc >> 16);
		m[7] = (uint8_t)(*crc >> 24);
	}
	finish(c, m, 3, extra);
}

void mqae_chan_idle(struct mqae_chan *c, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		uint8_t *m = room(c, 1);

		if (!m)
			return;
		finish(c, m, 0, 0);
	}
}

void mqae_chan_pad_to(struct mqae_chan *c, size_t len)
{
	if (len > c->len)
		mqae_chan_idle(c, len - c->len);
}

const uint8_t *mqae_chan_wire(struct mqae_chan *c, size_t upto)
{
	size_t n;

	if (upto > c->len)
		upto = c->len;
	/* the scrambler works a pair at a time (one pair per frame), so it
	 * stops one short of an odd request and takes that byte next time */
	n = (upto - c->scrambled) & ~(size_t)1;
	if (n) {
		mqa_descrambler_fill_bytes(&c->ks, c->data + c->scrambled,
					   c->data + c->scrambled, 0, (unsigned)n);
		c->scrambled += n;
	}
	return c->data;
}
