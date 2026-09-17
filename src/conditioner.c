/*
 * conditioner.c -- the two per-sample stages the intake runs over a
 * group after capturing its data channel (see conditioner.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/conditioner.h"
#include "mqa/lcg.h"

#define TAPS MQA_CONDITIONER_TAPS

/* --- the noise sources ----------------------------------------------------- */

/* The per-sample generator: two LCG states advanced together; the noise
 * byte is the old state's top byte. */
#define NOISE_MUL 0x17385ca9u
#define NOISE_ADD 0x47502932u

/* Seeding: the block number and the seed squared, then two steps of a
 * second LCG give the two states. Both stages reseed every 4096 samples. */
static void seed_noise(uint32_t s[2], uint32_t seed, uint32_t block)
{
	uint32_t x = (block + seed) * (block + seed);

	s[0] = x * 0x19660du + 0x3c6ef35fu;
	s[1] = s[0] * 0x19660du + 0x3c6ef35fu;
}

/* --- the bit ring --------------------------------------------------------- */

void mqa_bitring_reset(struct mqa_bitring *r)
{
	memset(r, 0, sizeof *r);
}

void mqa_bitring_append(struct mqa_bitring *r, unsigned bit)
{
	uint32_t mask = 1u << (r->wpos & 31);

	if (bit)
		r->word[r->wpos >> 5] |= mask;
	else
		r->word[r->wpos >> 5] &= ~mask;
	r->wpos = (r->wpos + 1) & (MQA_BITRING_BITS - 1);
}

/* 32 bits from the read position (not consumed). */
static uint32_t bitring_peek(const struct mqa_bitring *r)
{
	unsigned i = r->rpos >> 5, s = r->rpos & 31;
	uint64_t w = (uint64_t)r->word[i] | (uint64_t)r->word[(i + 1) & (MQA_BITRING_BITS / 32 - 1)] << 32;

	return (uint32_t)(w >> s);
}

static void bitring_skip(struct mqa_bitring *r, unsigned n)
{
	r->rpos = (r->rpos + n) & (MQA_BITRING_BITS - 1);
}

/* 32 bits of a word table from bit position p (the table holds at least
 * one word past the one p falls in). */
static uint32_t table_bits(const uint32_t *t, unsigned p)
{
	uint64_t w = (uint64_t)t[p >> 5] | (uint64_t)t[(p >> 5) + 1] << 32;

	return (uint32_t)(w >> (p & 31));
}

/* --- configuration ---------------------------------------------------------- */

/* The noise-shaping filters, one per rate family: whether all six taps
 * are used, then the taps in Q11. */
static const int32_t shaping[4][8] = {
	{ 1, 0, 2866,  811, 305, 396, -647, -551 },
	{ 1, 0, 3006,  946, 162, 446, -351, -445 },
	{ 0, 0, 3797, 2667, 668,   0,    0,    0 },
	{ 0, 0, 2867, 1834, 446,   0,    0,    0 },
};

/* 2^(-level/256) in Q31, by the reference's own approximation. */
int32_t mqa_conditioner_gain(uint32_t level)
{
	uint32_t frac = level << 16, e = level >> 16;
	uint32_t t = (uint32_t)(((uint64_t)0x55555555u * frac) >> 32);
	uint32_t u = (uint32_t)(((uint64_t)t * frac) >> 32);

	return (int32_t)(t + 0x80000000u - (u >> 2)) >> e;
}

/* 2^(index/32) in Q15. */
uint32_t mqa_conditioner_gain_index(unsigned index)
{
	static const uint16_t table[16] = {
		32768, 33486, 34219, 34968, 35734, 36516, 37316, 38133,
		38968, 39821, 40693, 41584, 42495, 43425, 44376, 45348,
	};

	return table[index & 15];
}

uint32_t mqa_conditioner_lag_strength(uint32_t v)
{
	uint32_t x = v << 16;
	uint32_t hi = (uint32_t)(((uint64_t)0x947bu * x) >> 32);

	hi = 94208 - hi + 340;
	return (uint32_t)(((uint64_t)hi * x) >> 32);
}

void mqa_conditioner_configure(struct mqa_conditioner *c, unsigned rate_code, unsigned dither_mode,
			       unsigned shift, int32_t gain2, const uint32_t ctx[4])
{
	int row = (int)(rate_code & 7) * 3 + ((int)(int8_t)rate_code >> 3);
	int32_t strength;

	memset(c, 0, sizeof *c);
	if (row >= 3)
		row = 3;
	c->six_taps = shaping[row][0] != 0;
	c->taps = &shaping[row][2];
	c->shift = shift & 31;
	c->full = (int32_t)(8388608u >> c->shift);
	c->limit = c->full - 8192;
	c->limit_hi = c->full - 1024;
	c->dither_mode = dither_mode;
	c->gain2 = gain2;
	c->gain = mqa_conditioner_gain(ctx[2]);
	strength = (int32_t)(14976u * ctx[3]);
	c->lag_prev = strength;
	c->lag_out = (int32_t)((uint32_t)-strength << 2);
	seed_noise(c->lcg1, 0xe7e1faeeu, 0);
	c->seed2 = dither_mode == 1 ? 0xf807b7dfu : 0xd5c31f79u;
	seed_noise(c->lcg2, c->seed2, 0);
	c->steady = 1;
}

void mqa_conditioner_seek(struct mqa_conditioner *c, uint32_t position)
{
	unsigned k;

	c->count1 = c->count2 = position;
	c->steady = 0;
	seed_noise(c->lcg1, 0xe7e1faeeu, position >> 12);
	seed_noise(c->lcg2, c->seed2, position >> 12);
	for (k = 0; k < 2; k++) {
		c->lcg1[k] = mqa_lcg_jump(c->lcg1[k], position & 0xfff);
		c->lcg2[k] = mqa_lcg_jump(c->lcg2[k], position & 0xfff);
	}
}

void mqa_conditioner_set_marker(struct mqa_conditioner *c, uint32_t at, const uint32_t *words, unsigned n)
{
	unsigned i;

	c->marker_pos = at;
	for (i = 0; i < n && i < MQA_CONDITIONER_MARKER_WORDS; i++)
		c->marker[i] = words[i];
}

/* --- the error-feedback filter ------------------------------------------------ */

/* Push the pair's errors and return the filter's outputs for them. */
static void shape(const struct mqa_conditioner *c, int32_t hist[TAPS][2], unsigned *h,
		  int32_t ea, int32_t eb, int32_t out[2])
{
	const int32_t *t = c->taps;
	unsigned k, ch;

	*h = (*h + TAPS - 1) % TAPS;
	hist[*h][0] = ea;
	hist[*h][1] = eb;
	for (ch = 0; ch < 2; ch++) {
		uint32_t acc = 0;

		for (k = 0; k < (c->six_taps ? 6u : 3u); k++)
			acc += (uint32_t)t[k] * (uint32_t)hist[(*h + k) % TAPS][ch];
		out[ch] = (int32_t)acc;
	}
}

/* --- stage 1: requantisation ----------------------------------------------------- */

/* Round to a multiple of 256 with the noise byte as subtractive dither,
 * then put the noise byte back: the low byte is filled and the rounding
 * error is uniform. */
static int32_t dither_round(int32_t x, uint32_t noise)
{
	return (int32_t)(((uint32_t)(x + 128 - (int32_t)noise)) & ~255u) + (int32_t)noise;
}

/* Near full scale the reference folds the value back with bits 0..8
 * cleared; ported as it stands. */
static int32_t fold(int32_t out, int32_t in, int32_t full, int32_t *err, int32_t neg_e)
{
	int32_t o = out & ~0x1ff, i = in & ~0x1ff, sg = (out >> 31) & ~0x1ff;
	int32_t t = (int32_t)((((uint32_t)i << 1) - (uint32_t)o) ^ (uint32_t)sg) - full;
	int32_t v = out;

	if (t > 0) {
		v = out + (t ^ sg);
		*err = neg_e;
	}
	return v & ~512;
}

static void requantise(struct mqa_conditioner *c, int32_t *a, int32_t *b, unsigned n)
{
	unsigned done = 0;

	while (done < n) {
		uint32_t count = c->count1;
		unsigned until = (0u - count) & 0xfff, chunk, marker_n = 0, i, mpos = 0;

		if (until == 0) {
			seed_noise(c->lcg1, 0xe7e1faeeu, count >> 12);
			until = 4096;
		}
		chunk = n - done < until ? n - done : until;
		if (c->marker_pos >= count && count != 0) {
			if (c->marker_pos == count)
				marker_n = 6;
			else if (chunk > c->marker_pos - count)
				chunk = c->marker_pos - count;
		}
		c->count1 = count + chunk;

		for (i = 0; i < chunk; i++) {
			uint32_t na = c->lcg1[0] >> 24, nb = c->lcg1[1] >> 24;
			int32_t x = a[done + i] >> c->shift, y = b[done + i] >> c->shift;
			int32_t out_a, out_b, err_a, err_b, filt[2];

			c->lcg1[0] = c->lcg1[0] * NOISE_MUL + NOISE_ADD;
			c->lcg1[1] = c->lcg1[1] * NOISE_MUL + NOISE_ADD;
			if (i < marker_n) {
				/* the marker: coarse steps, the packet's bits in the low bits */
				uint32_t da = na + (table_bits(c->marker, mpos) << 8);
				uint32_t db = nb + (table_bits(c->marker, mpos + 5) << 8);
				int32_t q1 = (int32_t)(((uint32_t)(((x + y) >> 1) - (int32_t)da + 4096)) & ~0x1fffu) + (int32_t)da;
				int32_t q2 = (int32_t)(((uint32_t)(((x - y) >> 1) - (int32_t)db + 4096)) & ~0x1fffu) + (int32_t)db;

				mpos += 10;
				out_a = q1 + q2;
				out_b = q1 - q2;
				err_a = x - out_a;
				err_b = y - out_b;
			} else {
				int32_t e1 = c->acc1[0] >> 11, e2 = c->acc1[1] >> 11;
				int32_t m = e1 + x, s = e2 + y;
				int32_t q1 = dither_round((s + m) >> 1, na);
				int32_t q2 = dither_round((m - s) >> 1, nb);

				out_a = q1 + q2;
				out_b = q1 - q2;
				err_a = x - out_a;
				err_b = y - out_b;
				if (((out_a >> 31) ^ out_a) >= c->limit)
					out_a = fold(out_a, x, c->full, &err_a, -e1);
				if (((out_b >> 31) ^ out_b) >= c->limit)
					out_b = fold(out_b, y, c->full, &err_b, -e2);
			}
			shape(c, c->err1, &c->h1, err_a, err_b, filt);
			c->acc1[0] = filt[0];
			c->acc1[1] = filt[1];
			a[done + i] = out_a;
			b[done + i] = out_b;
		}
		done += chunk;
	}
}

/* --- stage 2: signalling ---------------------------------------------------------- */

/* Samples close to full scale carry bits of the control stream in a
 * small code: one bit in the lower band, a nibble-selected code above. */
static void code_near_full_scale(struct mqa_conditioner *c, int32_t *s, unsigned n, struct mqa_bitring *ring)
{
	static const struct { int8_t len; int16_t offset; } code[16] = {
		{ 4, 0 }, { 7, 16 }, { 7, 144 }, { 7, 272 }, { 7, 400 }, { 7, 528 }, { 7, 656 }, { 7, 784 },
		{ 8, 912 }, { 8, 1168 }, { 8, 1424 }, { 8, 1680 }, { 9, 1936 }, { 9, 2448 }, { 10, 2960 }, { 15, 3984 },
	};
	unsigned i;

	for (i = 0; i < n; i++) {
		int32_t v = s[i], sign = v >> 31, mag = v ^ sign;
		uint32_t bits, value;
		unsigned used;

		if (mag < c->limit)
			continue;
		bits = bitring_peek(ring);
		if (mag < c->limit_hi) {
			value = bits & 1;
			used = 1;
		} else {
			unsigned k = bits & 15;

			value = ((bits >> 4) & ((1u << code[k].len) - 1)) + (uint32_t)code[k].offset;
			used = (unsigned)code[k].len + 4;
		}
		bitring_skip(ring, used);
		s[i] = (int32_t)(((uint32_t)mag + ((value + (uint32_t)sign) << 9)) ^ (uint32_t)sign);
	}
}

static int32_t abs32(int32_t x)
{
	return (x ^ (x >> 31)) - (x >> 31);
}

static void signal(struct mqa_conditioner *c, int32_t *a, int32_t *b, unsigned n, struct mqa_bitring *ring)
{
	uint32_t count = c->count2;
	uint32_t noise[32][2];
	unsigned i, embed_from = n, mpos = 60;
	uint32_t ring_gap = 0;
	int marker_now;

	if (n > 32)
		n = 32;                                   /* the reference's group size */
	if ((count & 0xfff) == 0)
		seed_noise(c->lcg2, c->seed2, count >> 12);
	marker_now = c->marker_pos == count && count != 0;
	c->count2 = count + n;

	if (c->steady || marker_now) {
		code_near_full_scale(c, a, n, ring);
		code_near_full_scale(c, b, n, ring);
	}
	if (marker_now) {
		embed_from = 24;
		ring_gap = (ring->wpos - ring->rpos) & (MQA_BITRING_BITS - 1);
	}
	for (i = 0; i < n; i++) {
		if (c->dither_mode) {
			noise[i][0] = c->lcg2[0];
			noise[i][1] = c->lcg2[1];
			c->lcg2[0] = c->lcg2[0] * NOISE_MUL + NOISE_ADD;
			c->lcg2[1] = c->lcg2[1] * NOISE_MUL + NOISE_ADD;
		} else {
			noise[i][0] = noise[i][1] = 0;
		}
	}

	for (i = 0; i < n; i++) {
		/* the level gain on the doubled, negated sample, less the feedback */
		int64_t ua = (int32_t)((0u - (uint32_t)a[i]) << 1), ub = (int32_t)((0u - (uint32_t)b[i]) << 1);
		int32_t xa = (int32_t)((ua * c->gain) >> 32) - c->lag[0];
		int32_t xb = (int32_t)((ub * c->gain) >> 32) - c->lag[1];
		uint32_t na = noise[i][0] >> 24, nb = noise[i][1] >> 24;
		int32_t out_a, out_b, filt[2];

		if (i >= embed_from) {
			uint32_t da = na + (table_bits(c->marker, mpos) << 8);
			uint32_t db = nb + (table_bits(c->marker, mpos + 5) << 8);

			mpos += 10;
			out_a = (int32_t)(((uint32_t)(xa + 4096 - (int32_t)da)) & ~0x1fffu) + (int32_t)da;
			out_b = (int32_t)(((uint32_t)(xb + 4096 - (int32_t)db)) & ~0x1fffu) + (int32_t)db;
		} else if (c->steady) {
			int32_t pa = xa + (c->acc2[0] >> 11), pb = xb + (c->acc2[1] >> 11);
			int32_t ea = pa - dither_round(pa, na), eb = pb - dither_round(pb, nb);
			int32_t threshold = (c->gain >> 23) + 255;

			/* one control bit in the rounding direction of the larger
			 * error, when both errors are clear of the threshold */
			if (threshold < abs32(ea + eb + 1) && threshold < abs32(ea - eb)) {
				uint32_t flip = (bitring_peek(ring) & 1) ? 0xffffff00u : 0;

				bitring_skip(ring, 1);
				if (abs32(ea) < abs32(eb))
					eb = (int32_t)((uint32_t)eb ^ flip);
				else
					ea = (int32_t)((uint32_t)ea ^ flip);
			}
			out_a = pa - ea;
			out_b = pb - eb;
		} else {
			out_a = xa;                       /* ramping: the reference passes the sample through */
			out_b = xb;
			c->unsupported = 1;
		}
		a[i] = out_a;
		b[i] = out_b;

		/* the leaky feedback for the next sample, from this output and the last */
		c->lag[0] = (int32_t)(((int64_t)out_a * c->lag_out + (int64_t)c->prev[0] * c->lag_prev) >> 32);
		c->lag[1] = (int32_t)(((int64_t)out_b * c->lag_out + (int64_t)c->prev[1] * c->lag_prev) >> 32);
		c->prev[0] = out_a;
		c->prev[1] = out_b;
		shape(c, c->err2, &c->h2, xa - out_a, xb - out_b, filt);
		c->acc2[0] = filt[0];
		c->acc2[1] = filt[1];
	}

	if (c->gain2 != 256) {
		/* the shift is undone: an 8.8 gain, the noise word as rounding */
		for (i = 0; i < n; i++) {
			a[i] = (int32_t)((uint32_t)(a[i] >> 8) * (uint32_t)c->gain2 + (uint32_t)(((uint64_t)(uint32_t)c->gain2 * noise[i][0]) >> 32));
			b[i] = (int32_t)((uint32_t)(b[i] >> 8) * (uint32_t)c->gain2 + (uint32_t)(((uint64_t)(uint32_t)c->gain2 * noise[i][1]) >> 32));
		}
	}
	if (marker_now) {
		/* the marker's last field moves the bitstream's read position,
		 * relative to where it stood when the group began */
		uint32_t base = ((ring->wpos - ring->rpos) & (MQA_BITRING_BITS - 1)) + ring->rpos - ring_gap;

		ring->rpos = (base + (table_bits(c->marker, mpos) & 63)) & (MQA_BITRING_BITS - 1);
		c->steady = 1;
	}
}

void mqa_conditioner_run(struct mqa_conditioner *c, int32_t *a, int32_t *b, unsigned n, struct mqa_bitring *ring)
{
	requantise(c, a, b, n);
	signal(c, a, b, n, ring);
}
