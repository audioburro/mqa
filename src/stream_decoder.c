/*
 * mqa/stream_decoder.c -- feeding the rings and running groups.
 *
 * The rings hold 832 frames plus a 64-frame tail that mirrors the head,
 * so that a group and its lookahead can always be read contiguously.
 * Frames are written at the fill position (they may run into the tail);
 * committing them mirrors what landed in the first 64 positions into
 * the tail and, when the fill wraps, copies what landed in the tail back
 * to the head.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/stream_decoder.h"

#define RING MQA_RING_WORDS
#define TAIL MQA_RING_TAIL

void mqa_stream_decoder_init(struct mqa_stream_decoder *sd, unsigned rate_hz)
{
	memset(sd, 0, sizeof *sd);
	mqa_decoder_init(&sd->dec, &mqa_carrier_classes[0]);
	mqa_intake_init(&sd->in, rate_hz);
	mqa_decoder_set_input_rate(&sd->dec, mqa_intake_rate_code(rate_hz));
}

void mqa_stream_decoder_set_signalling(struct mqa_stream_decoder *sd, int on)
{
	sd->signalling = on;
	if (on)
		mqa_watermark_init(&sd->watermark);
}

unsigned mqa_stream_decoder_space(const struct mqa_stream_decoder *sd)
{
	const struct mqa_decoder *d = &sd->dec;
	/*
	 * Frames stay in the rings until the output stage has taken them (the
	 * decoder refines a group in place after the next one is read), so
	 * the room is measured from the fill to the output position, keeping
	 * one frame free so a full ring never looks empty.
	 */
	unsigned room = (d->output_ring_pos + RING - d->fill) % RING;
	unsigned contiguous = RING + TAIL - d->fill;

	return room < contiguous ? room : contiguous;
}

size_t mqa_stream_decoder_feed(struct mqa_stream_decoder *sd, const int32_t *lr, size_t n)
{
	struct mqa_decoder *d = &sd->dec;
	unsigned space = mqa_stream_decoder_space(sd);
	unsigned k = n < space ? (unsigned)n : space, i, fill = d->fill;

	for (i = 0; i < k; i++) {
		d->ring_a[fill + i] = lr[2 * i] >> 8;
		d->ring_b[fill + i] = lr[2 * i + 1] >> 8;
	}
	/* commit: mirror the head into the tail, and the tail back after a wrap */
	for (i = fill; i < fill + k && i < TAIL; i++) {
		d->ring_a[RING + i] = d->ring_a[i];
		d->ring_b[RING + i] = d->ring_b[i];
	}
	fill += k;
	if (fill >= RING) {
		fill -= RING;
		for (i = 0; i < fill; i++) {
			d->ring_a[i] = d->ring_a[RING + i];
			d->ring_b[i] = d->ring_b[RING + i];
		}
	}
	d->fill = fill;
	return k;
}

void mqa_stream_decoder_finish(struct mqa_stream_decoder *sd)
{
	sd->finishing = 1;
	sd->dec.input_end = sd->dec.fill;         /* the last frame the rings hold */
}

void mqa_stream_decoder_after_group(struct mqa_stream_decoder *sd)
{
	struct mqa_decoder *d = &sd->dec;

	if (d->pos >= RING) {
		unsigned n, k;

		d->pos -= RING;
		n = d->pos + (d->primed ? MQA_GROUP : 0);
		for (k = 0; k < n && k < TAIL; k++) {
			d->ring_a[k] = d->ring_a[RING + k];
			d->ring_b[k] = d->ring_b[RING + k];
		}
	}
}

/* Frames the rings hold from position `from` up to the fill. */
static unsigned held_from(const struct mqa_decoder *d, unsigned from)
{
	return d->fill >= from ? d->fill - from : d->fill + RING - from;
}

size_t mqa_stream_decoder_run(struct mqa_stream_decoder *sd, int32_t *out, size_t capacity)
{
	struct mqa_decoder *d = &sd->dec;
	size_t written = 0;

	for (;;) {
		unsigned slot, group_slot;
		unsigned avail;

		/* the position wraps before the group that follows it, as the
		 * reference's driver does, so the group just decoded leaves the
		 * ring as it found it */
		mqa_stream_decoder_after_group(sd);
		group_slot = d->pos + (d->primed ? MQA_GROUP : 0);   /* may be in the tail */
		slot = group_slot % RING;
		int32_t la[MQA_GROUP + MQA_INTAKE_LOOKAHEAD + MQA_GROUP], lb[MQA_GROUP + MQA_INTAKE_LOOKAHEAD + MQA_GROUP];
		int32_t *a = NULL, *b = NULL;
		unsigned i, n, produced;
		int rc, step = 0;

		avail = held_from(d, slot);
		/* a group needs its own frames and the intake's lookahead; with
		 * less than that in the rings the caller must feed more, or say
		 * the input has ended */
		if (!sd->finishing && avail < MQA_GROUP + MQA_INTAKE_LOOKAHEAD + MQA_GROUP)
			break;

		if (written + MQA_STREAM_DECODER_GROUP_MAX > capacity)
			break;
		if (avail > 0) {
			/* the intake reads the slot and 512 frames on: give it a
			 * contiguous view when that would cross the ring's end */
			if (group_slot + MQA_GROUP + MQA_INTAKE_LOOKAHEAD + MQA_GROUP <= RING + TAIL) {
				a = d->ring_a + group_slot;
				b = d->ring_b + group_slot;
			} else {
				for (i = 0; i < sizeof la / sizeof la[0]; i++) {
					la[i] = d->ring_a[(slot + i) % RING];
					lb[i] = d->ring_b[(slot + i) % RING];
				}
				a = la;
				b = lb;
			}
			if (avail > MQA_GROUP + MQA_INTAKE_LOOKAHEAD + MQA_GROUP)
				avail = MQA_GROUP + MQA_INTAKE_LOOKAHEAD + MQA_GROUP;
			step = mqa_intake_group(&sd->in, a, b, avail);
			if (step && a == la) {
				/* the group's own frames go back where the reference
				 * keeps them: at the ring's end that is the mirrored
				 * tail, which the next wrap copies down */
				for (i = 0; i < MQA_GROUP; i++) {
					d->ring_a[group_slot + i] = la[i];
					d->ring_b[group_slot + i] = lb[i];
				}
			}
		}
		if (!step) {
			/* nothing the intake can do with what is there */
			if (!sd->finishing)
				break;
			/* the input is over: what the rings still hold is flushed
			 * through, as the reference's owner does at a file's end */
			if (d->flush_pos == ~0u) {
				if (d->pos == d->fill)
					break;
				d->flush_pos = d->fill;
			}
		}
		d->stream_pos = sd->in.stream_pos;
		/* the reference keeps one flag for both: a stream is running,
		 * and the decoder still has its end to announce */
		d->notify_enable = sd->in.active;
		d->out_l = sd->out_l;
		d->out_r = sd->out_r;
		if (sd->before_group)
			sd->before_group(sd->user, sd);
		rc = mqa_decoder_group(d, &sd->in.pkt);
		if (!d->notify_enable)
			sd->in.active = 0;                /* the decoder ended the stream */
		sd->groups++;
		if (rc == MQA_DECODER_UNSUPPORTED) {
			/* the group used a path the library does not implement:
			 * it made no progress, so stop rather than spin */
			sd->declined++;
			break;
		}
		produced = (unsigned)(d->out_l - sd->out_l);
		if (sd->signalling && produced) {
			/* the stream's parameters travel in the signalling too */
			mqa_watermark_set_stream(&sd->watermark, sd->in.render_filter, sd->in.orig_rate,
						 sd->in.render_bitdepth, sd->in.auth_level >= 8);
			mqa_watermark_apply(&sd->watermark, sd->out_l, sd->out_r, produced);
		}
		if (sd->after_group)
			sd->after_group(sd->user, sd, sd->out_l, sd->out_r, produced);
		for (n = 0; n < produced; n++) {
			out[2 * (written + n)] = (int32_t)((uint32_t)sd->out_l[n] << 8);
			out[2 * (written + n) + 1] = (int32_t)((uint32_t)sd->out_r[n] << 8);
		}
		written += produced;
		if (step && sd->in.pkt.count == 0 && rc == 0)
			break;
		if (rc == 0 && !sd->finishing)
			break;
	}
	return written;
}
