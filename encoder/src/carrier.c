/*
 * Putting the hidden channels into the PCM -- see mqae/carrier.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mqae/carrier.h"

#define FULL  8388607        /* 2^23 - 1 */

int32_t mqae_carrier_place(int32_t s, unsigned byte, int k, unsigned bit)
{
	int32_t step, base, up;

	/* the constrained bits, and the step between values that satisfy
	 * them: the low byte alone (256), or the byte and one bit above it */
	if (k < 8) {
		step = 256;
		base = (int32_t)(((uint32_t)s & ~255u) | (byte & 255u));
	} else {
		step = (int32_t)1 << (k + 1);
		base = (int32_t)(((uint32_t)s & ~(uint32_t)(step - 1)) |
				 ((bit & 1u) << k) | (byte & 255u));
	}
	if (base > s)
		base -= step;                  /* round down first */
	up = base + step;
	if (up > FULL)
		return base;
	if (base < -FULL)
		return up;
	return (s - base) <= (up - s) ? base : up;
}

void mqae_carrier_upper(int32_t *l, int32_t *r, unsigned xbit, unsigned bit)
{
	mqae_carrier_frame(l, r, xbit, bit, 128, 128);
}

void mqae_carrier_frame(int32_t *l, int32_t *r, unsigned xbit, unsigned bit,
			unsigned bl, unsigned br)
{
	int32_t a = *l >> 8, b = *r >> 8;      /* the 24-bit samples */
	unsigned k = 8 + xbit;

	/* the right sample carries a data byte only; the left one then
	 * carries the control bit, as the pair's exclusive-or at bit k */
	b = mqae_carrier_place(b, br, -1, 0);
	a = mqae_carrier_place(a, bl, (int)k, (bit ^ ((uint32_t)b >> k)) & 1u);
	*l = (int32_t)((uint32_t)a << 8);
	*r = (int32_t)((uint32_t)b << 8);
}
