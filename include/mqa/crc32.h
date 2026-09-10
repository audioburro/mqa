/*
 * mqa/crc32.h -- the reflected CRC-32 (polynomial 0xedb88320) the
 * reference decoder runs over sample words: the intake keeps one over
 * the input samples, the output stage one over its output.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_CRC32_H
#define MQA_DECODE_CRC32_H

#include <stdint.h>

/* Absorb one 32-bit word, least significant byte first. */
uint32_t mqa_crc32_word(uint32_t crc, uint32_t word);

#endif
