/*
 * The stage-1 decoder's group orchestration -- see include/mqa/decoder.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/decoder.h"
#include "mqa/lcg.h"
#include "mqa/carrier.h"

/* --- parser callbacks ------------------------------------------------- */

static void on_record(void *user, const struct mqa_stream_record *rec)
{
	struct mqa_decoder *d = user;

	/* both rings restart */
	d->sym_ring.wpos = d->sym_ring.cursor = 0;
	d->aux_ring.wpos = d->aux_ring.cursor = 0;
	/* the refinement stage: a new step, from the stream id's scale index */
	d->refine.step_request = mqa_residual_scale_table[(rec->id >> 6) & 63];
	d->refine.counter = 0;
	d->refine.seek = 0;
	d->refine.pending = 1;
	/* the residual stage: scale indices and the predictor seeds */
	mqa_residual_stage_configure(&d->residual, (rec->id >> 12) & 63, (rec->id >> 18) & 63,
				     -1, rec->a, rec->a_len);
	/* the output stage restarts from part B, with the gain from the
	 * stream's scale index if the record's id agrees with it in all three
	 * 6-bit fields, else a token scale of 3 */
	{
		struct mqa_output_restart r;
		uint32_t idx = d->params.scale_index & 63;
		uint32_t rep = idx << 6 | idx << 12 | idx << 18;
		int match = !((rec->id ^ rep) & 0xfc0) && !((rec->id ^ rep) & 0x3f000) &&
			    !(((rec->id >> 16) ^ (rep >> 16)) & 0xfc);

		memset(&r, 0, sizeof r);
		r.flavour = 0;
		r.valid = 1;
		r.descriptor = d->params.descriptor;
		r.variant = d->params.variant;
		r.salt_select = d->params.salt_select;
		r.scale = match ? (int32_t)mqa_residual_scale_table[d->params.scale_index & 63] : 3;
		r.kernel_param = d->params.kernel_param;
		r.format_field = d->params.format_field;
		memcpy(r.part_b, rec->b, sizeof r.part_b);
		if (mqa_output_stage_restart(&d->output, &r) < 0) {
			d->unsupported = 1;
			d->unsupported_why = "record restart form";
		}
	}
}

static void on_sync(void *user, const struct mqa_stream_sync *s)
{
	struct mqa_decoder *d = user;

	if (s->kind == 2) {
		d->refine.step_request = mqa_residual_scale_table[(s->id >> 6) & 63];
		d->refine.counter = s->pos;
		d->refine.seek = (int32_t)d->aux_ring.wpos;
		d->refine.pending = 1;
	} else if (s->kind == 5) {
		mqa_residual_stage_configure(&d->residual, (s->id >> 12) & 63, (s->id >> 18) & 63,
					     (int32_t)d->sym_ring.wpos, NULL, 0);
	}
	if (s->flag) {
		d->output.crc_armed = 1;
		d->output.crc_expect = s->word;
	}
}

/* Back to waiting for a packet start. */
static void stream_reset(struct mqa_decoder *d)
{
	d->primed = 0;
	d->mode = 0;
	d->resync_mode = 0;
}

/* The stream ended: tell the owner once, and drop the indicator. */
static void announce_end(struct mqa_decoder *d)
{
	if (!d->notify_enable)
		return;
	d->notify_enable = 0;
	d->output.indicator = 0;
	d->ended = 1;
}

/*
 * A sync message for a different stream: this one is over; what the
 * ring holds is flushed through and the decoder waits for a new start.
 */
static void on_sync_mismatch(void *user)
{
	struct mqa_decoder *d = user;

	announce_end(d);
	stream_reset(d);
	d->flush_pos = d->fill;
}

static void on_check_fail(void *user, uint8_t resync_mode)
{
	struct mqa_decoder *d = user;

	if (resync_mode)
		mqa_residual_stage_reset(&d->residual);
	d->refine.step = 0;
	d->refine.pending = 0;
}

void mqa_decoder_init(struct mqa_decoder *d, const struct mqa_carrier_class *cls)
{
	unsigned i;

	memset(d, 0, sizeof *d);
	d->sym_ring.data = d->sym_data;
	d->sym_ring.size = sizeof d->sym_data;
	d->aux_ring.data = d->aux_data;
	d->aux_ring.size = sizeof d->aux_data;
	mqa_descrambler_init(&d->descrambler);
	mqa_stream_init(&d->parser, &d->sym_ring, &d->aux_ring);
	d->parser.on_record = on_record;
	d->parser.on_sync = on_sync;
	d->parser.on_sync_mismatch = on_sync_mismatch;
	d->parser.on_check_fail = on_check_fail;
	d->parser.user = d;
	d->refine.ring = &d->aux_ring;
	d->refine.ch[0].coef = d->refine.ch[1].coef = mqa_refine_coefs_default;
	mqa_residual_stage_init(&d->residual, &d->sym_ring, cls);
	mqa_output_stage_init(&d->output);
	d->input_end = ~0u;
	d->flush_pos = ~0u;
	/* the output stage reads the carrier one frame behind the decoder's
	 * position, so it opens one frame behind the ring's start */
	d->output_ring_pos = MQA_RING_WORDS - 1;
	/* the noise sources start seeded from zero, as the reference's open
	 * leaves them; a packet start reseeds them from the stream */
	d->refine.lcg[0] = mqa_nr_lcg_step(0);
	d->refine.lcg[1] = mqa_nr_lcg_step(d->refine.lcg[0]);
	for (i = 0; i < 2; i++) {
		d->residual.ch[i].predictor.noise[0].state = d->refine.lcg[0];
		d->residual.ch[i].predictor.noise[1].state = d->refine.lcg[1];
	}
	mqa_output_stage_open(&d->output, 31);
}

void mqa_decoder_set_input_rate(struct mqa_decoder *d, unsigned rate_code)
{
	d->flags = (uint8_t)((rate_code & 0x1f) << 1);
	mqa_output_stage_open(&d->output, rate_code);
}

/* --- the group ------------------------------------------------------------ */

/* Descramble and parse until the data channel is `ahead` samples past
 * the reconstruction position. */
static void descramble_to(struct mqa_decoder *d, unsigned target, unsigned parse_pos, int sync_by_items)
{
	while (d->descramble_pos != target) {
		unsigned dist = target >= d->descramble_pos ? target - d->descramble_pos
						       : target + MQA_RING_WORDS - d->descramble_pos;
		unsigned room = (MQA_STREAM_FIFO - d->parser.avail) / 2;
		unsigned n = dist < room ? dist : room;
		int sync = sync_by_items ? d->parser.items > 3 : 0;

		if (!d->parser.started)
			return;
		/* a full FIFO gives an empty chunk; the parse must then make
		 * room, or the stream is stuck in a way not modelled here */
		uint32_t consumed_before = d->parser.consumed;

		d->descramble_pos = mqa_descrambler_fill(&d->descrambler, d->parser.fifo + d->parser.avail,
							 d->ring_a, d->ring_b, d->descramble_pos, n);
		d->parser.avail += 2 * n;
		mqa_stream_parse(&d->parser, parse_pos, sync);
		if (!d->parser.started)
			return;              /* the parser gave the stream up */
		if (n == 0 && d->parser.consumed == consumed_before) {
			d->unsupported = 1;
			d->unsupported_why = "descrambler stall";
			return;
		}
	}
}

/* Refine, decode residuals and reconstruct `count` samples at ring index `at`. */
static int reconstruct(struct mqa_decoder *d, unsigned at, unsigned count, const uint8_t *digits,
		       unsigned format, int check_crc, int pass)
{
	int32_t p[MQA_RESIDUAL_GROUP], q[MQA_RESIDUAL_GROUP];
	int32_t a0[MQA_GROUP], b0[MQA_GROUP];
	int n;

	if (d->on_layer) {
		memcpy(a0, d->ring_a + at, count * sizeof *a0);
		memcpy(b0, d->ring_b + at, count * sizeof *b0);
	}
	mqa_refine_group(&d->refine, d->ring_a + at, d->ring_b + at, count);
	if (mqa_residual_stage_group(&d->residual, count, digits, p, q) == MQA_RESIDUAL_UNSUPPORTED) {
		d->unsupported_why = "the residual stage over a carrier-digit FIFO";
		return MQA_DECODER_UNSUPPORTED;
	}
	if (d->on_layer)
		d->on_layer(d->layer_user, a0, b0, d->ring_a + at, d->ring_b + at, p, q, count);
	n = mqa_output_stage_group(&d->output, d->ring_a + at, d->ring_b + at, count, p, q,
				   d->out_l, d->out_r, format, check_crc, pass,
				   d->ring_a + at + count, d->ring_b + at + count,
				   &d->output_ring_pos);
	if (n < 0) {
		d->unsupported_why = d->output.kernel ? "the alternative reconstruction kernel" : "the output stage";
		return MQA_DECODER_UNSUPPORTED;
	}
	d->out_l += n;
	d->out_r += n;
	d->out_count += (unsigned)n;
	return 0;
}

/* --- packet start ------------------------------------------------------- */

#define SALT_MAIN 0x3895afe1u
#define SALT_ALT  0xf807b7dfu

/* The packet-start handler: install the stream parameters and restart
 * the stages. `with_digits` says the stream is being joined with its
 * leading carrier digits in hand. */
static int packet_start(struct mqa_decoder *d, const struct mqa_packet *pkt, int with_digits,
			const uint8_t *digits)
{
	struct mqa_output_restart r;
	unsigned set = (d->params.kernel_param & 7) * 3 + (d->params.kernel_param >> 3);
	int32_t level = (int32_t)(0x30000 - pkt->level);

	d->params.format_field = pkt->format_field;
	d->params.descriptor = pkt->descriptor;
	d->params.kernel_param = (d->flags >> 1) & 0x1f;
	d->params.variant = pkt->variant;
	d->params.salt_select = pkt->salt_select;
	d->params.scale_index = pkt->scale_index;
	d->params.cls = pkt->cls;
	d->params.shift = pkt->shift;
	set = (d->params.kernel_param & 7) * 3 + (d->params.kernel_param >> 3);
	if (set > 3)
		set = 3;
	d->aux_ring.cursor = 0;
	d->sym_ring.wpos = d->sym_ring.cursor = 0;
	d->aux_ring.wpos = 0;

	memset(&r, 0, sizeof r);
	r.flavour = 1;
	r.valid = pkt->decoding;
	r.descriptor = pkt->descriptor;
	r.variant = pkt->variant;
	r.salt_select = pkt->salt_select;
	r.scale = mqa_residual_scale_table[pkt->scale_index & 63];
	r.kernel_param = d->params.kernel_param;
	r.format_field = pkt->format_field;
	r.position = d->stream_pos - pkt->count;

	if (with_digits || pkt->decoding) {
		int32_t gain = mqa_refine_gain_from_level(level + (pkt->variant ? (int32_t)0xffff0000 : (int32_t)0xffff6a52));

		mqa_refine_setup(&d->refine, -gain, (int32_t)(mqa_carrier_classes[pkt->cls].levels << pkt->shift),
				 set, SALT_MAIN);
		if (mqa_residual_stage_setup(&d->residual, &mqa_carrier_classes[pkt->cls], pkt->scale_index,
					     d->params.kernel_param, pkt->shift) < 0) {
			d->unsupported_why = "a carrier class with more than one digit level";
			return MQA_DECODER_UNSUPPORTED;
		}
		if (with_digits)
			mqa_residual_stage_start(&d->residual, 1, digits);
		else
			mqa_residual_stage_position(&d->residual, 0, 0xffffffffu, 0, 1, digits);
	} else if (pkt->alt_mode) {
		int32_t gain = mqa_refine_gain_from_level(level);
		uint32_t salt = pkt->salt_select == 0 ? 0 : pkt->salt_select == 1 ? SALT_ALT : SALT_MAIN;

		mqa_refine_setup(&d->refine, -gain, (int32_t)(1u << pkt->shift), set, salt);
	}
	if (mqa_output_stage_restart(&d->output, &r) < 0) {
		d->unsupported_why = "the alternative reconstruction kernel";
		return MQA_DECODER_UNSUPPORTED;
	}
	return 0;
}

/*
 * A packet's sync position arriving before the stream is in resync mode:
 * the refinement and residual stages are set up afresh from the stream
 * parameters and the residual stage positioned at the sync.
 */
static int realign(struct mqa_decoder *d, const struct mqa_packet *pkt, unsigned parse_pos,
		   const uint8_t *digits)
{
	unsigned set = (d->params.kernel_param & 7) * 3 + (d->params.kernel_param >> 3);
	int32_t level = (int32_t)(0x30000 - pkt->level);
	int32_t gain = mqa_refine_gain_from_level(level + (d->params.variant ? (int32_t)0xffff0000
								  : (int32_t)0xffff6a52));
	const struct mqa_carrier_class *cls = &mqa_carrier_classes[d->params.cls & 3];

	if (set > 3)
		set = 3;
	mqa_refine_setup(&d->refine, -gain, (int32_t)(cls->levels << d->params.shift), set, SALT_MAIN);
	if (mqa_residual_stage_setup(&d->residual, cls, d->params.scale_index, d->params.kernel_param,
				     d->params.shift) < 0)
		return -1;
	mqa_residual_stage_position(&d->residual, parse_pos, (uint32_t)pkt->sync, pkt->sync_mode, 1, digits);
	return 0;
}

/*
 * Samples the ring holds beyond `pos` before the input ends: the intake
 * records the ring position of the end of input, or all ones while the
 * input keeps coming.
 */
static unsigned available(const struct mqa_decoder *d, unsigned count)
{
	unsigned end = d->input_end;

	if (d->pos >= end) {
		if (end > 63)
			return count;
		end += MQA_RING_WORDS;
	}
	return count < end - d->pos ? count : end - d->pos;
}

/* Pass `count` ring samples straight through to the output. */
static int passthrough(struct mqa_decoder *d, unsigned count, unsigned format)
{
	int n = mqa_output_stage_passthrough(&d->output, d->ring_a + d->pos, d->ring_b + d->pos, count,
					     d->out_l, d->out_r, format, &d->output_ring_pos);

	if (n < 0)
		return -1;
	d->pos += count;
	d->out_l += n;
	d->out_r += n;
	d->out_count += (unsigned)n;
	d->passed += (unsigned long)n;
	return 0;
}

/*
 * The input is over: the owner has set the ring position to flush up to,
 * and what the ring holds before it (or before the input's end) passes
 * through; the marker clears once it is reached.
 */
static int flush(struct mqa_decoder *d)
{
	unsigned target = d->flush_pos >= d->pos ? d->flush_pos : MQA_RING_WORDS, count;

	if (target > d->input_end && d->pos < d->input_end)
		target = d->input_end;
	if (target == d->flush_pos)
		d->flush_pos = ~0u;
	count = target - d->pos;
	stream_reset(d);
	d->passthrough_pending = 0;
	return passthrough(d, count, 0);
}

/*
 * A group with no packet in it: the stream is over (or was never MQA),
 * so what the decoder has primed is finished and the rest passes
 * through untouched.
 */
static int no_packet(struct mqa_decoder *d, const struct mqa_packet *pkt)
{
	unsigned count;

	if (d->primed) {
		/* finish the primed group; the samples the packet-less group
		 * covers pass through on the next call */
		d->primed = 0;
		d->pos += MQA_GROUP;
		d->passthrough_pending = pkt->count;
		if (reconstruct(d, d->pos - MQA_GROUP, MQA_GROUP, NULL, pkt->format, 0, 2) < 0)
			return -1;
		return 1;
	}
	count = pkt->present ? pkt->count : available(d, pkt->count);
	stream_reset(d);
	return passthrough(d, count, pkt->format) < 0 ? -1 : 1;
}

int mqa_decoder_group(struct mqa_decoder *d, const struct mqa_packet *pkt)
{
	uint8_t digits[2 * MQA_GROUP];
	unsigned remaining, parse_pos, ahead, target, group_at;
	int crc_due, two_pass;

#define UNSUPPORTED(why) do { d->unsupported_why = (why); return MQA_DECODER_UNSUPPORTED; } while (0)
	if (d->flush_pos != ~0u) {
		if (flush(d) < 0)
			UNSUPPORTED("passthrough output");
		return 1;
	}
	if (d->passthrough_pending) {
		/* samples left over from a packet-less group */
		unsigned count = available(d, d->passthrough_pending);

		d->passthrough_pending -= count;
		if (passthrough(d, count, 0) < 0)
			UNSUPPORTED("passthrough output");
		return 1;
	}
	if (pkt->count == 0)
		return 0;                    /* nothing to do this call */
	if (!pkt->present) {
		if (no_packet(d, pkt) < 0)
			UNSUPPORTED("passthrough output");
		return 1;
	}
	if (pkt->fresh) {
		/* a packet start: the stream's mode, and either join it here
		 * (descrambling its leading bytes) or wait for alignment */
		int join = pkt->flag5 && pkt->count == d->stream_pos, with_digits;

		d->mode = pkt->decoding ? 2 : pkt->alt_mode;
		if (join && d->mode != 0) {
			d->parser.started = 1;
			d->parser.consumed = 0;
			mqa_descrambler_start(&d->descrambler, 0);
			mqa_descrambler_fill_bytes(&d->descrambler, d->parser.fifo, pkt->prefix, 0, 64);
			d->parser.avail = 64;
			d->descramble_pos = (d->pos + MQA_GROUP) % MQA_RING_WORDS;
			d->descramble_pos = mqa_descrambler_fill(&d->descrambler, d->parser.fifo + 64, d->ring_a,
								 d->ring_b, d->descramble_pos, MQA_GROUP);
			d->parser.avail += 64;
			with_digits = pkt->decoding;
		} else {
			d->parser.started = 0;
			d->parser.consumed = pkt->has_consumed ? 2 * pkt->consumed_hi + pkt->consumed_lo : 0xffffffffu;
			with_digits = pkt->decoding && join;
		}
		/* the stream is joined with a group of zero digits: the FIFO's
		 * write position and packing phase are what the join sets, not
		 * its contents (a realignment, below, is the case that hands it
		 * real ones) */
		memset(digits, 0, sizeof digits);
		d->resync_mode = with_digits ? 1 : 0;
		d->parser.type = -1;          /* no message in progress */
		d->parser.rings_enabled = 0;
		if (packet_start(d, pkt, with_digits, digits) < 0)
			return MQA_DECODER_UNSUPPORTED;  /* packet_start() said which */
	}
	if (d->mode == 0) {
		if (no_packet(d, pkt) < 0)
			UNSUPPORTED("passthrough output");
		return 1;
	}
	if (d->mode != 2)
		UNSUPPORTED("alignment mode");
	if (!d->primed) {
		d->primed = 1;
		return 1;
	}
	remaining = d->stream_pos - pkt->count;     /* samples of the packet already consumed */
	parse_pos = remaining - MQA_GROUP;
	if (pkt->sync != -1) {
		if (d->resync_mode == 0) {
			uint8_t sync_digits[MQA_GROUP];

			d->resync_mode = 1;
			mqa_carrier_digits(d->ring_a + d->pos, d->ring_b + d->pos, MQA_GROUP / 2, pkt->shift,
					   &mqa_carrier_classes[pkt->cls], sync_digits);
			if (realign(d, pkt, parse_pos, sync_digits) < 0)
				UNSUPPORTED("realignment form");
		} else {
			mqa_residual_stage_sync(&d->residual, (uint32_t)pkt->sync, pkt->sync_mode);
		}
	}
	two_pass = d->stream_pos >= pkt->length;
	crc_due = 0;
	if (!d->parser.started) {
		/* join the data channel where the intake says the stream's
		 * bytes have been consumed up to, once that point is within
		 * this group's leading bytes */
		uint32_t consumed = d->parser.consumed;

		if (pkt->has_consumed) {
			consumed = 2 * pkt->consumed_hi + pkt->consumed_lo;
			d->parser.consumed = consumed;
		}
		if ((consumed >> 1) < d->stream_pos && remaining <= (consumed >> 1)) {
			unsigned start = consumed - 2 * remaining;

			d->parser.started = 1;
			d->parser.items = 0;
			d->parser.type = -1;
			mqa_descrambler_start(&d->descrambler, consumed);
			mqa_descrambler_fill_bytes(&d->descrambler, d->parser.fifo, pkt->prefix, start,
						   2 * pkt->count - start);
			d->parser.avail = 2 * pkt->count - start;
			d->descramble_pos = (d->pos + pkt->count + (d->mode == 2 ? MQA_GROUP : 0)) % MQA_RING_WORDS;
		}
	}
	if (d->parser.started) {
		/* run the data channel ahead of this group; the output CRC is
		 * checked at every 4096-sample boundary of the parse position,
		 * and once more at the packet's very end */
		ahead = pkt->length - parse_pos;
		if (ahead > MQA_DESCRAMBLE_AHEAD)
			ahead = MQA_DESCRAMBLE_AHEAD;
		target = (d->pos + ahead) % MQA_RING_WORDS;
		descramble_to(d, target, parse_pos, d->resync_mode != 0);
		if (d->unsupported)
			return MQA_DECODER_UNSUPPORTED;      /* a callback hit one (why is set) */
		if (d->parser.started)
			crc_due = pkt->length < parse_pos + pkt->count + 0x1000 ? two_pass
				: ((parse_pos + pkt->count) & 0xfff) == 0;
	}
	/* (with the data channel not started, the group decodes on the
	 * stages' current state alone) */

	/* the carrier digits come from half a group ahead; they feed the
	 * residual stage's digit FIFO, which only carrier classes with more
	 * than one level use (not implemented) */
	mqa_carrier_digits(d->ring_a + d->pos + MQA_GROUP / 2, d->ring_b + d->pos + MQA_GROUP / 2,
			   MQA_GROUP, pkt->shift, &mqa_carrier_classes[pkt->cls], digits);
	group_at = d->pos;
	d->pos += MQA_GROUP;
	if (reconstruct(d, group_at, MQA_GROUP, digits, pkt->format, crc_due && !two_pass, 0) < 0)
		return MQA_DECODER_UNSUPPORTED;      /* reconstruct() said which stage */
	if (two_pass) {
		/* the packet's last samples: one more, shorter pass over what
		 * remains after this group, after which the stream is over */
		unsigned rest = pkt->count;

		memset(digits, 0, sizeof digits);
		if (rest > MQA_GROUP / 2)
			mqa_carrier_digits(d->ring_a + d->pos + MQA_GROUP / 2, d->ring_b + d->pos + MQA_GROUP / 2,
					   rest - MQA_GROUP / 2, pkt->shift, &mqa_carrier_classes[pkt->cls], digits);
		group_at = d->pos;
		d->pos += rest;
		d->primed = 0;
		if (reconstruct(d, group_at, rest, digits, pkt->format, crc_due, 1) < 0)
			return MQA_DECODER_UNSUPPORTED;
		announce_end(d);
		stream_reset(d);
	}
	return 1;
#undef UNSUPPORTED
}
