/*
 * mqa/bitstream.c -- finding and parsing the MQA control bitstream.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include "mqa/bitstream.h"

#define BUF_GROW 65536

const char *mqa_bs_type_name(enum mqa_bs_type t)
{
	static const char *const names[MQA_BS_NTYPES] = {
		"hole", "reconstruction", "data", "terminate", "authentication",
		"datasync", "packet_6", "metadata", "key",
	};

	return t < MQA_BS_NTYPES ? names[t] : "unknown";
}

int mqa_bitstream_init(struct mqa_bitstream *bs, int xbit, mqa_bs_packet_fn fn, void *user)
{
	memset(bs, 0, sizeof *bs);
	bs->xbit = xbit;
	bs->word_shift = MQA_BS_WORD_SHIFT;
	bs->on_packet = fn;
	bs->user = user;
	bs->cap = BUF_GROW;
	bs->buf = calloc(bs->cap / 8 + 1, 1);
	return bs->buf ? 0 : -1;
}

void mqa_bitstream_reset(struct mqa_bitstream *bs)
{
	bs->xbit = -1;
	bs->lost = 0;
	bs->seeded = 0;
	bs->rpos = bs->wpos = 0;
	bs->base = 0;
	bs->header_at = 0;
	bs->span_at = 0;
	bs->item_at = 0;
	memset(bs->sync, 0, sizeof bs->sync);
}

void mqa_bitstream_free(struct mqa_bitstream *bs)
{
	free(bs->buf);
	bs->buf = NULL;
}

/* --- the bit buffer ------------------------------------------------------ */

static void put_bit(struct mqa_bitstream *bs, unsigned bit)
{
	if (bs->wpos >= bs->cap) {
		/* drop what has been consumed, growing if that is not enough */
		uint64_t keep = bs->wpos - bs->rpos;
		size_t byte0 = (size_t)(bs->rpos / 8);

		if (byte0) {
			memmove(bs->buf, bs->buf + byte0, (size_t)((bs->wpos + 7) / 8) - byte0);
			bs->base += 8 * byte0;
			bs->rpos -= 8 * byte0;
			bs->wpos -= 8 * byte0;
		}
		if (bs->wpos >= bs->cap) {
			size_t ncap = bs->cap + BUF_GROW + (size_t)keep;
			uint8_t *nb = realloc(bs->buf, ncap / 8 + 1);

			if (!nb)
				return;
			memset(nb + bs->cap / 8 + 1, 0, ncap / 8 - bs->cap / 8);
			bs->buf = nb;
			bs->cap = ncap;
		}
	}
	if (bit)
		bs->buf[bs->wpos / 8] |= (uint8_t)(1u << (bs->wpos % 8));
	else
		bs->buf[bs->wpos / 8] &= (uint8_t)~(1u << (bs->wpos % 8));
	bs->wpos++;
}

/* A cursor for one parse attempt; `short_` is set when bits run out. */
struct cursor {
	const struct mqa_bitstream *bs;
	uint64_t pos;
	uint32_t csum;
	int short_;
};

static uint64_t bits(struct cursor *c, unsigned n)
{
	uint64_t v = 0;
	unsigned i;

	if (c->pos + n > c->bs->wpos) {
		c->short_ = 1;
		c->pos += n;
		return 0;
	}
	for (i = 0; i < n; i++, c->pos++) {
		unsigned bit = (c->bs->buf[c->pos / 8] >> (c->pos % 8)) & 1;
		unsigned x = c->csum & 1;

		v |= (uint64_t)bit << i;
		/* the checksum register: a shift register fed with every bit */
		c->csum = (c->csum >> 1) | ((uint32_t)bit << 31);
		if (x)
			c->csum ^= 3;
	}
	return v;
}

static int32_t sbits(struct cursor *c, unsigned n)
{
	uint64_t v = bits(c, n);

	return (int32_t)((v ^ (1ull << (n - 1))) - (1ull << (n - 1)));
}

static void skip(struct cursor *c, unsigned n)
{
	while (n) {
		unsigned k = n > 32 ? 32 : n;

		bits(c, k);
		n -= k;
	}
}

static void bytes(struct cursor *c, uint8_t *out, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++)
		out[i] = (uint8_t)bits(c, 8);
}

/* --- packets -------------------------------------------------------------- */

/* The packet's raw bits so far (for the parts copied verbatim). */
static void raw_copy(const struct mqa_bitstream *bs, uint64_t start, uint64_t end, struct mqa_bs_packet *p)
{
	unsigned i, n;

	if (end > bs->wpos)
		end = bs->wpos;
	n = end > start ? (unsigned)(end - start) : 0;
	if (n > 32 * MQA_BS_RAW_WORDS)
		n = 32 * MQA_BS_RAW_WORDS;
	memset(p->raw, 0, sizeof p->raw);
	for (i = 0; i < n; i++)
		if ((bs->buf[(start + i) / 8] >> ((start + i) % 8)) & 1)
			p->raw[i / 32] |= 1u << (i % 32);
}

static void parse_datasync(struct cursor *c, struct mqa_bitstream *bs, struct mqa_bs_packet *p, uint64_t start)
{
	struct mqa_bs_datasync *d = &p->u.datasync;
	unsigned i;

	skip(c, 36);                                  /* the magic */
	d->stream_pos_flag = (unsigned)bits(c, 1);
	skip(c, 1);
	d->orig_rate = (unsigned)bits(c, 5);
	d->src_rate = (unsigned)bits(c, 5);
	d->render_filter = (unsigned)bits(c, 5);
	d->unknown_1 = (unsigned)bits(c, 2);
	d->render_bitdepth = (unsigned)bits(c, 2);
	d->unknown_2 = (unsigned)bits(c, 4);
	d->auth_info = (unsigned)bits(c, 4);
	d->auth_level = (unsigned)bits(c, 4);
	d->item_count = (unsigned)bits(c, 7);
	for (i = 0; i < d->item_count; i++) {
		unsigned size = (unsigned)bits(c, 8);

		if (i < MQA_BS_MAX_ITEMS)
			d->item[i].size = size;
	}
	for (i = 0; i < d->item_count; i++) {
		unsigned type = (unsigned)bits(c, 8);

		if (i < MQA_BS_MAX_ITEMS)
			d->item[i].type = type;
	}
	d->stream_position = d->stream_pos_flag ? (uint32_t)bits(c, 32) : 0;
	d->items_at = (unsigned)(c->pos - start);
	for (i = 0; i < d->item_count; i++) {
		struct mqa_bs_item scratch, *it = i < MQA_BS_MAX_ITEMS ? &d->item[i] : &scratch;
		uint64_t item_start = c->pos;

		if (i >= MQA_BS_MAX_ITEMS) {
			skip(c, 0);
			continue;
		}
		switch (it->type) {
		case 0:
			it->u.base.stage2_dither = (unsigned)bits(c, 2);
			it->u.base.gain_index = (unsigned)bits(c, 4);
			it->u.base.level = (unsigned)bits(c, 7);
			it->u.base.lag = (unsigned)bits(c, 7);
			if (d->stream_pos_flag) {
				it->u.base.start_pos = (uint32_t)bits(c, 27);
				skip(c, 1);
			}
			break;
		case 1:
			it->u.low.scale_index = (unsigned)bits(c, 6);
			it->u.low.carrier_class = (unsigned)bits(c, 2);
			it->u.low.variant = (unsigned)bits(c, 1);
			it->u.low.salt_select = (unsigned)bits(c, 2);
			if (d->stream_pos_flag) {
				it->u.low.sync_mode = (unsigned)bits(c, 8);
				it->u.low.consumed_lo = (unsigned)bits(c, 1);
				it->u.low.offset = sbits(c, 12);
			}
			break;
		case 2:
			it->u.low.scale_index = (unsigned)bits(c, 6);
			it->u.low.salt_select = (unsigned)bits(c, 2);
			if (d->stream_pos_flag) {
				it->u.low.sync_mode = (unsigned)bits(c, 3);
				it->u.low.consumed_lo = (unsigned)bits(c, 1);
				it->u.low.offset = sbits(c, 12);
			}
			break;
		case 3:
			it->u.cipher.iv = bits(c, 32) | bits(c, 32) << 32;
			it->u.cipher.start_pos = (uint32_t)bits(c, 32);
			break;
		default:
			break;
		}
		/* the item's declared size wins: skip whatever is left of it */
		if (c->pos - item_start < it->size)
			skip(c, (unsigned)(it->size - (c->pos - item_start)));
		if (c->short_)
			break;
		/* the item is in: report it now, once */
		if (bs->on_item && !(bs->item_at == p->offset + 1 && bs->item_index >= i)) {
			bs->item_at = p->offset + 1;
			bs->item_index = i;
			p->bits = (unsigned)(c->pos - start);
			raw_copy(bs, start, c->pos, p);
			bs->on_item(bs->user, p, i);
		}
	}
}

/* Parse one packet at the read position; returns 1 when complete, 0
 * when more bits are needed, -1 on a checksum error. */
static int parse_packet(struct mqa_bitstream *bs, struct mqa_bs_packet *p)
{
	struct cursor c = { bs, bs->rpos, (uint32_t)(bs->abspos & 15), 0 };
	uint64_t start = c.pos;
	unsigned pad, size;

	memset(p, 0, sizeof *p);
	p->offset = bs->base + start;
	p->checksum_seed = (unsigned)(bs->abspos & 15);
	p->type = (enum mqa_bs_type)bits(&c, 4);
	if (c.short_)
		return 0;
	if (bs->on_header && bs->header_at != p->offset + 1) {
		bs->header_at = p->offset + 1;
		bs->on_header(bs->user, p->type, p->offset);
	}
	switch (p->type) {
	case MQA_BS_HOLE:
		size = (unsigned)bits(&c, 4);
		if (size == 15)
			size = (unsigned)bits(&c, 12);
		p->u.sized.size = size;
		skip(&c, size);
		break;
	case MQA_BS_RECONSTRUCTION:
	case MQA_BS_PACKET_6:
		p->u.sized.size = size = (unsigned)bits(&c, 12);
		if (p->type == MQA_BS_RECONSTRUCTION && !c.short_ && bs->on_span && bs->span_at != p->offset + 1) {
			bs->span_at = p->offset + 1;
			bs->on_span(bs->user, p->offset + 16, p->offset + 16 + size);
		}
		skip(&c, size);
		break;
	case MQA_BS_DATA:
		p->u.sized.unknown = (unsigned)bits(&c, 8);
		p->u.sized.size = size = (unsigned)bits(&c, 12);
		skip(&c, size);
		break;
	case MQA_BS_TERMINATE:
		p->u.terminate.bits_to_end = (uint32_t)bits(&c, 17);
		break;
	case MQA_BS_AUTHENTICATION:
		p->u.auth.auth_level = (unsigned)bits(&c, 4);
		bytes(&c, p->u.auth.data, 384);
		break;
	case MQA_BS_DATASYNC:
		parse_datasync(&c, bs, p, start);
		break;
	case MQA_BS_METADATA: {
		struct mqa_bs_metadata *m = &p->u.metadata;

		m->metadata_type = (unsigned)bits(&c, 7);
		m->is_last = (unsigned)bits(&c, 1);
		m->fragment_number = (unsigned)bits(&c, 12);
		m->size = (unsigned)bits(&c, 8) + 1;
		bytes(&c, m->data, m->size);
		break;
	}
	case MQA_BS_KEY:
		p->u.sized.size = size = (unsigned)bits(&c, 12);
		p->u.sized.unknown = (unsigned)bits(&c, 32);
		skip(&c, size >= 32 ? size - 32 : 0);
		break;
	default:
		return -1;                            /* not a packet: lost sync */
	}
	if (c.short_)
		return 0;
	raw_copy(bs, start, c.pos, p);
	/* the checksum covers the packet padded with zero bits to a word;
	 * the register then runs on through the checksum field itself */
	pad = (unsigned)(-(c.pos - start) & 31);
	{
		unsigned i, expect;

		for (i = 0; i < pad; i++) {
			unsigned x = c.csum & 1;

			c.csum >>= 1;
			if (x)
				c.csum ^= 3;
		}
		expect = c.csum & 15;
		p->checksum_ok = 0;
		p->checksum = (unsigned)bits(&c, 4);
		if (c.short_)
			return 0;
		p->checksum_ok = expect == p->checksum;
	}
	p->bits = (unsigned)(c.pos - start);
	if (!p->checksum_ok)
		return -1;

	bs->rpos = c.pos;
	/* the next packet's position: a datasync that announces one resets it */
	if (p->type == MQA_BS_DATASYNC && p->u.datasync.stream_pos_flag)
		bs->abspos = p->u.datasync.stream_position;
	bs->abspos += p->bits;
	return 1;
}

/* --- feeding -------------------------------------------------------------- */

static void resync_from(struct mqa_bitstream *bs, uint64_t pos)
{
	/* forget the buffered bits: the search restarts on fresh frames,
	 * one bit later than the failed packet */
	bs->rpos = bs->wpos = pos;
	bs->lost = 1;
	memset(bs->sync, 0, sizeof bs->sync);
}

static void on_sync(struct mqa_bitstream *bs, int bit)
{
	bs->xbit = bit;
	bs->lost = 0;
	bs->base = 0;
	bs->rpos = bs->wpos = 0;
	bs->sync_frame = bs->frames + 1 - MQA_BS_MAGIC_BITS;
	bs->seeded = 0;
}

/* Bits at a position, without touching the checksum register. */
static uint64_t peek(const struct mqa_bitstream *bs, uint64_t pos, unsigned n)
{
	uint64_t v = 0;
	unsigned i;

	for (i = 0; i < n; i++, pos++)
		v |= (uint64_t)((bs->buf[pos / 8] >> (pos % 8)) & 1) << i;
	return v;
}

/*
 * Every packet's checksum register starts from the low bits of the
 * packet's absolute bit position in the stream. The opening datasync's
 * position is the one it announces (0 when it carries none): peek it
 * once that much of the packet is in.
 */
static int seed_checksum(struct mqa_bitstream *bs)
{
	uint64_t pos = 0;
	unsigned items;

	if (bs->wpos < 80)
		return 0;
	if (peek(bs, 40, 1)) {
		items = (unsigned)peek(bs, 73, 7);
		if (bs->wpos < 80 + 16 * items + 32)
			return 0;
		pos = peek(bs, 80 + 16 * items, 32);
	}
	bs->abspos = pos;
	bs->seeded = 1;
	return 1;
}

static void drain(struct mqa_bitstream *bs)
{
	struct mqa_bs_packet p;
	int r;

	if (!bs->seeded && !seed_checksum(bs))
		return;
	while ((r = parse_packet(bs, &p)) != 0) {
		if (r < 0) {
			bs->errors++;
			resync_from(bs, bs->rpos + 1);
			return;
		}
		bs->packets++;
		if (bs->on_packet)
			bs->on_packet(bs->user, &p);
	}
}

int mqa_bitstream_feed(struct mqa_bitstream *bs, const int32_t *l, const int32_t *r, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++, bs->frames++) {
		uint32_t x = (uint32_t)(l[i] ^ r[i]);

		if (bs->xbit < 0 || bs->lost) {
			/* searching: every candidate bit keeps its own history */
			int b, lo = bs->xbit < 0 ? MQA_BS_MIN_BIT : bs->xbit;
			int hi = bs->xbit < 0 ? MQA_BS_MAX_BIT : bs->xbit;

			for (b = lo; b <= hi; b++) {
				uint64_t *reg = &bs->sync[b - MQA_BS_MIN_BIT];

				*reg = (*reg >> 1) | ((uint64_t)((x >> (b + bs->word_shift)) & 1) << (MQA_BS_MAGIC_BITS - 1));
				if (*reg == MQA_BS_MAGIC) {
					unsigned k;

					on_sync(bs, b);
					for (k = 0; k < MQA_BS_MAGIC_BITS; k++)
						put_bit(bs, (unsigned)(MQA_BS_MAGIC >> k) & 1);
					break;
				}
			}
			continue;
		}
		put_bit(bs, (x >> (bs->xbit + bs->word_shift)) & 1);
		if (bs->wpos - bs->rpos >= 32 * 1024 || (bs->wpos & 1023) == 0)
			drain(bs);
	}
	if (bs->xbit >= 0 && !bs->lost)
		drain(bs);
	return bs->xbit >= 0;
}

int mqa_bitstream_feed_interleaved(struct mqa_bitstream *bs, const int32_t *lr, size_t n)
{
	int32_t l[256], r[256];
	size_t done = 0;
	int found = 0;

	while (done < n) {
		size_t k = n - done < 256 ? n - done : 256, i;

		for (i = 0; i < k; i++) {
			l[i] = lr[2 * (done + i)];
			r[i] = lr[2 * (done + i) + 1];
		}
		found = mqa_bitstream_feed(bs, l, r, k);
		done += k;
	}
	return found;
}
