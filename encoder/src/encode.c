/*
 * The whole encoder -- see mqae/encode.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqae/encode.h"
#include "mqae/carrier.h"
#include "mqa/intake.h"
#include "mqa/lcg.h"
#include "mqa/conditioner.h"
#include "mqa/residual_stage.h"

/* The salt a packet start gives the refinement (src/decoder.c). */
#define SALT_MAIN 0x3895afe1u

/*
 * The model: the conditioner and the refinement, set up exactly as the
 * decoder's packet start sets them up from the same datasync fields. If
 * this and the decoder ever disagree the encoder is working back from
 * the wrong carrier, which is what tests/test_encode.c checks.
 */
static void model_setup(struct mqae_encode *e)
{
	const struct mqae_config *c = &e->out.cfg;
	unsigned rate_code = mqa_intake_rate_code(c->src_rate);
	unsigned kernel_param = rate_code & 0x1f;
	unsigned set = (kernel_param & 7) * 3 + (kernel_param >> 3);
	uint32_t level_record[4];
	uint32_t gain2;
	int32_t level, gain;

	/* the conditioner, from datasync item 0 */
	level_record[0] = ~0u;
	level_record[1] = c->level << 8;
	level_record[2] = c->level << 8;
	level_record[3] = 0;                       /* no feedback term (lag 127) */
	gain2 = mqa_conditioner_gain_index(c->gain_index) >> (7 - c->xbit);
	if (gain2 == 0)
		gain2 = 256u << c->xbit;
	mqa_conditioner_configure(&e->cond, rate_code, c->stage2_dither, c->xbit,
				  (int32_t)gain2, level_record);

	/* the refinement, from the level and the carrier class */
	if (set > 3)
		set = 3;
	level = (int32_t)(0x30000 - (c->level << 8));
	gain = mqa_refine_gain_from_level(level + (c->variant ? (int32_t)0xffff0000
							     : (int32_t)0xffff6a52));
	if (mqae_refine_init(&e->refine, -gain,
			     (int32_t)(mqa_carrier_classes[c->carrier_class & 3].levels << c->xbit),
			     set, SALT_MAIN,
			     mqa_residual_scale_table[c->scale_index & 63]) < 0)
		e->failed = 1;
	/*
	 * What the two of them do to the carrier's level. The conditioner
	 * negates and doubles before applying its own (negative) gain, and
	 * the refinement scales by sixteen times its gain; together they
	 * come to almost exactly two.
	 */
	e->model_gain = -2.0 * (double)e->cond.gain / 4294967296.0;
	if (gain2 != 256)
		e->model_gain *= gain2 / 256.0;
	e->model_gain *= 16.0 * (double)e->refine.st.gain / 4294967296.0;
	mqa_bitring_reset(&e->cond_bits);
}

int mqae_encode_open(struct mqae_encode *e, const struct mqae_config *cfg, uint64_t total)
{
	memset(e, 0, sizeof *e);
	if (mqae_encoder_open(&e->out, cfg, total) < 0)
		return -1;
	if (mqae_residual_init(&e->res, cfg->scale_index,
			       &mqa_carrier_classes[cfg->carrier_class & 3]) < 0) {
		mqae_encoder_close(&e->out);
		return -1;
	}
	mqae_analysis_init(&e->an);
	model_setup(e);
	if (e->failed) {
		mqae_encoder_close(&e->out);
		mqae_residual_free(&e->res);
		return -1;
	}
	e->sign = 1;                     /* the filter flips it at every tap */
	mqa_recon_init(&e->check);
	mqa_gain_from_scale(&e->check_gain, mqa_residual_scale_table[cfg->scale_index & 63]);
	e->src_cap = 4 * MQAE_ENCODE_BLOCK * 2 * 2;
	e->src = malloc(e->src_cap * sizeof *e->src);
	return e->src ? 0 : -1;
}

unsigned mqae_encode_headroom(double carrier_peak, unsigned measured_at)
{
	double have = (double)(mqa_conditioner_gain_index(measured_at) >> 7);
	unsigned i;

	if (carrier_peak <= 0)
		return 0;
	/* the index whose gain covers the overshoot, measured against the
	 * one the peak was found at */
	for (i = 0; i < 16; i++) {
		double factor = (double)(mqa_conditioner_gain_index(i) >> 7) / have;

		if (carrier_peak / factor <= 8388607.0 * 0.999)
			return i;
	}
	return 15;
}

void mqae_encode_close(struct mqae_encode *e)
{
	mqae_encoder_close(&e->out);
	mqae_residual_free(&e->res);
	mqae_refine_free(&e->refine);
	free(e->src);
	e->src = NULL;
}

/* Source frames held, in frames (not samples). */
static size_t held(const struct mqae_encode *e)
{
	return e->src_len / 2;
}

/* Of those, the ones that came from the file. */
static size_t real(const struct mqae_encode *e)
{
	return e->src_real / 2;
}

/*
 * The block's two byte streams into the data channel, paced.
 *
 * A decoder reads the channel about 480 frames ahead of the group it is
 * reconstructing and routes the payloads into two rings, 2048 bytes for
 * the residual symbols and 1024 for the refinement. Both are consumed
 * steadily as the block decodes, so both have to *arrive* steadily: a
 * block's bytes dumped at its start overrun the rings long before the
 * decoder gets to them, and what it reads afterwards is wrong.
 *
 * So the block's frames are cut into segments and each stream gives up
 * its share of each, with idle messages filling what is left. It is
 * what a real stream looks like, small messages of each kind, all the
 * way through.
 */
static void pace(struct mqae_encode *e, const uint8_t *aux, size_t alen,
		 const uint8_t *sym, size_t slen, unsigned taps)
{
	size_t base = e->out.chan.len;
	size_t apos = 0, spos = 0;
	unsigned segs = 16, s;

	/*
	 * A sync message before the block's own bytes. It says "resume the
	 * residual stage at the ring's write position", which is where
	 * those bytes are about to go, so on a healthy stream it changes
	 * nothing, and on one that has lost its place (a failed check
	 * takes the rings offline) it is where the decoder gets back in.
	 * Real streams carry one every couple of thousand frames.
	 */
	mqae_chan_sync(&e->out.chan, 5, e->res.scale_index, NULL);
	for (s = 1; s <= segs; s++) {
		size_t awant = alen * s / segs, swant = slen * s / segs;

		while (apos < awant) {
			unsigned n = awant - apos < 255 ? (unsigned)(awant - apos) : 255;

			mqae_chan_message(&e->out.chan, 2, aux + apos, n);
			apos += n;
		}
		while (spos < swant) {
			unsigned n = swant - spos < 255 ? (unsigned)(swant - spos) : 255;

			mqae_chan_message(&e->out.chan, 5, sym + spos, n);
			spos += n;
		}
		mqae_chan_pad_to(&e->out.chan, base + 2 * (size_t)taps * s / segs);
	}
}

/*
 * One block: analyse, place the carrier, model the decoder, choose the
 * residuals, code them, and write the frames out.
 */
static void encode_block(struct mqae_encode *e, unsigned taps, int32_t *out)
{
	unsigned solve = taps + 2 * MQAE_RESIDUAL_WARM;
	unsigned window = solve + MQAE_ANALYSE_WARMUP;
	unsigned have = (unsigned)(held(e) / 2);
	unsigned c, i, g;
	int32_t *y = NULL, *pcm = NULL, *cond_a = NULL, *cond_b = NULL;
	int32_t *pq[2] = { NULL, NULL }, *got[2] = { NULL, NULL };
	double *carrier[2] = { NULL, NULL }, *split[2] = { NULL, NULL };
	size_t len = 0, alen = 0;
	const uint8_t *bytes, *aux = NULL;

	if (window > have)
		window = have;               /* the file's end: less to look at */
	if (solve > window)
		solve = window;
	y = malloc(2 * window * sizeof *y);
	pcm = malloc(2 * taps * sizeof *pcm);
	cond_a = malloc(taps * sizeof *cond_a);
	cond_b = malloc(taps * sizeof *cond_b);
	for (c = 0; c < 2; c++) {
		carrier[c] = malloc(solve * sizeof *carrier[c]);
		split[c] = malloc(solve * sizeof *split[c]);
		pq[c] = malloc(solve * sizeof *pq[c]);
		got[c] = malloc(taps * sizeof *got[c]);
	}
	if (!y || !pcm || !cond_a || !cond_b || !carrier[1] || !split[1] || !pq[1] || !got[1]) {
		e->failed = 1;
		goto out;
	}

	/* 1. the carrier and split the source implies */
	for (c = 0; c < 2; c++) {
		for (i = 0; i < 2 * window; i++)
			y[i] = e->src[2 * i + c] >> 8;      /* 24-bit, as the decoder holds them */
		mqae_analyse(&e->an, y, solve, window - solve, carrier[c], split[c]);
	}

	/* 2. the carrier as a sample word: the control bit placed, the low
	 *    byte still to come. The bits have to exist before the sample
	 *    is rounded to carry them. */
	for (i = 0; i < taps; i++) {
		double a = carrier[0][i] < 0 ? -carrier[0][i] : carrier[0][i];
		double b = carrier[1][i] < 0 ? -carrier[1][i] : carrier[1][i];

		if (a / e->model_gain > e->carrier_peak)
			e->carrier_peak = a / e->model_gain;
		if (b / e->model_gain > e->carrier_peak)
			e->carrier_peak = b / e->model_gain;
	}
	if (e->measure_only) {
		e->taps += taps;
		goto out;
	}
	mqae_encoder_reserve(&e->out, e->taps + taps);
	for (i = 0; i < taps; i++) {
		double a = carrier[0][i] / e->model_gain, b = carrier[1][i] / e->model_gain;
		int32_t l = (int32_t)((a < 0 ? a - 0.5 : a + 0.5)) << 8;
		int32_t r = (int32_t)((b < 0 ? b - 0.5 : b + 0.5)) << 8;

		if (a > 8388607.0 || a < -8388607.0 || b > 8388607.0 || b < -8388607.0)
			e->clipped_carrier++;

		mqae_carrier_upper(&l, &r, e->out.cfg.xbit,
				   mqae_bits_at(&e->out.bits, e->taps + i));
		pcm[2 * i] = l;
		pcm[2 * i + 1] = r;
		cond_a[i] = (l >> 8) & ~0xff;              /* the decoder clears it */
		cond_b[i] = (r >> 8) & ~0xff;
	}

	/* 3. what the decoder will make of it, group by group as it does */
	{
		int32_t *pre_a = e->on_carrier ? malloc(taps * sizeof *pre_a) : NULL;
		int32_t *pre_b = e->on_carrier ? malloc(taps * sizeof *pre_b) : NULL;

		mqae_refine_block_start(&e->refine);
		for (g = 0; g < taps; g += MQAE_RESIDUAL_GROUP) {
			unsigned n = taps - g < MQAE_RESIDUAL_GROUP ? taps - g : MQAE_RESIDUAL_GROUP;

			mqa_conditioner_run(&e->cond, cond_a + g, cond_b + g, n, &e->cond_bits);
			if (pre_a) {
				memcpy(pre_a + g, cond_a + g, n * sizeof *pre_a);
				memcpy(pre_b + g, cond_b + g, n * sizeof *pre_b);
			}
			/* the correction that puts the carrier back where the
			 * analysis wanted it, and the bytes that say so */
			mqae_refine_group(&e->refine, cond_a + g, cond_b + g,
					  carrier[0] + g, carrier[1] + g, n);
		}
		aux = mqae_refine_block_end(&e->refine, &alen);
		if (e->refine.failed) {
			e->failed = 1;
			free(pre_a);
			free(pre_b);
			goto out;
		}
		e->aux_bytes += alen;
		if (e->on_carrier)
			e->on_carrier(e->carrier_user, pre_a, pre_b, cond_a, cond_b, taps);
		free(pre_a);
		free(pre_b);
	}

	/* 4. the residual that puts the split back, against that carrier.
	 *    The carrier the decoder has is not the one the analysis asked
	 *    for, and the difference has to land somewhere: half in each
	 *    output sample of the tap, rather than all of it in one. */
	for (i = 0; i < solve; i++) {
		int j;

		e->sign = -e->sign;
		for (j = 0; j < 2; j++) {
			double ideal = carrier[j][i];
			/* past the block the model has not run, so the
			 * refinement's plain scaling stands in for it: the
			 * overhang only steadies the solve and is discarded */
			int32_t a = i < taps ? (j == 0 ? cond_a[i] : cond_b[i])
					     : (int32_t)ideal;
			double t = split[j][i] + (a - ideal) / 2.0;
			double p = e->sign * (t - (double)(a >> 1));

			pq[j][i] = (int32_t)(p < 0 ? p - 0.5 : p + 0.5);
		}
	}
	/* the sign must come back for the next block: it advanced `solve`
	 * taps here but only `taps` of them are real */
	for (i = taps; i < solve; i++)
		e->sign = -e->sign;

	/* 5. code the block and give the bytes to the data channel */
	bytes = mqae_residual_block(&e->res, pq[0], pq[1], taps, got[0], got[1], &len);
	if (e->res.coder.failed) {
		e->failed = 1;
		goto out;
	}
	e->clipped = e->res.clipped;
	e->bytes += len;
	pace(e, aux, alen, bytes, len, taps);
	for (i = 0; i < taps; i++) {
		e->residual_energy += (double)pq[0][i] * pq[0][i];
		e->coding_error += (double)(got[0][i] - pq[0][i]) * (got[0][i] - pq[0][i]);
	}
	e->counted += taps;
	/* what the decoder should now produce, and how far that is from
	 * the source: the filter run forwards over the model's carrier and
	 * the residuals the coder really delivered */
	{
		struct mqa_recon_coeffs k;

		mqa_recon_coeffs_default(&k);
		for (i = 0; i < taps; i++) {
			int32_t cp[2] = { cond_a[i], cond_b[i] };
			int32_t rp[2] = { got[0][i], got[1][i] };
			int32_t dl[2] = { 0, 0 }, dr[2] = { 0, 0 };
			int j;

			mqa_recon_tap(&e->check, &e->check_gain, &k, cp, rp, dl, dr);
			for (j = 0; j < 2; j++) {
				double d = (double)dl[j] - (e->src[2 * (2 * i + j)] >> 8);

				e->out_error += d * d;
				e->out_energy += (double)(e->src[2 * (2 * i + j)] >> 8)
					       * (e->src[2 * (2 * i + j)] >> 8);
			}
		}
	}
	for (i = 0; i < taps; i++) {
		double d = cond_a[i] - carrier[0][i];

		e->carrier_energy += carrier[0][i] * carrier[0][i];
		e->carrier_error += d * d;
	}
	/* 6. out it goes, the data channel filling the low bytes */
	memcpy(out, pcm, 2 * taps * sizeof *out);
	mqae_encoder_write(&e->out, out, taps, 1);
	/* how much of the channel the residuals are using: what is still
	 * unwritten once the block's own frames have carried what they can */
	if (mqae_encoder_chan_ahead(&e->out) > e->chan_peak)
		e->chan_peak = mqae_encoder_chan_ahead(&e->out);
	e->taps += taps;
out:
	free(y);
	free(pcm);
	free(cond_a);
	free(cond_b);
	for (c = 0; c < 2; c++) {
		free(carrier[c]);
		free(split[c]);
		free(pq[c]);
		free(got[c]);
	}
}

int mqae_encode_push(struct mqae_encode *e, const int32_t *src, size_t frames,
		     int end, int32_t *out, size_t max, size_t *produced)
{
	size_t done = 0;

	*produced = 0;
	if (e->failed)
		return -1;
	for (;;) {
		size_t room, take;
		unsigned taps;

		/* top the buffer up */
		room = (e->src_cap - e->src_len) / 2;
		take = frames - done < room ? frames - done : room;
		if (take) {
			memcpy(e->src + e->src_len, src + 2 * done, 2 * take * sizeof *src);
			e->src_len += 2 * take;
			e->src_real = e->src_len;
			done += take;
		}
		if (end && done == frames)
			e->ended = 1;

		/* a block needs its own taps and the overhang the solve wants */
		taps = MQAE_ENCODE_BLOCK;
		if (held(e) < 2 * (size_t)(taps + MQAE_ENCODE_AHEAD)) {
			if (!e->ended)
				break;                    /* wait for more source */
			/* only what the file actually holds: the padding
			 * below is there for the solve to look at, not to
			 * be encoded */
			taps = (unsigned)(real(e) / 2);
			taps -= taps % MQAE_RESIDUAL_GROUP;
			if (taps == 0)
				break;                    /* the tail is not a group */
			/*
			 * The file's end: the solve wants to look past it, so
			 * it is given silence to look at. Without that the
			 * backward recursion starts from nothing and the last
			 * block's residuals are noise.
			 */
			{
				size_t want = 4 * (size_t)(taps + MQAE_ENCODE_AHEAD);

				if (want > e->src_cap)
					want = e->src_cap;
				if (want > e->src_len) {
					memset(e->src + e->src_len, 0,
					       (want - e->src_len) * sizeof *e->src);
					e->src_len = want;
				}
			}
		}
		/* a measuring pass has no output to fit anywhere */
		if (!e->measure_only && *produced + taps > max)
			break;                            /* the caller's buffer */
		encode_block(e, taps, out + 2 * *produced);
		if (e->failed)
			return -1;
		*produced += taps;
		/* the frames the block consumed; the overhang stays */
		memmove(e->src, e->src + 4 * taps, (e->src_len - 4 * taps) * sizeof *e->src);
		e->src_len -= 4 * taps;
		e->src_real = e->src_real > 4 * (size_t)taps ? e->src_real - 4 * taps : 0;
	}
	return 0;
}
