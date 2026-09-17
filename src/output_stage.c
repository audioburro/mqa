/*
 * The output stage -- see include/mqa/output_stage.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/output_stage.h"
#include "mqa/lcg.h"
#include "mqa/residual_stage.h"   /* mqa_gain_from_scale */

#define RING_WORDS 832    /* the carrier-history rings' length */

/* --- dither -------------------------------------------------------------- */

void mqa_dither_fill(struct mqa_dither_source *d, uint32_t scale,
		     int32_t *l, int32_t *r, unsigned n)
{
	while (n) {
		unsigned left = (0u - d->counter) & (MQA_DITHER_RESEED - 1), chunk, i;

		if (left == 0) {
			uint32_t s = d->salt + (d->counter >> 11);

			d->lcg[0] = mqa_nr_lcg_step(s * s);
			d->lcg[1] = mqa_nr_lcg_step(d->lcg[0]);
			left = MQA_DITHER_RESEED;
		}
		chunk = left < n ? left : n;
		if (d->salt == 0) {
			memset(l, 0, 2 * chunk * sizeof *l);
			memset(r, 0, 2 * chunk * sizeof *r);
		} else {
			for (i = 0; i < 2 * chunk; i++) {
				l[i] = (int32_t)(((uint64_t)d->lcg[0] * scale) >> 32);
				r[i] = (int32_t)(((uint64_t)d->lcg[1] * scale) >> 32);
				d->lcg[0] = mqa_lcg_step(d->lcg[0]);
				d->lcg[1] = mqa_lcg_step(d->lcg[1]);
			}
		}
		d->counter += chunk;
		n -= chunk;
		l += 2 * chunk;
		r += 2 * chunk;
	}
}

void mqa_output_stage_open(struct mqa_output_stage *s, unsigned rate_code)
{
	static const uint8_t unset[8] = { 0x00, 31 << 2, 0x00, 31 << 1, 0x00, 0x00, 0x0f, 0x00 };
	uint8_t *b = s->desc.bytes;

	memcpy(b + 8, unset, 8);                   /* what the decoder opened with */
	memset(b, 0, 8);
	b[1] = (uint8_t)((rate_code & 31) << 2);
	b[3] = (uint8_t)((rate_code & 31) << 1);
	b[6] = 0x1f;                               /* a fixed output rate: no resampling */
}

/* --- descriptor bookkeeping --------------------------------------------- */

static void descriptor_update(struct mqa_output_stage *s, unsigned format, int advance)
{
	uint8_t *b = s->desc.bytes;
	unsigned field;

	memcpy(b + 8, b, 8);                       /* keep the previous one */
	b[14] = (uint8_t)((b[14] & ~0xe0) | (format & 7) << 5);
	if (format > 1) {
		b[9] = (uint8_t)((b[9] & ~0x7c) | (s->format_field & 0x1f) << 2);
		b[11] = (uint8_t)((b[11] & ~0x40) | 0x80);
	}
	field = (b[3] >> 1) & 0x1f;
	if (advance)
		field = (field & 7) == 7 ? 0x1f : (field + 1) & 0x1f;
	b[11] = (uint8_t)((b[11] & ~0x3e) | field << 1);
}

/* --- the carrier pair ring ---------------------------------------------- */

static void pair_ring_push(struct mqa_output_stage *s, int32_t a, int32_t b)
{
	int32_t pl = (int32_t)(((int64_t)a * s->pair_gain) >> 16);
	int32_t pr = (int32_t)(((int64_t)b * s->pair_gain) >> 16);

	if (s->pair_wpos == 0)
		s->pair_wpos = MQA_OUTPUT_PAIR_RING;
	s->pair_wpos--;
	s->pairs[2 * s->pair_wpos] = pl;
	s->pairs[2 * s->pair_wpos + 1] = pr;
	s->pairs[2 * (s->pair_wpos + MQA_OUTPUT_PAIR_RING)] = pl;
	s->pairs[2 * (s->pair_wpos + MQA_OUTPUT_PAIR_RING) + 1] = pr;

	s->rate_acc += s->rate_inc;
	if (s->rate_acc > 0) {
		do {
			s->rate_acc -= s->rate_period;
			s->rate_pos += s->rate_step;
		} while (s->rate_acc > 0);
	}
	if (s->rate_acc == 0)
		s->rate_pos = 0;
}

/* Copy the `n` most recent pairs out of the ring, oldest first. */
static void pair_ring_read(struct mqa_output_stage *s, int32_t *l, int32_t *r, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		unsigned idx = (s->pair_wpos + n - 1 - i) % MQA_OUTPUT_PAIR_RING;

		l[i] = s->pairs[2 * idx];
		r[i] = s->pairs[2 * idx + 1];
	}
}

/* --- the output CRC ------------------------------------------------------ */

#define CRC32_POLY_REFLECTED 0xedb88320u

void mqa_output_stage_init(struct mqa_output_stage *s)
{
	unsigned i, bit;

	memset(s, 0, sizeof *s);
	/* the reconstruction filter's coefficients: the decoder opens with
	 * the first four of its table and the stream never changes them */
	mqa_recon_coeffs_default(&s->coeffs);
	for (i = 0; i < 256; i++) {
		uint32_t c = i;

		for (bit = 0; bit < 8; bit++)
			c = (c & 1) ? (c >> 1) ^ CRC32_POLY_REFLECTED : c >> 1;
		s->crc_table[i] = c;
	}
	/* the reference's initial resampler and rate status */
	s->spec = &mqa_resampler_identity;
	s->rate_step = s->rate_inc = s->rate_period = 1;
	s->rs_mode = 2;
	s->rs_dither[0] = s->rs_dither[1] = 1;
	s->status.code = s->status.code_out = MQA_RATE_CODE_NONE;
	s->status.select = 1;
	s->status.flags = 0x3ff;
}

static uint32_t crc_absorb(const uint32_t *t, uint32_t crc, int32_t sample)
{
	unsigned k;

	for (k = 0; k < 4; k++)
		crc = t[crc & 0xff] ^ (crc >> 8);
	return crc ^ (uint32_t)sample;
}

void mqa_output_crc_accumulate(struct mqa_output_stage *s, const int32_t *l, const int32_t *r,
			       unsigned n, int check)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		s->crc = crc_absorb(s->crc_table, s->crc, l[i]);
		s->crc = crc_absorb(s->crc_table, s->crc, r[i]);
	}
	if (!check)
		return;
	if (s->crc_armed && s->crc_expect != s->crc &&
	    s->indicator && s->indicator_enable && !s->indicator_hold)
		s->notify = 1;
	s->crc = 0;
	s->crc_armed = 0;
}

/* --- restart from a parameter record ------------------------------------- */

/* The two dither salts a record can select. */
#define SALT_A 0xf807b7dfu
#define SALT_B 0xe9d30005u

static int32_t le32(const uint8_t *b)
{
	return (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
}

int mqa_output_stage_restart(struct mqa_output_stage *s, const struct mqa_output_restart *r)
{
	uint8_t *b = s->desc.bytes;
	unsigned c;

	s->restarts++;
	b[0] = (uint8_t)((b[0] & ~0x1f) | (r->descriptor & 0x1f));
	b[0] = (uint8_t)((b[0] & ~0x60) | (r->descriptor & 0x60));
	b[1] = (uint8_t)((b[1] & ~0x01) | ((r->descriptor >> 8) & 0x01));   /* bits 7-8 of the halfword */
	b[0] = (uint8_t)((b[0] & ~0x80) | (r->descriptor & 0x80));
	b[1] = (uint8_t)((b[1] & ~0x02) | ((r->descriptor >> 8) & 0x02));
	s->crc_armed = 0;
	s->format_field = r->format_field;
	s->crc = 0;
	if (r->valid) {
		int reparam = 1;

		if (r->flavour != 0) {
			if (r->position == s->dither.counter && s->gain.scale == (uint32_t)r->scale &&
			    s->variant == r->variant)
				reparam = 0;
			else
				s->dither.counter = r->position;
		} else {
			s->dither.counter = 0;
		}
		if (reparam) {
			s->variant = r->variant;
			mqa_gain_from_scale(&s->gain, r->scale);
		s->dither.salt = r->salt_select == 0 ? 0 : r->salt_select == 1 ? SALT_A : SALT_B;
		{
			uint32_t seed = s->dither.salt + (s->dither.counter >> 11);

			s->dither.lcg[0] = mqa_nr_lcg_step(seed * seed);
			s->dither.lcg[0] = mqa_lcg_jump(s->dither.lcg[0], 2 * (s->dither.counter & 0x7ff));
			s->dither.lcg[1] = mqa_nr_lcg_step(s->dither.lcg[0]);
		}
			s->kernel = r->variant == 0;
			if (s->kernel) {
				/* the alternative kernel: its history cleared,
				 * the shape triple chosen the way the
				 * refinement stage chooses its coefficients,
				 * and the first eight taps of every group
				 * given to the filter without output */
				unsigned set = (r->kernel_param & 7) * 3 + (r->kernel_param >> 3);

				mqa_recon2_init(&s->recon2, set > 3 ? 3 : set);
				s->skip = MQA_RECON2_LATENCY;
				s->lookahead = MQA_RECON2_LATENCY;
			} else {
				/* the short filter: history cleared */
				s->recon.sign = 1;
				memset(s->recon.ch, 0, sizeof s->recon.ch);
				s->skip = 0;
				s->lookahead = 0;
			}
		}
		if (r->flavour == 0 && s->kernel) {
			/* the alternative kernel has no use for the record's
			 * history: the reference reads past it */
		} else if (r->flavour == 0) {
			/* the record's part B is the history */
			for (c = 0; c < 2; c++) {
				const uint8_t *p = r->part_b + 16 * c;

				s->recon.ch[c].pred[1] = le32(p);
				s->recon.ch[c].pred[0] = le32(p + 4);
				s->recon.ch[c].corr[1] = le32(p + 8);
				s->recon.ch[c].corr[0] = le32(p + 12);
			}
		} else if (s->kernel) {
			/* prime with the same 16 pairs, oldest first */
			int32_t la[16], ra[16];

			pair_ring_read(s, la, ra, 16);
			mqa_recon2_prime(&s->recon2, la, ra, NULL, NULL, 16);
		} else {
			/* replay the 16 most recent carrier pairs, oldest first */
			unsigned i;

			for (i = 16; i-- > 0; ) {
				unsigned idx = (s->pair_wpos + i) % MQA_OUTPUT_PAIR_RING;
				int32_t pair[2] = { s->pairs[2 * idx], s->pairs[2 * idx + 1] };

				mqa_recon_replay_tap(&s->recon, &s->coeffs, pair);
			}
		}
	}
	s->pair_gain = r->variant == 1 ? 0x8000 : 0xaab1;
	return 0;
}

/* --- the group ------------------------------------------------------------ */

static void clamp24(int32_t *v, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		if (v[i] > 0x7fffff)
			v[i] = 0x7fffff;
		else if (v[i] < -0x800000)
			v[i] = -0x800000;
	}
}

int mqa_output_stage_group(struct mqa_output_stage *s,
			   const int32_t *a, const int32_t *b, unsigned count,
			   const int32_t *p, const int32_t *q,
			   int32_t *l, int32_t *r,
			   unsigned format, int check_crc, int ending,
			   const int32_t *tail_a, const int32_t *tail_b,
			   unsigned *ring_pos)
{
	unsigned i, n = count, out;

	descriptor_update(s, format, 1);
	for (i = 0; i < count; i++)
		pair_ring_push(s, a[i], b[i]);

	if (s->skip) {
		unsigned drop = s->skip < n ? s->skip : n;

		/* the skipped taps still go through the filter */
		if (s->kernel) {
			mqa_recon2_prime(&s->recon2, a, b, p, q, drop);
			/* at a stream's very start (output position zero) the
			 * reference then clears what the warm-up left in the
			 * output and residue histories; a stream joined part way
			 * through keeps them */
			if (s->skip == drop && s->dither.counter == 0)
				mqa_recon2_settle(&s->recon2);
		}
		s->skip = (uint8_t)(s->skip - drop);
		a += drop;
		b += drop;
		p += drop;
		q += drop;
		n -= drop;
	}

	mqa_dither_fill(&s->dither, s->gain.scale, l, r, n);
	if (s->kernel) {
		mqa_recon2_group(&s->recon2, &s->gain, a, b, p, q, l, r, n);
	} else {
		for (i = 0; i < n; i++) {
			int32_t carrier[2] = { a[i], b[i] }, residual[2] = { p[i], q[i] };

			mqa_recon_tap(&s->recon, &s->gain, &s->coeffs, carrier, residual,
				      l + 2 * i, r + 2 * i);
		}
	}
	out = 2 * n;
	clamp24(l, out);
	clamp24(r, out);
	if (ending) {
		/* dither beyond the group, for the alternative kernel's lookahead */
		mqa_dither_fill(&s->dither, s->gain.scale, l + out, r + out, s->lookahead);
	}
	if (ending == 2 && s->kernel && tail_a && tail_b) {
		/* flush the kernel: the samples just past the group, scaled
		 * back up as the pair ring's replay scales them, with no
		 * residual. These outputs are not clamped: the reference
		 * clamps the group before the flush runs. */
		int32_t ta[MQA_RECON2_LATENCY], tb[MQA_RECON2_LATENCY];
		int32_t none[MQA_RECON2_LATENCY] = { 0 };

		for (i = 0; i < MQA_RECON2_LATENCY; i++) {
			ta[i] = (int32_t)((uint32_t)tail_a[i] + (uint32_t)(tail_a[i] >> 1));
			tb[i] = (int32_t)((uint32_t)tail_b[i] + (uint32_t)(tail_b[i] >> 1));
		}
		mqa_recon2_group(&s->recon2, &s->gain, ta, tb, none, none,
				 l + out, r + out, MQA_RECON2_LATENCY);
		out += 2 * MQA_RECON2_LATENCY;
	}
	mqa_output_crc_accumulate(s, l, r, out, check_crc);
	*ring_pos = (*ring_pos + count) % RING_WORDS;
	return (int)out;
}

/* --- the passthrough resampler ------------------------------------------ */

const int32_t mqa_rate_code_base[3] = { 44100, 48000, 64000 };

/* Rotate the history so the newest pair leads, and restart the phase. */
static void resampler_reset(struct mqa_output_stage *s)
{
	int32_t rotated[2 * MQA_OUTPUT_PAIR_RING];
	unsigned i;

	for (i = 0; i < MQA_OUTPUT_PAIR_RING; i++) {
		rotated[2 * i] = s->pairs[2 * (s->pair_wpos + i)];         /* via the mirror */
		rotated[2 * i + 1] = s->pairs[2 * (s->pair_wpos + i) + 1];
	}
	memcpy(s->pairs, rotated, sizeof rotated);
	memcpy(s->pairs + 2 * MQA_OUTPUT_PAIR_RING, rotated, sizeof rotated);
	s->rate_pos = 0;
	s->rate_acc = 0;
	s->pair_wpos = 0;
}

/* Put a record in force; a different ratio or table restarts the filter. */
static void resampler_configure(struct mqa_output_stage *s, const struct mqa_resampler_spec *spec)
{
	if (spec->num != (unsigned)s->rate_inc || spec->den != (unsigned)s->rate_period ||
	    spec->table != s->spec->table) {
		resampler_reset(s);
		if (spec->num == 1)
			s->rs_mode = spec->den == 1 ? 2 : 1;
		else
			s->rs_mode = spec->den == 1 ? 3 : 0;
	}
	s->spec = spec;
	s->rate_step = (int32_t)spec->taps;
	s->rate_inc = (int32_t)spec->num;
	s->rate_period = (int32_t)spec->den;
}

/* What a record's ratio means for the output: 0 and 2 leave samples alone. */
static unsigned resampling_state(const struct mqa_resampler_spec *spec)
{
	static const unsigned by_ratio[9] = { 1, 2, 3, 6, 4, 6, 6, 6, 5 };
	unsigned d = spec->num - spec->den + 1;

	return d > 8 ? 6 : by_ratio[d];
}

/* The rate code of the resampled output, from the input's code. */
static unsigned output_rate_code(unsigned code, const struct mqa_resampler_spec *spec)
{
	int32_t rate = 0, v;
	unsigned shifts = 0, i;

	if (code <= 23)
		rate = (int32_t)spec->num * (mqa_rate_code_base[code >> 3] << (code & 7));
	rate /= (int32_t)spec->den;
	if (rate <= 0x7fff)
		return MQA_RATE_CODE_NONE;
	for (v = rate >> 1; v > 0x7fff; v >>= 1)
		shifts++;
	if (shifts > 7)
		return MQA_RATE_CODE_NONE;
	v = rate >> shifts;
	for (i = 0; i < 3; i++)
		if (v == mqa_rate_code_base[i])
			return 8 * i + shifts;
	return MQA_RATE_CODE_NONE;
}

enum { PASS_COPY, PASS_RESAMPLE };

/* Whether the current state resamples, and the descriptor's part in it. */
static int passthrough_plan(struct mqa_output_stage *s)
{
	struct mqa_rate_status *st = &s->status;
	uint8_t *b = s->desc.bytes;
	uint32_t bit;

	if (st->state == 0 || st->state == 2)
		return PASS_COPY;
	if (b[14] & 0x10)
		return PASS_COPY;
	if ((b[11] >> 1) & 7)
		return PASS_COPY;
	b[11] = (uint8_t)((b[11] & ~0x3e) | (st->code_out & 31) << 1);
	if (st->state > 21)
		return PASS_COPY;
	bit = 1u << st->state;
	if (bit & 0x380000)
		return PASS_RESAMPLE;
	if (bit & 0x3800)
		return PASS_RESAMPLE;       /* (after telling the owner) */
	return bit & 0x7e ? PASS_RESAMPLE : PASS_COPY;
}

/*
 * Follow the descriptor: when its rate code, format or low nibble (or
 * whether a stream format is set at all) changed, choose the resampler
 * record for the new situation and configure the resampler. The
 * reference also reports each change to an owner object; none is ever
 * attached in practice, so those calls are left out.
 */
static int rate_status_update(struct mqa_output_stage *s, unsigned format)
{
	struct mqa_rate_status *st = &s->status;
	const uint8_t *b = s->desc.bytes;
	unsigned code_now = (b[11] >> 1) & 31, fmt_now = (b[9] >> 2) & 7, low_now = b[8] & 15;
	uint8_t active_now = format != 0;
	const struct mqa_resampler_spec *spec = NULL;
	unsigned code, low3, pick, idx;
	int changed = 0;

	if (code_now != st->code) { st->code = code_now; changed = 1; }
	if (fmt_now != st->format) { st->format = fmt_now; changed = 1; }
	if (low_now != st->desc_low) { st->desc_low = low_now; changed = 1; }
	if (active_now != st->active) { st->active = active_now; changed = 1; }
	if (!changed)
		return passthrough_plan(s);

	st->state = 0;
	code = st->code;
	if (code == MQA_RATE_CODE_NONE)
		return PASS_COPY;
	low3 = code & 7;
	st->code_out = code;
	if (low3 > 3 || st->aux == 3)
		return PASS_COPY;
	if ((int)st->select > 4)
		return PASS_COPY;
	pick = st->select;

	/* a record for the rate: a special one for a particular mode, else
	 * one of a group when the format allows, else the plain table */
	if (st->aux == 0 || (st->aux == 1 && st->active)) {
		if ((code & 24) == 0 && low3 == 1 && st->aux == 2 && (st->flags & 0x30) == 0x20 &&
		    mqa_resampler_special)
			spec = mqa_resampler_special;
	}
	if (!spec && (int)st->format <= 7 && (int)low3 < (int)st->format) {
		if (st->aux == 2)
			pick = (int)pick < (int)st->format ? pick : st->format;
		idx = 5 * low3 + pick;
		if (mqa_resampler_group[idx][0])
			spec = mqa_resampler_group[idx][st->desc_low];
	}
	if (!spec) {
		if (st->aux != 0 && (st->aux != 1 || !st->active))
			return PASS_COPY;
		spec = mqa_resampler_single[5 * low3 + pick];
		if (!spec)
			return PASS_COPY;
	}

	st->state = resampling_state(spec);
	resampler_configure(s, spec);
	if (st->code != MQA_RATE_CODE_NONE)
		st->code_out = output_rate_code(st->code, spec);
	return passthrough_plan(s);
}

/* --- passthrough --------------------------------------------------------- */

int mqa_output_stage_passthrough(struct mqa_output_stage *s,
				 const int32_t *a, const int32_t *b, unsigned count,
				 int32_t *l, int32_t *r, unsigned format, unsigned *ring_pos)
{
	descriptor_update(s, format, 0);
	if (rate_status_update(s, format) != PASS_COPY && count)
		return MQA_OUTPUT_UNSUPPORTED;   /* resampling is not implemented */
	memcpy(l, a, count * sizeof *l);
	memcpy(r, b, count * sizeof *r);
	*ring_pos = (*ring_pos + count) % RING_WORDS;
	return (int)count;
}
