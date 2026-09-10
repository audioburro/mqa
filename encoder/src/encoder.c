/*
 * Assembling a stream -- see mqae/encoder.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqae/encoder.h"
#include "mqae/carrier.h"
#include "mqa/intake.h"

/* How far ahead of the end the terminate goes: its count is 17 bits, so
 * it must be inside 131071 frames, and the intake has to have parsed it
 * before the stream reaches the end; one hole's worth is plenty. */
#define TERMINATE_AHEAD 65536

void mqae_config_default(struct mqae_config *cfg)
{
	memset(cfg, 0, sizeof *cfg);
	cfg->src_rate = 48000;
	cfg->orig_rate = 96000;
	cfg->xbit = 0;
	cfg->render_filter = 8;
	cfg->render_bitdepth = 2;
	cfg->gain_index = 0;
	cfg->stage2_dither = 2;
	cfg->level = 15;
	cfg->scale_index = 25;
	cfg->carrier_class = 0;
	cfg->variant = 1;
	cfg->salt_select = 1;
	mqae_auth_none(&cfg->auth);
}

/* The opening datasync: what the stream is, and how to decode it. */
static void write_datasync(struct mqae_encoder *e)
{
	struct mqae_datasync ds;
	const struct mqae_config *c = &e->cfg;

	memset(&ds, 0, sizeof ds);
	ds.orig_rate = mqa_intake_rate_code(c->orig_rate);
	ds.src_rate = mqa_intake_rate_code(c->src_rate);
	ds.render_filter = c->render_filter;
	ds.render_bitdepth = c->render_bitdepth;
	ds.auth_info = c->auth.info;
	ds.auth_level = c->auth.level;
	ds.nitems = 2;
	ds.item[0].type = 0;
	ds.item[0].u.base.stage2_dither = c->stage2_dither;
	ds.item[0].u.base.gain_index = c->gain_index;
	ds.item[0].u.base.level = c->level;
	ds.item[0].u.base.lag = 127;                  /* no feedback term */
	ds.item[1].type = 1;
	ds.item[1].u.params.scale_index = c->scale_index;
	ds.item[1].u.params.carrier_class = c->carrier_class;
	ds.item[1].u.params.variant = c->variant;
	ds.item[1].u.params.salt_select = c->salt_select;
	mqae_bits_datasync(&e->bits, &ds);
}

int mqae_encoder_open(struct mqae_encoder *e, const struct mqae_config *cfg, uint64_t total)
{
	memset(e, 0, sizeof *e);
	e->cfg = *cfg;
	e->total = total;
	if (mqae_bits_init(&e->bits) < 0)
		return -1;
	if (mqae_chan_init(&e->chan) < 0) {
		mqae_bits_free(&e->bits);
		return -1;
	}
	write_datasync(e);
	/*
	 * A decoder will not start a stream until it has met a
	 * reconstruction packet, the packets that carry what a renderer
	 * downstream needs. This encoder has nothing to put in one, so it
	 * writes an empty one, which says there is no reconstruction data.
	 */
	mqae_bits_packet(&e->bits, MQA_BS_RECONSTRUCTION);
	mqae_bits_put(&e->bits, 0, 12);
	mqae_bits_end(&e->bits);

	/* the data channel opens with the parameter record, from which a
	 * decoder takes its scales and the two stages' initial state */
	mqae_chan_record(&e->chan, e->cfg.scale_index, NULL, NULL);
	return 0;
}

void mqae_encoder_close(struct mqae_encoder *e)
{
	mqae_bits_free(&e->bits);
	mqae_chan_free(&e->chan);
}

/*
 * Control bits out to `n`: the terminate packet when the end is close
 * enough to name, and holes the rest of the time.
 *
 * The last hole is sized to land on `n` rather than past it, so that
 * the channel's packet boundaries line up with the caller's frames --
 * which anything wanting to put a packet at a particular frame needs. A
 * gap of fewer than twelve bits cannot be filled by a hole at all, that
 * being the shortest one there is, so the stream can still overshoot by
 * up to eleven bits; nothing depends on an exact position.
 */
static void ensure_bits(struct mqae_encoder *e, uint64_t n)
{
	while (e->bits.nbits < n && !e->bits.failed) {
		uint64_t at = e->bits.nbits, room = n - at;

		if (e->total && !e->terminated && e->total > at + MQAE_BITS_TERMINATE &&
		    at + MQAE_BITS_TERMINATE + TERMINATE_AHEAD + 4096 >= e->total) {
			mqae_bits_terminate_at(&e->bits, e->total);
			e->terminated = 1;
			continue;
		}
		if (room > 4119)
			mqae_bits_hole(&e->bits, 4095);
		else if (room >= 12)
			mqae_bits_hole(&e->bits, (unsigned)(room - 12));
		else
			mqae_bits_hole(&e->bits, 0);
	}
}

void mqae_encoder_reserve(struct mqae_encoder *e, uint64_t frames)
{
	ensure_bits(e, frames);
}

void mqae_encoder_write(struct mqae_encoder *e, int32_t *lr, size_t n, int placed)
{
	const uint8_t *wire;
	size_t i;

	ensure_bits(e, e->frames + n);
	mqae_chan_pad_to(&e->chan, 2 * (size_t)(e->frames + n));
	wire = mqae_chan_wire(&e->chan, 2 * (size_t)(e->frames + n));
	if (e->bits.failed || e->chan.failed) {
		e->failed = 1;
		return;
	}
	for (i = 0; i < n; i++) {
		uint64_t at = e->frames + i;

		if (placed)
			mqae_carrier_bytes(&lr[2 * i], &lr[2 * i + 1],
					   wire[2 * at], wire[2 * at + 1]);
		else
			mqae_carrier_frame(&lr[2 * i], &lr[2 * i + 1], e->cfg.xbit,
					   mqae_bits_at(&e->bits, at),
					   wire[2 * at], wire[2 * at + 1]);
	}
	e->frames += n;
}
