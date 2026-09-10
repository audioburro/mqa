/*
 * mqae/carrier.h -- putting the two hidden channels into the PCM.
 *
 * A carrier sample is a 24-bit PCM word with its bottom bits spoken for:
 *
 *   bits 0..7        the data channel, one byte per sample (chan.h)
 *   bit  8 + xbit    the control channel, as the exclusive-or of the
 *                    left and right samples' bit, one bit per frame
 *                    (bits.h)
 *
 * Writing them is quantisation rather than masking: the sample's low
 * bits are constrained to what the channels need, and the nearest sample
 * satisfying the constraint is chosen, so the error is at most half a
 * step of 2^(9+xbit). That error is the price of the channels, and it is
 * why the encoder's analysis has to run against the carrier it wrote
 * rather than the one it wanted.
 *
 * Samples are 32-bit words with the 24-bit sample in the top 24 bits, as
 * the decoder's owners hand them over.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQAE_CARRIER_H
#define MQAE_CARRIER_H

#include <stddef.h>
#include <stdint.h>

/* The largest 24-bit magnitude a carrier may reach: embedding needs a
 * step of headroom, so that rounding a sample up to a legal value can
 * never wrap it round to full negative. */
#define MQAE_CARRIER_HEADROOM(xbit) (1 << (9 + (xbit)))

/*
 * One frame. `l`/`r` are the carrier sample words, updated in place;
 * `bit` is the control channel's bit for this frame and `bl`/`br` the
 * data channel's two bytes.
 */
void mqae_carrier_frame(int32_t *l, int32_t *r, unsigned xbit, unsigned bit,
			unsigned bl, unsigned br);

/*
 * The same in two halves, for an encoder that has to know the carrier
 * before it knows what the data channel will say.
 *
 * A decoder clears the low byte before it does anything else, so what
 * it reconstructs from depends only on the bits above it. Fixing those
 * first, with the byte left at the middle of its range, means the byte
 * can be filled in later without moving anything the decoder will look
 * at -- and the error that leaves is centred on zero rather than always
 * downwards. Between the two calls the encoder can work out what the
 * byte should say, which depends on the carrier.
 */
void mqae_carrier_upper(int32_t *l, int32_t *r, unsigned xbit, unsigned bit);

/* Substitute the data channel's two bytes into a placed frame. */
static inline void mqae_carrier_bytes(int32_t *l, int32_t *r, unsigned bl, unsigned br)
{
	*l = (int32_t)(((uint32_t)*l & ~0xff00u) | (bl & 0xffu) << 8);
	*r = (int32_t)(((uint32_t)*r & ~0xff00u) | (br & 0xffu) << 8);
}

/* The 24-bit value nearest `s` whose low byte is `byte` and whose bit
 * `k` (0 for none, 8..15) is `bit`; clamped to fit 24 bits. */
int32_t mqae_carrier_place(int32_t s, unsigned byte, int k, unsigned bit);

#endif
