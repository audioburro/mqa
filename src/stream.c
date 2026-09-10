/*
 * Framed message stream parser -- see include/mqa/stream.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/stream.h"

/* Bytes after the type byte, per type 0..4 (types >= 5: 1, a size). */
static const uint8_t header_extra[5] = { 0, 1, 1, 1, 5 };

/* The 2-bit reflected CRC table (polynomial 0x03), computed once. */
static uint8_t crc2[256];
static int crc2_ready;

static void crc2_init(void)
{
	unsigned i, bit;

	for (i = 0; i < 256; i++) {
		unsigned r = i;

		for (bit = 0; bit < 8; bit++)
			r = (r & 1) ? (r >> 1) ^ 0x03 : r >> 1;
		crc2[i] = (uint8_t)r;
	}
	crc2_ready = 1;
}

uint32_t mqa_stream_check_update(uint32_t reg, uint8_t byte)
{
	if (!crc2_ready)
		crc2_init();
	return (((uint32_t)byte << 24) | (reg >> 8)) ^ crc2[reg & 0xff];
}

void mqa_stream_init(struct mqa_stream_parser *p,
		     struct mqa_byte_ring *ring_sym, struct mqa_byte_ring *ring_aux)
{
	memset(p, 0, sizeof *p);
	p->type = -1;
	p->ring_sym = ring_sym;
	p->ring_aux = ring_aux;
	crc2_init();
}

unsigned mqa_stream_push(struct mqa_stream_parser *p, const uint8_t *bytes, unsigned n)
{
	if (n > MQA_STREAM_FIFO - p->avail)
		n = MQA_STREAM_FIFO - p->avail;
	memcpy(p->fifo + p->avail, bytes, n);
	p->avail += n;
	return n;
}

static unsigned min_u(unsigned a, unsigned b)
{
	return a < b ? a : b;
}

/* Drop `n` bytes from the front of the FIFO. */
static void consume(struct mqa_stream_parser *p, unsigned n)
{
	p->avail -= n;
	p->consumed += n;
	if (p->avail)
		memmove(p->fifo, p->fifo + n, p->avail);
}

static void ring_put(struct mqa_byte_ring *r, uint8_t b)
{
	r->data[r->wpos] = b;
	r->wpos = r->wpos + 1 >= r->size ? 0 : r->wpos + 1;
}

static uint32_t read24(const uint8_t *b)
{
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16;
}

/*
 * Parse the header at the front of the FIFO. Returns 1 when a message
 * header has been consumed and its payload (if any) can be processed, 0
 * when parsing must stop (not enough bytes yet, or a fatal condition).
 */
static int parse_header(struct mqa_stream_parser *p, unsigned pos, int sync_enabled)
{
	uint8_t b0 = p->fifo[0];
	unsigned type = b0 & 0xf, extra, hlen, i;

	p->nibble = b0 >> 4;
	p->type = (int)type;

	if (p->consumed == 0) {
		/* the stream must open with a parameter record */
		if (type != 4) {
			p->started = 0;
			return 0;
		}
		extra = 5;
	} else if (type > 4) {
		extra = 1;
	} else {
		extra = header_extra[type];
		if (type == 3 && p->avail > 1) {
			uint8_t b1 = p->fifo[1];

			extra = (b1 & 1) ? 7 : 5;
			if (!(b1 & 2))
				extra = (b1 & 1) ? 3 : 1;
		}
	}
	hlen = extra + 1;
	if (hlen > p->avail) {
		p->type = -1;      /* wait for the rest of the header */
		return 0;
	}

	p->remaining = 0;
	if (type == 3) {
		uint8_t b1 = p->fifo[1];
		unsigned kind = (b1 >> 2) & 0xf;

		if (sync_enabled && (kind == 2 || kind == 5)) {
			struct mqa_stream_sync s;
			unsigned w0 = 2, w1 = 3, w2 = 4, w3 = 5;

			if (b1 & 1) {
				uint32_t id = (read24(p->fifo + 1) | b1) & ~0x3fu;

				if (p->rings_enabled) {
					uint32_t cur = p->stream_id;

					if ((((cur & 0xffff) ^ id) & 0xfc0) ||
					    ((cur ^ id) & 0x3f000) ||
					    ((((cur >> 16) & 0xff) ^ (id >> 16)) & 0xfc)) {
						if (p->on_sync_mismatch)
							p->on_sync_mismatch(p->user);
						return 0;
					}
				}
				p->stream_id = id;
				p->rings_enabled = 1;
				w0 = 4; w1 = 5; w2 = 6; w3 = 7;
			}
			memset(&s, 0, sizeof s);
			s.pos = pos;
			s.id = p->stream_id;
			s.kind = kind;
			s.mode2 = p->mode2;
			s.flag = (b1 >> 1) & 1;
			if (s.flag)
				s.word = (uint32_t)p->fifo[w0] | (uint32_t)p->fifo[w1] << 8 |
					 (uint32_t)p->fifo[w2] << 16 | (uint32_t)p->fifo[w3] << 24;
			if (p->on_sync)
				p->on_sync(p->user, &s);
		}
	} else if (type == 4) {
		p->stream_id = read24(p->fifo + 1);
		p->rec_len_a = p->fifo[4];
		p->remaining = (unsigned)p->fifo[4] + p->fifo[5];
		if (p->consumed != 0 || p->avail < p->remaining + 6) {
			p->started = 0;
			return 0;
		}
		p->rings_enabled = 1;
	} else if (type != 0) {
		p->remaining = p->fifo[1];
	}

	/* seed the check with the message's stream offset, absorb the header */
	p->check = p->consumed;
	p->check_expect = 0;
	p->check_pending = 0;
	for (i = 1; i <= extra; i++)
		p->check = mqa_stream_check_update(p->check, p->fifo[i]);
	consume(p, hlen);
	return 1;
}

static void handle_record(struct mqa_stream_parser *p, unsigned n)
{
	struct mqa_stream_record rec;
	unsigned la = min_u(p->rec_len_a, MQA_STREAM_RECORD_A);
	unsigned lb = n - p->rec_len_a;   /* unsigned, as the decoder does it */
	unsigned copy;

	if (lb > 31)
		lb = MQA_STREAM_RECORD_B;
	memset(&rec, 0, sizeof rec);
	rec.id = p->stream_id;
	rec.a_len = la;
	rec.b_len = lb;
	memcpy(rec.a, p->fifo, la);
	/* a malformed header can place part B past the FIFO's end; the
	 * reference decoder reads on regardless, we leave those bytes zero */
	copy = p->rec_len_a < MQA_STREAM_FIFO ? min_u(lb, MQA_STREAM_FIFO - p->rec_len_a) : 0;
	memcpy(rec.b, p->fifo + p->rec_len_a, copy);
	if (p->on_record)
		p->on_record(p->user, &rec);
}

static void handle_payload(struct mqa_stream_parser *p, unsigned n)
{
	struct mqa_byte_ring *ring = p->type == 2 ? p->ring_aux : p->ring_sym;
	unsigned i;

	if (!p->rings_enabled)
		return;
	if (n > 0xfd)
		n = 0xfe;
	for (i = 0; i < n; i++)
		ring_put(ring, p->fifo[i]);
}

/* A message is complete: verify its check. Returns 1 to keep parsing. */
static int end_message(struct mqa_stream_parser *p)
{
	uint32_t c = p->check;

	p->items++;
	p->type = -1;

	if (p->check_pending) {
		uint32_t expect = p->check_expect;

		p->check_expect = 0;
		p->check_pending = 0;
		c = crc2[c & 0xff] ^ (c >> 8);
		c = crc2[c & 0xff] ^ (c >> 8);
		c = crc2[c & 0xff] ^ (c >> 8);
		c = expect ^ crc2[c & 0xff];
		p->check = c;
	}
	if ((c & 0xf) == p->nibble)
		return 1;

	/* check failed: go offline until the next parameter record */
	p->consumed = 0xffffffffu;
	p->started = 0;
	if (p->rings_enabled) {
		p->rings_enabled = 0;
		if (p->on_check_fail)
			p->on_check_fail(p->user, p->resync_mode);
	}
	return 0;
}

void mqa_stream_parse(struct mqa_stream_parser *p, unsigned pos, int sync_enabled)
{
	for (;;) {
		int fresh = 0;
		unsigned n, i;

		if (p->avail == 0)
			return;
		if (p->type < 0) {
			if (!parse_header(p, pos, sync_enabled))
				return;
			fresh = 1;
		}
		if (p->remaining == 0 && !fresh) {
			if (!end_message(p))
				return;
			continue;
		}
		if (p->avail == 0) {
			if (p->remaining)
				return;
			if (!end_message(p))
				return;
			continue;
		}

		n = min_u(p->avail, p->remaining);
		if (p->type == 4)
			handle_record(p, n);
		else if (p->type == 5 || p->type == 2)
			handle_payload(p, n);

		for (i = 0; i < n; i++)
			p->check = mqa_stream_check_update(p->check, p->fifo[i]);
		consume(p, n);
		p->remaining -= n;
		if (p->remaining)
			return;
		if (!end_message(p))
			return;
	}
}
