/*
 * mqa/intake.c -- finding the stream in the samples and keeping the
 * packet record the decoder is driven by.
 *
 * The structure follows the reference decoder's intake call (one per
 * group): the stream start, then per group the lookahead bitstream
 * extraction, the sync bookkeeping, the group's sample capture and the
 * position update. Packet parsing itself is the library's bitstream
 * parser; what each packet does to the stream state is ported from the
 * reference's handlers.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/intake.h"
#include "mqa/residual_stage.h"     /* mqa_residual_scale_table */

#define FRAMES_PER_GROUP MQA_GROUP

/* --- the packets' effects ------------------------------------------------ */

/*
 * A datasync's item 0: the refinement level and the stage-2 parameters.
 * The reference builds a four-word level record from it and, when the
 * item announces where its parameters apply (a resync), remembers that
 * position as the pending sync.
 */
static const uint32_t default_level_record[4] = { 0, 0, 20 << 8, 0x1555 };

static void apply_item0(struct mqa_intake *in, const struct mqa_bs_packet *p, const struct mqa_bs_item *it)
{
	const struct mqa_bs_datasync *d = &p->u.datasync;
	unsigned u5 = it->u.base.level, u6 = it->u.base.lag;
	unsigned xbit = (unsigned)in->xbit;

	in->decode_disabled = in->no_decode;
	/* the level record: the level itself, and the conditioner's leaky
	 * feedback strength derived from the level, the gain index and a
	 * second level field (127: none) */
	in->level_record[0] = ~0u;
	in->level_record[2] = u5 << 8;
	if (u6 == 127) {
		in->level_record[1] = u5 << 8;
		in->level_record[3] = 0;
	} else {
		uint32_t v = (mqa_conditioner_gain_index(it->u.base.gain_index) >> (7 - xbit)) << (8 - xbit);
		int32_t strength = (int32_t)((u6 << 8) + (u5 << 8) - mqa_conditioner_lag_strength(v));

		in->level_record[1] = 0u - (u6 << 8);
		in->level_record[3] = (uint32_t)(strength & ~(strength >> 31));
	}
	in->pkt.alt_mode = 0;
	in->pkt.level = u5 << 8;
	if (!d->stream_pos_flag) {
		/* the stream's opening datasync: its parameters apply at once,
		 * and the conditioner is set up from them */
		uint32_t gain2 = mqa_conditioner_gain_index(it->u.base.gain_index) >> (7 - xbit);

		if (gain2 == 0)
			gain2 = 256u << xbit;
		mqa_conditioner_configure(&in->cond, d->src_rate, it->u.base.stage2_dither, xbit,
					  (int32_t)gain2, in->level_record);
		in->pkt.sync = 0;
		if (!in->decode_disabled) {
			in->first_sync = 1;
			in->synced_once = 1;
		}
		return;
	}
	{
		uint32_t at = it->u.base.start_pos * 32;
		unsigned words = 5, from = d->items_at + 48;

		if (in->stream_pos >= at || in->decode_disabled || !in->indicator)
			return;
		in->pending_sync = (int32_t)at;
		in->pending_sync_at = in->stream_pos;
		/* the resync marker: the item's bits beyond its 48 bits of
		 * fields, whole words */
		if (it->size >= 48)
			words = ((it->size - 48) >> 5) + 1;
		if (from + 32 * words <= 32 * MQA_BS_RAW_WORDS) {
			uint32_t table[MQA_CONDITIONER_MARKER_WORDS];
			unsigned i;

			for (i = 0; i < words && i < MQA_CONDITIONER_MARKER_WORDS; i++) {
				unsigned bit = from + 32 * i;
				uint64_t w = (uint64_t)p->raw[bit / 32] | (uint64_t)(bit / 32 + 1 < MQA_BS_RAW_WORDS ? p->raw[bit / 32 + 1] : 0) << 32;

				table[i] = (uint32_t)(w >> (bit % 32));
			}
			mqa_conditioner_set_marker(&in->cond, at, table, i);
		}
	}
}

/* Item 1: the carrier class, scale index, salt and the joining position. */
static void apply_item1(struct mqa_intake *in, const struct mqa_bs_datasync *d, const struct mqa_bs_item *it)
{
	if (in->decode_disabled)
		return;
	in->pkt.cls = (uint8_t)it->u.low.carrier_class;
	in->pkt.scale_index = (uint8_t)it->u.low.scale_index;
	in->pkt.alt_mode = 0;
	in->have_params = 1;
	in->pkt.decoding = 1;
	in->pkt.variant = (uint8_t)it->u.low.variant;
	in->pkt.salt_select = it->u.low.salt_select;
	if (!d->stream_pos_flag)
		return;
	in->pkt.sync_mode = (uint8_t)it->u.low.sync_mode;
	if (in->pending_sync == -1)
		return;
	in->pkt.consumed_hi = (uint32_t)(in->pending_sync + it->u.low.offset);
	in->pkt.has_consumed = 1;
	in->pkt.consumed_lo = (uint8_t)it->u.low.consumed_lo;
}

/* Item 2: the same without a class. */
static void apply_item2(struct mqa_intake *in, const struct mqa_bs_datasync *d, const struct mqa_bs_item *it)
{
	if (in->decode_disabled)
		return;
	in->pkt.scale_index = (uint8_t)it->u.low.scale_index;
	in->pkt.salt_select = it->u.low.salt_select;
	in->have_params = 1;
	in->pkt.decoding = 1;
	if (!d->stream_pos_flag || in->pending_sync == -1)
		return;
	in->pkt.consumed_hi = (uint32_t)(in->pending_sync + it->u.low.offset);
	in->pkt.has_consumed = 1;
	in->pkt.consumed_lo = (uint8_t)it->u.low.consumed_lo;
}

/* A datasync's header: the rates and the render descriptor. */
static void apply_datasync_header(struct mqa_intake *in, const struct mqa_bs_datasync *d)
{
	if (d->src_rate != in->rate_field) {
		/* the stream is not at the rate the decoder was opened for:
		 * the reference ends the stream */
		in->rate_changed = 1;
		return;
	}
	in->orig_rate = d->orig_rate;
	in->render_filter = d->render_filter;
	in->render_bitdepth = d->render_bitdepth;
	in->auth_level = d->auth_level;
	in->auth_info = d->auth_info;
	if (in->pkt.fresh)
		in->auth_byte = (d->auth_info | d->auth_level << 4) & 0xff;
	in->pkt.format_field = d->orig_rate;
	in->pkt.descriptor = (uint16_t)((d->render_filter | d->unknown_1 << 5 | d->render_bitdepth << 7) & 0x3ff);
	in->ds_flag = d->unknown_2 & 1;
}

/*
 * A datasync's items take effect one by one as each one's bits arrive,
 * as in the reference (the last item of a resync's datasync often lands
 * a group after the first); the completed packet is not acted on again.
 */
static void on_item(void *user, const struct mqa_bs_packet *p, unsigned index)
{
	struct mqa_intake *in = user;
	const struct mqa_bs_datasync *d = &p->u.datasync;

	if (index == 0) {
		if (in->pkt.fresh) {
			/* a fresh stream's first datasync: the conditioner is set
			 * up from its header before the packet is acted on */
			unsigned xbit = (unsigned)in->xbit;

			mqa_conditioner_configure(&in->cond, d->src_rate, 0, xbit,
						  (int32_t)(256u << xbit), default_level_record);
		}
		apply_datasync_header(in, d);
	}
	if (in->rate_changed || index >= MQA_BS_MAX_ITEMS)
		return;
	switch (d->item[index].type) {
	case 0: apply_item0(in, p, &d->item[index]); break;
	case 1: apply_item1(in, d, &d->item[index]); break;
	case 2: apply_item2(in, d, &d->item[index]); break;
	default: break;
	}
}

static void on_packet(void *user, const struct mqa_bs_packet *p)
{
	struct mqa_intake *in = user;

	switch (p->type) {
	case MQA_BS_DATASYNC:
		break;                                /* acted on item by item */
	case MQA_BS_TERMINATE:
		/* the stream ends this many frames after the packet */
		in->end_pos = (uint32_t)(in->bs.abspos + p->u.terminate.bits_to_end);
		break;
	default:
		break;
	}
}

/* The reference marks the stream aligned when it meets a reconstruction
 * packet's header; that gates the first group's decoding. */
static void on_header(void *user, enum mqa_bs_type type, uint64_t offset)
{
	struct mqa_intake *in = user;

	(void)offset;
	if (type == MQA_BS_RECONSTRUCTION)
		in->aligned = 1;
}

/* Run the bitstream parser over n frames of the rings. */
/* A reconstruction packet's payload: its bits go to the conditioner's
 * ring once the frames carrying them have been extracted. */
static void on_span(void *user, uint64_t first, uint64_t end)
{
	struct mqa_intake *in = user;

	if (in->nspans < MQA_INTAKE_SPANS) {
		in->span[in->nspans].next = first;
		in->span[in->nspans].end = end;
		in->nspans++;
	}
}

static void extract(struct mqa_intake *in, const int32_t *a, const int32_t *b, unsigned n)
{
	unsigned i;

	/* the channel's bits are kept by stream position for the payloads */
	for (i = 0; i < n; i++) {
		unsigned bit = ((uint32_t)(a[i] ^ b[i]) >> (8 + in->xbit)) & 1u;
		unsigned at = (unsigned)(in->extracted + i) % MQA_BITRING_BITS;

		if (bit)
			in->channel[at / 32] |= 1u << (at % 32);
		else
			in->channel[at / 32] &= ~(1u << (at % 32));
	}
	in->extracted += n;
	mqa_bitstream_feed(&in->bs, a, b, n);
	/* the payload bits now extracted, in stream order, while the stream
	 * has synced (the reference's condition at the time it appends) */
	while (in->nspans) {
		uint64_t lim = in->span[0].end < in->extracted ? in->span[0].end : in->extracted;

		while (in->span[0].next < lim) {
			unsigned at = (unsigned)in->span[0].next % MQA_BITRING_BITS;

			if (in->synced_once)
				mqa_bitring_append(&in->ring, (in->channel[at / 32] >> (at % 32)) & 1u);
			in->span[0].next++;
		}
		if (in->span[0].next < in->span[0].end)
			break;
		memmove(&in->span[0], &in->span[1], (in->nspans - 1) * sizeof in->span[0]);
		in->nspans--;
	}
}

/* --- the group's own samples ---------------------------------------------- */

/* The samples' low bytes are the data channel: captured, then cleared;
 * the conditioner then refills them, and the result is checksummed. The
 * reference also hashes the captured bytes and the samples' top bits for
 * the authentication (not implemented). */
static void capture_samples(struct mqa_intake *in, int32_t *a, int32_t *b, unsigned count)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		in->prefix[2 * i] = (uint8_t)a[i];
		a[i] &= ~0xff;
		in->prefix[2 * i + 1] = (uint8_t)b[i];
		b[i] &= ~0xff;
	}
	mqa_conditioner_run(&in->cond, a, b, count, &in->ring);
	for (i = 0; i < count; i++) {
		in->crc = mqa_crc32_word(in->crc, (uint32_t)a[i]);
		in->crc = mqa_crc32_word(in->crc, (uint32_t)b[i]);
	}
}

/*
 * The authentication indicator the decoder carries into its output: how
 * the stream authenticated (3 studio, 2 signed, 5 unsigned), or 0 when
 * the next authentication cannot complete before the current one runs
 * out. `at` is the position the indicator is wanted for; `claim` marks
 * the block it falls in as the one being waited for.
 */
static uint32_t auth_indicator(struct mqa_intake *in, uint32_t at, int claim)
{
	uint32_t block = ((at + MQA_INTAKE_AUTH_BLOCK - 1) & ~(MQA_INTAKE_AUTH_BLOCK - 1)) + MQA_INTAKE_AUTH_BLOCK;
	uint32_t blocks = ((block - (in->stream_pos & ~(MQA_INTAKE_AUTH_BLOCK - 1))) >> 16) - 1;

	if (claim || blocks < in->auth_pending)
		in->auth_pending = blocks;
	if (in->until_end < block - in->stream_pos)
		return 0;
	if (in->auth_byte > 143)
		return 3;                         /* a studio stream          */
	return in->auth_byte <= 15 ? 5 : 2;       /* unsigned, or signed      */
}

/* --- the stream start -------------------------------------------------------- */

static void stream_start(struct mqa_intake *in)
{
	struct mqa_packet *p = &in->pkt;

	p->descriptor = (uint16_t)(((p->descriptor & 0x3ff) & ~0x80) | 0x100);
	p->descriptor &= (uint16_t)~0x7fu;               /* low bits cleared for the datasync's */
	in->pending_sync = -1;
	p->format_field = in->rate_field;
	in->active = 1;
	in->indicator_enable = 0;
	in->auth_pending = 0;
	in->auth_state = 0;
	in->indicator = 1;
	in->auth_byte = 0;
	in->auth_pending = 0;
	in->synced_once = 0;
	in->stream_pos = 0;
	in->end_pos = ~0u;
	p->decoding = 0;
	p->has_consumed = 0;
	p->level = 0;
	p->fresh = 1;
	in->just_started = 1;
	in->aligned = 0;
	in->no_decode = 0;
	in->decode_disabled = 1;
	in->have_params = 0;
	p->salt_select = 1;
	p->variant = 0;
	in->crc = 0;
	mqa_bitring_reset(&in->ring);
	in->extracted = 0;
	in->nspans = 0;
	mqa_conditioner_configure(&in->cond, 0, 0, 0, 256, default_level_record);
}

/* --- the group ---------------------------------------------------------------- */

/* The 5-bit rate code of a sample rate: base index and power of two. */
unsigned mqa_intake_rate_code(unsigned hz)
{
	static const unsigned base[3] = { 44100, 48000, 64000 };
	unsigned i, k;

	for (i = 0; i < 3; i++)
		for (k = 0; k < 8; k++)
			if ((base[i] << k) == hz)
				return i << 3 | k;
	return 31;
}

void mqa_intake_init(struct mqa_intake *in, unsigned rate_hz)
{
	memset(in, 0, sizeof *in);
	mqa_bitstream_init(&in->bs, -1, on_packet, in);
	in->bs.on_header = on_header;
	in->bs.on_span = on_span;
	in->bs.on_item = on_item;
	in->bs.word_shift = 0;                            /* ring samples are 24-bit right-justified */
	in->rate_field = mqa_intake_rate_code(rate_hz);
	in->end_pos = ~0u;
	in->until_end = MQA_INTAKE_AUTH_WINDOW;   /* the decoder opens authenticated */
	in->pending_sync = -1;
	in->pkt.sync = -1;
	in->pkt.length = ~0u;
}

/*
 * Two of the record's fields are stream state the reference keeps in the
 * record itself: whether a sync has been announced before, and the
 * authentication indicator. The decoder reads them from the record, so
 * they are copied over whenever the record is handed out.
 */
static void sync_record(struct mqa_intake *in)
{
	in->pkt.flag5 = (uint8_t)in->synced_once;
	in->pkt.format = (uint32_t)in->indicator;
	/* the carrier's shift is the channel bit itself: a stream that hides
	 * its control channel higher up leaves the carrier that much shorter */
	in->pkt.shift = (uint32_t)in->xbit;
}

/*
 * The group's record: the reference's intake for a started stream.
 * `a`/`b` are the group's slot, `available` the frames from there.
 */
static int started_group(struct mqa_intake *in, int32_t *a, int32_t *b, unsigned available)
{
	struct mqa_packet *p = &in->pkt;
	unsigned count, seen;
	int end_reached;

	p->has_consumed = p->has_consumed ? in->just_started : 0;
	p->present = 1;
	p->fresh = (uint8_t)in->just_started;
	in->first_sync = in->just_started ? in->stream_pos <= 1 : 0;

	count = available < FRAMES_PER_GROUP ? available : FRAMES_PER_GROUP;
	if (in->end_pos <= in->stream_pos + count)
		count = in->end_pos - in->stream_pos;
	p->sync = -1;
	if (in->end_pos > in->stream_pos + count)
		count &= ~31u;
	p->count = count;

	/* the bitstream runs 480 frames ahead of the group */
	seen = in->just_started;                          /* (the fresh flag the reference tests) */
	if (in->end_pos > in->stream_pos + MQA_INTAKE_LOOKAHEAD) {
		unsigned frames = in->end_pos - MQA_INTAKE_LOOKAHEAD - in->stream_pos;

		if (frames > FRAMES_PER_GROUP)
			frames = FRAMES_PER_GROUP;
		if (available < frames + MQA_INTAKE_LOOKAHEAD) {
			p->count = 0;                     /* not enough lookahead yet */
			return 0;
		}
		extract(in, a + MQA_INTAKE_LOOKAHEAD, b + MQA_INTAKE_LOOKAHEAD, frames);
		if (in->rate_changed) {
			p->present = 0;
			return 1;
		}
		seen = in->just_started;
	}
	if (seen && !in->aligned) {
		/* a fresh stream that did not align: the reference tells its
		 * owner and hands over nothing */
		p->present = 0;
		return 1;
	}
	if (p->fresh) {
		/* the stream is aligned and starts here: how it authenticates
		 * is settled now, and travels in the record */
		uint32_t at = in->synced_once ? 0 : (in->stream_pos ? in->stream_pos + 1000 : 0);

		in->indicator = (int)auth_indicator(in, at, 1);
	}

	/* a pending sync position reached: announce it */
	if ((int32_t)in->stream_pos == in->pending_sync) {
		p->sync = in->pending_sync;
		in->first_sync = !in->synced_once;
		in->synced_once = 1;
		in->pending_sync = -1;
	}
	end_reached = in->stream_pos + count >= in->end_pos;
	if (!in->decode_disabled)
		capture_samples(in, a, b, count);
	in->until_end = in->until_end > count ? in->until_end - count : 0;
	in->stream_pos += count;
	if ((in->stream_pos & (MQA_INTAKE_AUTH_BLOCK - 1)) == 0 && in->end_pos - in->stream_pos >= MQA_INTAKE_AUTH_BLOCK) {
		/* a block is complete: its checksum goes to the authentication,
		 * which the library takes as passing (see intake.h) */
		in->crc = 0;
		in->until_end = MQA_INTAKE_AUTH_WINDOW;
	}
	in->just_started = 0;
	p->length = in->end_pos;
	if (end_reached) {
		/* the stream's end: the reference hashes what is left (the
		 * last authentication, which the library takes as passing),
		 * tells its owner and drops the stream. The next group looks
		 * for a new one. */
		in->crc = 0;
		in->until_end = MQA_INTAKE_AUTH_WINDOW;
		in->active = 0;
		in->have_params = 0;
	}
	return 1;
}

int mqa_intake_group(struct mqa_intake *in, int32_t *a, int32_t *b, unsigned available)
{
	struct mqa_packet *p = &in->pkt;
	int rc;

	p->prefix = in->prefix;
	if (in->active) {
		rc = started_group(in, a, b, available);
		sync_record(in);
		return rc;
	}

	/* no stream yet: look for one starting in this group's frames. The
	 * reference matches the low 32 bits of the magic over the group and
	 * the next one, for each candidate bit in turn */
	{
		unsigned frames = available < FRAMES_PER_GROUP ? available : FRAMES_PER_GROUP;
		unsigned window = available < frames + FRAMES_PER_GROUP ? available : frames + FRAMES_PER_GROUP;
		int start = -1, bit;

		if (frames == 0)
			return 0;                         /* no frames: nothing for the decoder to do */
		p->present = 0;
		for (bit = 0; bit < 8 && start < 0; bit++) {
			uint32_t reg = 0;
			unsigned i;

			for (i = 0; i < window; i++) {
				reg = (reg >> 1) | (((uint32_t)(a[i] ^ b[i]) >> (bit + 8)) & 1u) << 31;
				if (i >= 31 && reg == (uint32_t)MQA_BS_MAGIC) {
					start = (int)i - 31;
					in->xbit = bit;
					break;
				}
			}
		}
		if (start < 0) {
			p->count = frames;                /* no stream: these frames pass through */
			sync_record(in);
			return 1;
		}
		if (start > 0) {
			/* the stream starts inside this group: the frames before
			 * it pass through first */
			p->count = (unsigned)start < frames ? (unsigned)start : frames;
			sync_record(in);
			return 1;
		}
		if (available < 512) {
			p->count = frames;                /* not enough to start on */
			sync_record(in);
			return 1;
		}
		stream_start(in);
		mqa_bitstream_reset(&in->bs);
		in->bs.xbit = in->xbit + MQA_BS_MIN_BIT;
		/* the reference then reads the lookahead: 480 frames from the slot */
		extract(in, a, b, MQA_INTAKE_LOOKAHEAD);
		p->count = FRAMES_PER_GROUP;
		rc = started_group(in, a, b, available);
		sync_record(in);
		return rc;
	}
}
