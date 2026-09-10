/*
 * mqa/recon2.h -- the alternative reconstruction kernel (variant 0).
 *
 * A stream's parameter record selects one of two reconstruction filters
 * (output_stage.h): the short one in reconstruct.h, or this one, which
 * most 44.1 kHz material uses. It doubles the rate the same way, one
 * carrier sample and one residual in and two output samples per channel
 * out, but it runs a 4-tap interpolator and a 14-tap shaping filter ahead
 * of the two lifting steps, and works eight taps behind its input.
 *
 * Section 8.3 of docs/mqa-stage1-spec.md specifies the tap; 8.4 the
 * warm-up, the eight-tap skip at the start of every group and the flush
 * at the end of a stream. The coefficients are entries 4..25 of
 * mqa_recon_coeff_table (A.7); the three `m` coefficients come from a
 * table of four triples chosen by the parameter record.
 *
 * Because the kernel lags by MQA_RECON2_LATENCY taps, a group of 32
 * carrier samples yields 48 output samples, not 64, and the first eight
 * taps of a group go through the filter without producing output. The
 * output stage handles that; this header only knows how to run a tap.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_RECON2_H
#define MQA_DECODE_RECON2_H

#include <stdint.h>
#include "mqa/lifting.h"

/*
 * The filter looks eight taps back, so a group's first eight taps
 * produce no output and the stream's last eight are flushed out by
 * replaying stored pairs.
 */
#define MQA_RECON2_LATENCY 8

/* The kernel's history, per channel: every delay line runs newest first. */
struct mqa_recon2_channel {
	int32_t x[4];        /* carrier samples                            */
	int32_t u[13];       /* the first FIR's outputs                    */
	int32_t y[3];        /* second output samples                      */
	int32_t z[3];        /* the first output's residue                 */
	int32_t p[8];        /* residuals                                  */
};

struct mqa_recon2_state {
	struct mqa_recon2_channel ch[2];   /* 0 = left, 1 = right          */
	int32_t sign;                      /* +1/-1, flipped before a tap  */
	unsigned shape;                    /* which triple of m is in force */
};

/* The four m triples a parameter record can select. */
#define MQA_RECON2_SHAPES 4
extern const int32_t mqa_recon2_shape_table[MQA_RECON2_SHAPES][3];

/* Clear the history and select a triple (values past the table are the first). */
void mqa_recon2_init(struct mqa_recon2_state *st, unsigned shape);

/*
 * Push `n` samples through the two FIR stages without producing output:
 * the filter's warm-up. The stream start replays stored carrier pairs
 * this way (with p and q NULL, which also scales each sample by 3/2, the
 * inverse of the gain the pair ring stores them with), and every group
 * begins with a few samples the output skips.
 *
 * The sign is not advanced: only taps that produce output flip it.
 */
void mqa_recon2_prime(struct mqa_recon2_state *st,
		      const int32_t *a, const int32_t *b,
		      const int32_t *p, const int32_t *q, unsigned n);

/*
 * Reconstruct `n` taps. a/b are the carrier samples, p/q the residuals;
 * l and r hold 2n dither samples on entry and the output pair per tap on
 * return, as in reconstruct.h.
 */
void mqa_recon2_group(struct mqa_recon2_state *st, const struct mqa_gain_params *gp,
		      const int32_t *a, const int32_t *b,
		      const int32_t *p, const int32_t *q,
		      int32_t *l, int32_t *r, unsigned n);

#endif
