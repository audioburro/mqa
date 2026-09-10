/*
 * mqa/descrambler.h -- recovering the data channel from the carrier.
 *
 * Every carrier sample contributes one byte, the low byte of the sample
 * XORed with the top byte of a generator state; there is one generator
 * per channel, stepped once per frame, and both are re-seeded every 2048
 * byte pairs from a running reseed count. A group of 32 carrier samples
 * per channel yields 64 bytes, which go to the message parser
 * (stream.h).
 *
 * Section 5.1 of docs/mqa-stage1-spec.md specifies the recurrence, the
 * reseed and the state a decoder must reproduce to start at an arbitrary
 * position; the constants are in A.5.
 *
 * The source buffers are the decoder's 832-word carrier rings, and reads
 * wrap at their end. A fresh descrambler has a zero budget, so its first
 * fill reseeds at once. mqa_descrambler_start() seeds for an arbitrary
 * byte position, advancing the generator with a jump table rather than
 * step by step.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_DESCRAMBLER_H
#define MQA_DECODE_DESCRAMBLER_H

#include <stddef.h>
#include <stdint.h>

#define MQA_DESCRAMBLER_RING_SIZE   832    /* words per source ring buffer */
#define MQA_DESCRAMBLER_BATCH       2048   /* byte pairs per reseed        */

struct mqa_descrambler {
	uint32_t lcg[2];         /* scrambler states for buffer A and B */
	uint32_t reseeds;        /* how many times the LCGs were reseeded */
	uint32_t budget;         /* byte pairs left before the next reseed */
};

/* Fresh descrambler: zero states, so the first fill reseeds immediately. */
void mqa_descrambler_init(struct mqa_descrambler *ks);

/* Reseed both LCGs from the reseed counter (the decoder's fixed recipe). */
void mqa_descrambler_reseed(struct mqa_descrambler *ks);

/* Start (or restart) at an absolute sample position: the batch it falls
 * in is seeded and the pair advanced to the position within it. */
void mqa_descrambler_start(struct mqa_descrambler *ks, uint32_t position);

/*
 * Descramble `nbytes` bytes of an interleaved (A, B, A, B, ...) byte
 * buffer starting at src[start] into out[0..]; an odd count begins with
 * a lone B byte. Used for a packet's leading bytes at stream start.
 */
void mqa_descrambler_fill_bytes(struct mqa_descrambler *ks, uint8_t *out,
				const uint8_t *src, unsigned start, unsigned nbytes);

/*
 * Produce `count` byte pairs into `out` from ring buffers `a`/`b`, reading
 * from ring index `pos`. Returns the ring index after the last word read
 * (wrapped to 0 at the buffer end), which the caller passes back next time.
 */
unsigned mqa_descrambler_fill(struct mqa_descrambler *ks, uint8_t *out,
			    const int32_t *a, const int32_t *b,
			    unsigned pos, unsigned count);

#endif
