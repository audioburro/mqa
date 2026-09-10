/*
 * Adaptive arithmetic decoder -- see include/mqa/entropy.h for the
 * algorithm description.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mqa/entropy.h"
#include "mqa/lcg.h"

/* 32-bit wrapping arithmetic, made explicit where values can exceed 2^31. */
static inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static inline int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

/* Signed 64-bit product of two 32-bit values, as an unsigned bit pattern
 * so that sums of several can wrap without undefined behaviour. */
static inline uint64_t prod64(int32_t a, int32_t b) { return (uint64_t)((int64_t)a * b); }

/* Unsigned division as the ARM EABI helper the decoder calls does it:
 * a zero divisor yields 0 (libgcc's "about as wrong as it could be"). */
static uint32_t udiv(uint32_t n, uint32_t d, unsigned *zero_divides)
{
	if (d == 0) {
		(*zero_divides)++;
		return 0;
	}
	return n / d;
}

static int32_t clamp32(int32_t x, int32_t lo, int32_t hi)
{
	return x < lo ? lo : x > hi ? hi : x;
}

/* --- tables ------------------------------------------------------------- */

/*
 * The 32 bins of the symbol distribution, extracted from the decoder.
 * Bins 1..15 are the mirror image of 30..16 (negative values); bins 0
 * and 31 are open-ended sentinels. threshold[k] is the start of bin k in
 * the 32-bit code space; recip = 2^32 / width (rounded down), so a value
 * inside a bin is (code - threshold) * recip >> 32, fixed up by one.
 */
const struct mqa_entropy_record mqa_entropy_records[MQA_ENTROPY_RECORDS] = {
	{ -1073741824, 0x00000000u, 0x00000000u,     0,  9251 },
	{     -524288, 0x00040000u, 0x80000000u,     2,  7173 },
	{     -393216, 0x00080000u, 0x1c71c71du,     9,  4424 },
	{     -327680, 0x00110000u, 0x035e50d8u,    76,  3408 },
	{     -262144, 0x005d0000u, 0x00ec9792u,   277,  2945 },
	{     -229376, 0x00e78000u, 0x005ff402u,   683,  2731 },
	{     -196608, 0x023d0000u, 0x00295252u,  1586,  2520 },
	{     -163840, 0x05560000u, 0x00135509u,  3390,  2323 },
	{     -131072, 0x0bf50000u, 0x000bd08fu,  5547,  2192 },
	{     -114688, 0x115fc000u, 0x0008c8a8u,  7461,  2141 },
	{      -98304, 0x18a90000u, 0x0006c2dcu,  9693,  2080 },
	{      -81920, 0x22204000u, 0x000568b8u, 12116,  2027 },
	{      -65536, 0x2df54000u, 0x00048164u, 14546,  1987 },
	{      -49152, 0x3c29c000u, 0x00037134u, 19039,  1954 },
	{      -32768, 0x4ec18000u, 0x0002d8d0u, 23020,  1925 },
	{      -16384, 0x653c8000u, 0x000263ffu, 27414,  1917 },
	{           0, 0x80020000u, 0x000263ffu, 27414,  1917 },
	{       16384, 0x9ac78000u, 0x0002d8d0u, 23020,  1925 },
	{       32768, 0xb1428000u, 0x00037134u, 19039,  1954 },
	{       49152, 0xc3da4000u, 0x00048164u, 14546,  1987 },
	{       65536, 0xd20ec000u, 0x000568b8u, 12116,  2027 },
	{       81920, 0xdde3c000u, 0x0006c2dcu,  9693,  2080 },
	{       98304, 0xe75b0000u, 0x0008c8a8u,  7461,  2141 },
	{      114688, 0xeea44000u, 0x000bd08fu,  5547,  2192 },
	{      131072, 0xf40f0000u, 0x00135509u,  3390,  2323 },
	{      163840, 0xfaae0000u, 0x00295252u,  1586,  2520 },
	{      196608, 0xfdc70000u, 0x005ff402u,   683,  2731 },
	{      229376, 0xff1c8000u, 0x00ec9792u,   277,  2945 },
	{      262144, 0xffa70000u, 0x035e50d8u,    76,  3408 },
	{      327680, 0xfff30000u, 0x1c71c71du,     9,  4424 },
	{      393216, 0xfffc0000u, 0x80000000u,     2,  7173 },
	{      524288, 0x00000000u, 0x00000000u,     0,  9251 },
};

/* First bin to try for each value of the code word's top nibble; the
 * exact bin is then found by walking downwards. */
static const uint8_t bin_by_nibble[16] = {
	8, 10, 12, 13, 14, 14, 15, 15, 16, 17, 17, 18, 19, 21, 23, 30,
};

/* Magnitude class of a value: index by |x| >> 14 (capped at 31). */
static const uint8_t class_by_magnitude[32] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 9, 9, 10, 10, 11, 11,
	12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14,
};

/* --- range coder -------------------------------------------------------- */

static uint8_t ring_next(struct mqa_byte_ring *r)
{
	uint8_t b = r->data[r->cursor];

	r->cursor = r->cursor + 1 >= r->size ? 0 : r->cursor + 1;
	return b;
}

static void range_refill(struct mqa_range_decoder *rc)
{
	uint32_t base = rc->base ? rc->base : 256u;

	rc->value = rc->value * base + ring_next(rc->src);
	rc->range = rc->range * base;
}

void mqa_range_normalize(struct mqa_range_decoder *rc)
{
	/* a coder with no range left cannot be renormalised: refilling
	 * multiplies, so the caller must have started it (range 1) */
	if (rc->range == 0)
		return;
	while (rc->range <= 0xffffffu)
		range_refill(rc);
}

/* --- symbol magnitude ---------------------------------------------------- */

/*
 * Which bin a signed magnitude falls in, as a signed index: 0..15 for
 * non-negative values, ~class (-1..-16) for negative ones, so that
 * index + 16 selects a record and mirrors around the zero bin.
 */
static int magnitude_bin(int32_t x)
{
	int32_t s = x >> 31;
	int32_t c = (x >> 14) ^ s;

	c = c <= 31 ? class_by_magnitude[c] : 15;
	return (int)(c ^ s);
}

/* Code-space start of value `x` within bin `rec`. */
uint32_t mqa_entropy_bin_position(const struct mqa_entropy_record *rec, int32_t x)
{
	return rec->threshold + (uint32_t)sub32(x, rec->base) * (uint32_t)(int32_t)rec->width;
}

const struct mqa_entropy_record *mqa_entropy_bin(int32_t x)
{
	return &mqa_entropy_records[magnitude_bin(x) + 16];
}

/*
 * Outcome of decoding one symbol's magnitude: the two quantised
 * magnitudes a = level * q and b = a + level * 256, their sum t, and a
 * magnitude measure m used to adapt level and the variance.
 */
struct magnitude {
	int32_t a, b, t, m;
};

/*
 * The escape form: the low 19 bits of value are a number below 32. The
 * interval collapses from 2^19 to 32 units, then an 8-bit residual is
 * taken straight from the coder (0x80 meaning "16-bit residual follows").
 */
static void decode_small(struct mqa_entropy_decoder *d, struct mqa_range_decoder *rc,
			 uint32_t dither, struct magnitude *mg)
{
	int32_t res, q;

	rc->range = (((rc->range - 32u) >> 19) + 1u) << 5;
	rc->value = (rc->value & 0x7ffffu) + ((rc->value >> 19) << 5);
	mqa_range_normalize(rc);

	res = (int32_t)(rc->value & 0xffu) - 0x80;
	rc->range >>= 8;
	rc->value >>= 8;
	if (res == 0) {
		range_refill(rc);
		mqa_range_normalize(rc);
		res = (int16_t)rc->value;
		rc->range >>= 16;
		rc->value >>= 16;
	}

	q = add32((int32_t)dither, mul32(res, 256));
	mg->a = mul32(d->level, q);
	mg->b = add32(mg->a, mul32(d->level, 256));
	mg->t = add32(mg->b, mg->a);
	mg->m = sub32(mg->t ^ (mg->t >> 31), mg->t >> 31) >> 7;   /* |t| >> 7 */
}

/*
 * The table form: locate the bin containing the code word, recover the
 * value inside it, quantise to the step size with the random offset,
 * then narrow the coder's interval to the bins of the two magnitudes.
 */
static void decode_large(struct mqa_entropy_decoder *d, struct mqa_range_decoder *rc,
			 uint32_t code, uint32_t dither, struct magnitude *mg)
{
	const struct mqa_entropy_record *rec, *ra, *rb;
	uint32_t off, q, lo_a, lo_b, lo, hi, span;
	int32_t v, s, qv;

	rec = &mqa_entropy_records[bin_by_nibble[code >> 28]];
	while (code < rec->threshold)
		rec--;

	off = code - rec->threshold;
	q = (uint32_t)(((uint64_t)off * rec->recip) >> 32);
	if (off < (uint32_t)rec->width * q)
		q--;
	v = add32((int32_t)q, rec->base);

	/* quantise |v| / level, keeping the low byte equal to the dither */
	s = v >> 31;
	qv = (int32_t)udiv((uint32_t)(v ^ s), (uint32_t)d->level, &d->zero_divides) ^ s;
	qv = add32(sub32(qv, (int32_t)dither) & ~0xff, (int32_t)dither);

	mg->a = mul32(d->level, qv);
	mg->b = add32(mg->a, mul32(d->level, 256));
	mg->t = add32(mg->b, mg->a);

	ra = &mqa_entropy_records[magnitude_bin(mg->a) + 16];
	rb = &mqa_entropy_records[magnitude_bin(mg->b) + 16];
	mg->m = (int32_t)ra->weight + rb->weight;

	lo_a = mqa_entropy_bin_position(ra, mg->a);
	lo_b = mqa_entropy_bin_position(rb, mg->b);
	/* narrow the interval: the code word is split into a 19-bit low part
	 * and a high part; both are re-based onto [lo, hi) in 2^13 units */
	lo = lo_a >> 13;
	hi = lo_b >> 13;
	span = hi - lo;
	rc->value = (rc->value >> 19) * span + ((rc->value & 0x7ffffu) - lo);
	rc->range = (((rc->range - hi) >> 19) + 1u) * span;
}

/* --- per-block LMS update of the filter taps ---------------------------- */

static void adapt_taps(struct mqa_entropy_decoder *d, uint32_t dither)
{
	int32_t h[MQA_ENTROPY_HISTORY], e[4], c[4];
	int k, j;

	for (k = 0; k < MQA_ENTROPY_HISTORY; k++)
		h[k] = d->history[(d->head + k) % MQA_ENTROPY_HISTORY];

	/* prediction error of the four oldest entries from the four newer */
	for (j = 4; j < 8; j++) {
		int32_t p = 0;

		for (k = 0; k < 4; k++)
			p = add32(p, mul32(d->coef[k], h[j - 3 + k]));
		e[j - 4] = add32(h[j - 4], p >> 12);
	}

	/* gradient step, with a little dither, then leaky taps for the AR stage */
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

/* --- one symbol ----------------------------------------------------------- */

static int32_t decode_symbol(struct mqa_entropy_decoder *d, int with_residual,
			     uint32_t *dither_out, unsigned pos)
{
	const struct mqa_gain_params *gp = &d->gain;
	struct mqa_range_decoder *rc = d->coder;
	struct magnitude mg;
	uint32_t s1, s2, dither, dscaled, code;
	int32_t v, sa, sb, pred, p1, p2, out, resid, pv;
	uint64_t acc_a, acc_b;
	int16_t hist_in;
	int k;

	/* two random draws */
	s1 = mqa_nr_lcg_step(d->rng);
	s2 = mqa_nr_lcg_step(s1);
	d->rng = s2;
	dither = s2 >> 24;
	dscaled = (uint32_t)(((uint64_t)s1 * gp->scale) >> 32);
	*dither_out = dither;

	/* decode the magnitude */
	mqa_range_normalize(rc);
	code = (rc->value << 13) | 0x1fffu;
	if ((rc->value & 0x7ffffu) < 32)
		decode_small(d, rc, dither, &mg);
	else
		decode_large(d, rc, code, dither, &mg);

	/* scale by the running variance, track it, adapt the step size */
	v = d->variance;
	sa = (int32_t)(uint32_t)(prod64(mg.a, v) >> 16);
	sb = (int32_t)(uint32_t)(prod64(mg.b, v) >> 16);
	d->variance = (int32_t)(uint32_t)(prod64(mg.m, v) >> 12);

	hist_in = (int16_t)clamp32(mg.t >> 9, -512, 512);
	d->head = (d->head + MQA_ENTROPY_HISTORY - 1) % MQA_ENTROPY_HISTORY;
	d->history[d->head] = hist_in;

	pred = add32(sa, sb) >> 1;
	d->level = clamp32((int32_t)udiv((uint32_t)mul32(d->level_rate, d->level),
					  (uint32_t)mg.m, &d->zero_divides),
			   d->level_min, d->level_max);

	/* two AR stages over the last four predictions; the taps rotate
	 * with the symbol's position in its block, and the prediction
	 * slots are filled from the top down (3, 2, 1, 0) */
	acc_a = acc_b = 0;
	for (k = 0; k < MQA_ENTROPY_TAPS; k++) {
		int32_t tap = d->taps[(k + pos) % MQA_ENTROPY_TAPS];

		acc_a += prod64(tap, d->pred_a[k]);
		acc_b += prod64(tap, d->pred_b[k]);
	}
	p1 = sub32(pred, mul32((int32_t)(uint32_t)(acc_a >> 32), 16));
	p2 = sub32(p1, mul32((int32_t)(uint32_t)(acc_b >> 32), 16));
	d->pred_a[MQA_ENTROPY_TAPS - 1 - pos] = p1;
	d->pred_b[MQA_ENTROPY_TAPS - 1 - pos] = p2;

	if (!with_residual)
		return p2;

	/* range-coded residual, decoded only when the gain-shaped interval
	 * between the two scaled magnitudes is wider than one unit */
	{
		int32_t hi_in = add32(2 * sub32(sb, (int32_t)dscaled), 1);
		int32_t lo_in = sub32(2 * sub32(sa, (int32_t)dscaled), 1);
		int32_t hi_g = mqa_mulhi32(hi_in, gp->gain) >> gp->shift;
		int32_t lo_g = mqa_mulhi32(lo_in, gp->gain) >> gp->shift;
		int32_t n = sub32(hi_g, lo_g);

		resid = hi_g;
		if (n > 1) {
			struct mqa_range_decoder *rr = d->residual;
			uint32_t recip = 0xffffffffu / (uint32_t)n;
			uint32_t quot, rem, top;

			mqa_range_normalize(rr);
			quot = (uint32_t)(((uint64_t)rr->value * recip) >> 32);
			rem = rr->value - quot * (uint32_t)n;
			top = (uint32_t)(((uint64_t)recip * (rr->range - 1u)) >> 32);
			if (rem >= (uint32_t)n)
				rem -= (uint32_t)n;
			rr->range = top + 1u;
			rr->value = quot;
			resid = sub32(resid, (int32_t)rem);
		}
	}

	pv = sub32(p2, pred);
	if (d->scale2 == gp->scale)
		pv = mqa_gain_step(gp, mqa_gain_input(gp, pv));
	out = add32(add32(mul32((int32_t)gp->scale, resid), (int32_t)dscaled), pv);
	return out;
}

void mqa_entropy_decode(struct mqa_entropy_decoder *d, int32_t *out, unsigned count,
			int with_residual)
{
	unsigned done = 0;
	uint32_t dither = 0;

	/* the decoder's fourth argument: only its low bit, and only if the
	 * object has a non-zero scale */
	with_residual = d->gain.scale ? (with_residual & 1) : 0;

	while (done < count) {
		unsigned end = done + MQA_ENTROPY_BLOCK < count ? done + MQA_ENTROPY_BLOCK : count;
		unsigned pos;

		for (pos = 0; done < end; done++, pos++)
			out[done] = decode_symbol(d, with_residual, &dither, pos);

		/* end of block: adapt the filter, keep the variance off the floor */
		adapt_taps(d, dither);
		if (d->variance < (int32_t)(d->scale2 << 1))
			d->variance = (int32_t)(d->scale2 << 1);
	}
}
