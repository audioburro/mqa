/*
 * mqa/lsb_correction.h -- the final per-sample +-1 correction.
 *
 * After reconstruction every output pair receives a correction of -1, 0
 * or +1 per channel, chosen by a 24-bit scrambler register (a reflected
 * CRC-24 clocked with zero bytes, crc24.h), a priming bit stream (one
 * byte per eight samples from a short segment, MSB first) and a 16-entry
 * table of correction pairs indexed by three bits of the scrambler and
 * one bit derived from the samples. Section A.9 of
 * docs/mqa-stage1-spec.md gives the table and the register.
 *
 * The first priming segment is a fixed constant, six bytes and a CRC-32
 * byte. When it runs out the reference installs a fresh one through a
 * callback; what that fetches has been characterised (a C string from
 * the decoder object, again CRC-terminated) but not verified against a
 * stream that needs it, so the refresh is left to the caller. See the
 * top README's open questions.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_LSB_CORRECTION_H
#define MQA_DECODE_LSB_CORRECTION_H

#include <stdint.h>
#include "mqa/crc24.h"

/* Correction pairs {dL, dR}, extracted from the decoder. */
extern const int8_t mqa_lsb_correction_table[16][2];

/* The initial, fixed priming segment (payload + CRC-32 byte). */
#define MQA_LSB_INITIAL_SEGMENT_LEN 7
extern const uint8_t mqa_lsb_initial_segment[MQA_LSB_INITIAL_SEGMENT_LEN];

struct mqa_lsb_corrector;

/*
 * Called when the priming segment is exhausted. Must install a new
 * segment via mqa_lsb_set_segment(). If NULL, the current segment is
 * simply re-used from its start.
 */
typedef void (*mqa_lsb_refill_fn)(struct mqa_lsb_corrector *c, void *user);

struct mqa_lsb_corrector {
	struct mqa_crc24 crc;
	uint32_t scrambler;        /* CRC-24 register                          */
	uint32_t shift;            /* priming bits, consumed from bit 31 down   */
	unsigned samples_seen;     /* modulo 8: when to fetch the next byte     */

	const uint8_t *segment;
	unsigned segment_len;
	unsigned cursor;           /* next unread byte of segment[]             */

	mqa_lsb_refill_fn refill;
	void *user;
};

void mqa_lsb_init(struct mqa_lsb_corrector *c, mqa_lsb_refill_fn refill, void *user);

/* Replace the priming segment and rewind to its start. */
void mqa_lsb_set_segment(struct mqa_lsb_corrector *c, const uint8_t *segment, unsigned len);

/* Apply the correction to one (L, R) sample pair in place. */
void mqa_lsb_correct(struct mqa_lsb_corrector *c, int32_t *l, int32_t *r);

#endif
