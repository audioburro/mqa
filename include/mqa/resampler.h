/*
 * mqa/resampler.h -- the passthrough resampler and the output-rate status
 * that drives it.
 *
 * Samples the decoder does not unfold (a stream that is over, or input
 * that never was MQA) pass through the output stage. Before they do, the
 * reference decoder re-reads the output descriptor's rate code, format
 * and low nibble and, when any of them changed, picks a resampler record
 * for the new rate (a coefficient table with a taps-per-phase count and
 * an output:input ratio) and points the resampler at it. The resampler's
 * history is the carrier pair ring of the output stage; switching
 * records rotates that ring so its newest pair is at the front and
 * restarts the phase accumulator. The resampling itself (a polyphase
 * FIR with dither) is not implemented here: passed-through samples are
 * only copied when the status says no rate change applies.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_RESAMPLER_H
#define MQA_DECODE_RESAMPLER_H

#include <stddef.h>
#include <stdint.h>

struct mqa_resampler_spec {
	const int32_t *table;         /* coefficient rows, `taps` per phase   */
	unsigned taps;
	unsigned num, den;            /* output : input rate ratio            */
};

extern const struct mqa_resampler_spec mqa_resampler_identity;      /* 1:1 */
extern const struct mqa_resampler_spec *const mqa_resampler_single[20];
extern const struct mqa_resampler_spec *const mqa_resampler_group[20][16];
extern const struct mqa_resampler_spec *const mqa_resampler_special;

/* The sample rates a 5-bit rate code names: rates[code >> 3] << (code & 7). */
#define MQA_RATE_CODE_NONE 31
extern const int32_t mqa_rate_code_base[3];

/*
 * Output-rate status: what the descriptor last
 * announced and what the resampler was configured for.
 */
struct mqa_rate_status {
	unsigned code;                /* descriptor rate code last seen (31: none) */
	unsigned select;              /* which record column the owner selected    */
	unsigned code_out;            /* rate code of the resampled output         */
	unsigned format;              /* descriptor format field, low 3 bits       */
	uint8_t desc_low;             /* descriptor low nibble                     */
	uint8_t active;               /* a non-zero stream format was seen         */
	unsigned state;               /* resampling state (0, 2: none)             */
	unsigned flags;               /* owner-set option bits                     */
	unsigned aux;                 /* owner-set mode (0..3)                     */
};

#endif
