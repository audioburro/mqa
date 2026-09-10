/*
 * mqa/carrier.h -- extracting the data digits hidden in carrier samples.
 *
 * Besides the descrambled message stream (descrambler.h), each carrier
 * sample carries one small "digit" that the residual stage consumes
 * directly. For a group the decoder takes each sample of both channels,
 * drops the low `shift` + 8 bits, offsets the result and reduces it
 * modulo the class's level count M:
 *
 *     v     = (sample >> (shift + 8)) + 0x690000
 *     digit = v mod M               M in {1, 3, 4, 8} by class
 *
 * The reduction is done exactly as the reference decoder does it, by
 * multiplying with a 32-bit reciprocal of M and taking the high word of
 * the product with M: identical to v mod M for every value that fits
 * the carrier's range, and bit-exact beyond it. Output is interleaved:
 * digits[2i] from channel A, digits[2i + 1] from channel B.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_CARRIER_H
#define MQA_DECODE_CARRIER_H

#include <stdint.h>

struct mqa_carrier_class {
	uint8_t  levels;       /* M: digit values per sample            */
	uint8_t  param[3];     /* further per-class bytes, not used here */
	uint32_t word;         /* a further per-class word, not used here*/
	uint32_t recip;        /* ceil(2^32 / M), 0 for M == 1           */
};

#define MQA_CARRIER_CLASSES 4
extern const struct mqa_carrier_class mqa_carrier_classes[MQA_CARRIER_CLASSES];

/* Extract `count` digit pairs from a[] and b[] into digits[2 * count]. */
void mqa_carrier_digits(const int32_t *a, const int32_t *b, unsigned count,
			unsigned shift, const struct mqa_carrier_class *cls,
			uint8_t *digits);

#endif
