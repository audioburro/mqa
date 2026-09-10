/*
 * Choosing what the entropy decoder will produce -- see mqae/entropy.h.
 *
 * Every state update below is the decoder's own (src/entropy.c), in the
 * same order and the same 32-bit arithmetic: the encoder keeps a decoder
 * and runs it forward, the only difference being that the quantised
 * magnitude comes from a search rather than from a code word.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqae/entropy.h"
#include "mqa/lcg.h"

static inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static inline int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static inline uint64_t prod64(int32_t a, int32_t b) { return (uint64_t)((int64_t)a * b); }

static uint32_t udiv(uint32_t n, uint32_t d)
{
	return d == 0 ? 0 : n / d;
}

static int32_t clamp32(int32_t x, int32_t lo, int32_t hi)
{
	return x < lo ? lo : x > hi ? hi : x;
}

void mqae_entropy_attach(struct mqae_entropy *e, struct mqae_coder *coder)
{
	e->coder = coder;
}

/*
 * What one choice of step index costs and reaches. `k` picks the
 * magnitude pair (a, b); the interval [lo, hi) is what the coder must
 * narrow to for a decoder to recover it, and (lo_g, hi_g] the residual
 * indices the symbol can then carry.
 */
struct choice {
	int32_t a, b, m;
	uint32_t lo, hi;
	int32_t sa, sb;
	int32_t lo_g, hi_g;
	int ok;
};

static void evaluate(const struct mqae_entropy *e, int32_t qv, int32_t dscaled, struct choice *c)
{
	const struct mqa_entropy_decoder *d = &e->d;
	const struct mqa_entropy_record *ra, *rb;
	uint32_t lo_a, lo_b;
	int32_t hi_in, lo_in;

	memset(c, 0, sizeof *c);
	c->a = mul32(d->level, qv);
	c->b = add32(c->a, mul32(d->level, 256));
	/* the table's reach: past this the code space wraps round on itself
	 * and the interval stops meaning anything */
	if (c->a > 500000 || c->a < -500000 || c->b > 500000 || c->b < -500000)
		return;
	ra = mqa_entropy_bin(c->a);
	rb = mqa_entropy_bin(c->b);
	c->m = (int32_t)ra->weight + rb->weight;
	lo_a = mqa_entropy_bin_position(ra, c->a);
	lo_b = mqa_entropy_bin_position(rb, c->b);
	c->lo = lo_a >> 13;
	c->hi = lo_b >> 13;
	if (c->hi <= c->lo || e->coder->range < c->hi)
		return;
	c->sa = (int32_t)(uint32_t)(prod64(c->a, d->variance) >> 16);
	c->sb = (int32_t)(uint32_t)(prod64(c->b, d->variance) >> 16);
	hi_in = add32(2 * sub32(c->sb, dscaled), 1);
	lo_in = sub32(2 * sub32(c->sa, dscaled), 1);
	c->hi_g = mqa_mulhi32(hi_in, d->gain.gain) >> d->gain.shift;
	c->lo_g = mqa_mulhi32(lo_in, d->gain.gain) >> d->gain.shift;
	c->ok = c->hi_g > c->lo_g;
}

/*
 * The two AR stages' contributions. They depend on the predictor's
 * history and not on the choice being made, so the symbol's value can
 * be worked out before the choice and used again after it.
 */
static void predictor_term(const struct mqa_entropy_decoder *d, unsigned pos,
			   int32_t *sub_a, int32_t *sub_b, int32_t *pv)
{
	uint64_t acc_a = 0, acc_b = 0;
	int32_t v;
	int k;

	for (k = 0; k < MQA_ENTROPY_TAPS; k++) {
		int32_t tap = d->taps[(k + pos) % MQA_ENTROPY_TAPS];

		acc_a += prod64(tap, d->pred_a[k]);
		acc_b += prod64(tap, d->pred_b[k]);
	}
	*sub_a = mul32((int32_t)(uint32_t)(acc_a >> 32), 16);
	*sub_b = mul32((int32_t)(uint32_t)(acc_b >> 32), 16);
	v = sub32(0, add32(*sub_a, *sub_b));
	if (d->scale2 == d->gain.scale)
		v = mqa_gain_step(&d->gain, mqa_gain_input(&d->gain, v));
	*pv = v;
}

/* The decoder's per-block filter adaptation. */
static void adapt_taps(struct mqa_entropy_decoder *d, uint32_t dither)
{
	int32_t h[MQA_ENTROPY_HISTORY], e[4], c[4];
	int k, j;

	for (k = 0; k < MQA_ENTROPY_HISTORY; k++)
		h[k] = d->history[(d->head + k) % MQA_ENTROPY_HISTORY];
	for (j = 4; j < 8; j++) {
		int32_t p = 0;

		for (k = 0; k < 4; k++)
			p = add32(p, mul32(d->coef[k], h[j - 3 + k]));
		e[j - 4] = add32(h[j - 4], p >> 12);
	}
	for (k = 0; k < 4; k++) {
		int32_t g = 0;

		for (j = 4; j < 8; j++)
			g = add32(g, mul32(h[j - 3 + k], e[j - 4]));
		g = add32(g, (int32_t)(dither << 4));
		c[k] = sub32(d->coef[k], g >> 12);
		d->coef[k] = (int16_t)c[k];
		d->taps[k] = mul32((int32_t)d->decay[k], c[k]);
	}
}

/* One symbol: choose, record, and update the state exactly as the
 * decoder will when it reads what was recorded. */
static int32_t symbol(struct mqae_entropy *e, int32_t target, unsigned pos, uint32_t *dither_out)
{
	struct mqa_entropy_decoder *d = &e->d;
	struct choice c, best;
	uint32_t s1, s2, dither, dscaled;
	int32_t pv, want, r, resid, out, sa, sb, pred, p1, p2, v, sub_a, sub_b;
	int32_t klo, khi, k, chosen;
	int64_t centre;
	int16_t hist_in;

	s1 = mqa_nr_lcg_step(d->rng);
	s2 = mqa_nr_lcg_step(s1);
	d->rng = s2;
	dither = s2 >> 24;
	dscaled = (uint32_t)(((uint64_t)s1 * d->gain.scale) >> 32);
	*dither_out = dither;

	predictor_term(d, pos, &sub_a, &sub_b, &pv);
	/* the residual index that would land on the target */
	if (d->gain.scale == 0)
		want = 0;
	else {
		int64_t num = (int64_t)target - (int32_t)dscaled - pv;
		int64_t s = (int64_t)d->gain.scale;

		want = (int32_t)((num >= 0 ? num + s / 2 : num - s / 2) / s);
	}

	/*
	 * The magnitudes sit on a lattice (the decoder snaps them so that
	 * their low byte is the dither byte), so the search is over the
	 * lattice index. The reachable residual indices grow with it, so a
	 * bisection finds the interval the target belongs in; the two
	 * neighbours are then compared, because the intervals overlap and
	 * the middle of one is a better place to sit than the edge.
	 */
	/*
	 * The magnitude is level * (dither + 256k), and the table runs out
	 * around 2^19, so that is how far the index can go.
	 */
	{
		int32_t step = d->level > 0 ? d->level * 256 : 256;
		int32_t reach = 600000 / step + 2;

		klo = -reach;
		khi = reach;
	}
	while (klo < khi) {
		k = klo + (khi - klo) / 2;
		evaluate(e, add32((int32_t)dither, mul32(256, k)), (int32_t)dscaled, &c);
		centre = c.ok ? ((int64_t)c.lo_g + c.hi_g) / 2 : (k < 0 ? INT32_MIN : INT32_MAX);
		if (centre >= want)
			khi = k;
		else
			klo = k + 1;
	}
	chosen = klo;
	best.ok = 0;
	{
		int64_t bcost = 0;

		for (k = klo - 1; k <= klo + 1; k++) {
			int64_t cost;

			evaluate(e, add32((int32_t)dither, mul32(256, k)), (int32_t)dscaled, &c);
			if (!c.ok)
				continue;
			/* how far this interval's nearest reachable index is
			 * from the one wanted, and then how central it sits */
			cost = want <= c.lo_g ? (int64_t)c.lo_g + 1 - want
			     : want > c.hi_g ? (int64_t)want - c.hi_g : 0;
			cost = cost * 1000000
			     + (cost ? 0
				     : (want * 2 > (int64_t)c.lo_g + c.hi_g
					? want * 2 - c.lo_g - c.hi_g
					: (int64_t)c.lo_g + c.hi_g - want * 2));
			if (!best.ok || cost < bcost) {
				best = c;
				bcost = cost;
				chosen = k;
			}
		}
	}
	if (!best.ok) {
		/* nothing the table can carry: the coder is stuck. The caller
		 * sees this as a clipped symbol and the stream stays valid --
		 * a zero-width interval would not be decodable at all. */
		e->clipped++;
		evaluate(e, add32((int32_t)dither, mul32(256, 0)), (int32_t)dscaled, &best);
		chosen = 0;
		if (!best.ok) {
			e->coder->failed = 1;
			e->coder->why = "no codeable magnitude";
			return 0;
		}
	}
	(void)chosen;

	/* commit: the interval, then the fine residual inside it */
	mqae_coder_narrow(e->coder, best.lo, best.hi);

	v = d->variance;
	sa = best.sa;
	sb = best.sb;
	d->variance = (int32_t)(uint32_t)(prod64(best.m, v) >> 12);
	hist_in = (int16_t)clamp32(add32(best.b, best.a) >> 9, -512, 512);
	d->head = (d->head + MQA_ENTROPY_HISTORY - 1) % MQA_ENTROPY_HISTORY;
	d->history[d->head] = hist_in;
	pred = add32(sa, sb) >> 1;
	d->level = clamp32((int32_t)udiv((uint32_t)mul32(d->level_rate, d->level), (uint32_t)best.m),
			   d->level_min, d->level_max);
	p1 = sub32(pred, sub_a);
	p2 = sub32(p1, sub_b);
	d->pred_a[MQA_ENTROPY_TAPS - 1 - pos] = p1;
	d->pred_b[MQA_ENTROPY_TAPS - 1 - pos] = p2;

	r = want;
	if (r <= best.lo_g)
		r = best.lo_g + 1;
	if (r > best.hi_g)
		r = best.hi_g;
	if (r != want)
		e->clipped++;
	resid = r;
	{
		int32_t n = sub32(best.hi_g, best.lo_g);

		if (n > 1) {
			/* the decoder renormalises before it takes the
			 * residual, as it does before every read */
			mqae_coder_normalize(e->coder);
			mqae_coder_divide(e->coder, (uint32_t)n, (uint32_t)(best.hi_g - r));
		}
		else
			resid = best.hi_g;
	}
	out = add32(add32(mul32((int32_t)d->gain.scale, resid), (int32_t)dscaled),
		    sub32(p2, pred));
	if (d->scale2 == d->gain.scale)
		out = add32(add32(mul32((int32_t)d->gain.scale, resid), (int32_t)dscaled),
			    mqa_gain_step(&d->gain, mqa_gain_input(&d->gain, sub32(p2, pred))));
	return out;
}

void mqae_entropy_encode(struct mqae_entropy *e, const int32_t *target, int32_t *got,
			 unsigned count)
{
	unsigned done = 0;
	uint32_t dither = 0;

	while (done < count) {
		unsigned end = done + MQA_ENTROPY_BLOCK < count ? done + MQA_ENTROPY_BLOCK : count;
		unsigned pos;

		for (pos = 0; done < end; done++, pos++) {
			mqae_coder_normalize(e->coder);
			got[done] = symbol(e, target[done], pos, &dither);
		}
		adapt_taps(&e->d, dither);
		if (e->d.variance < (int32_t)(e->d.scale2 << 1))
			e->d.variance = (int32_t)(e->d.scale2 << 1);
	}
}

void mqae_entropy_block_init(struct mqae_entropy *e, uint32_t block, int32_t rate,
			     unsigned level_hi, unsigned variance_low)
{
	struct mqa_entropy_decoder *d = &e->d;
	unsigned bits = block ? 11 : 7;
	uint32_t v = variance_low & 0x7f;

	d->level = 0x200;
	d->level_rate = rate;
	if (block) {
		v |= (level_hi & 0xf) << 7;
		d->level = mqa_residual_level_table[(0u - (level_hi & 0xf)) & 3] >>
			   (((level_hi & 0xf) + 3) >> 2);
	}
	mqae_coder_normalize(e->coder);
	mqae_coder_field(e->coder, bits, v);
	d->variance = (int32_t)((uint32_t)mqa_residual_level_table[(variance_low & 0x7f) & 3]
				<< ((variance_low & 0x7f) >> 2)) >> 8;
}
