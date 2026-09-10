/*
 * mqa/output_stage.h -- the output stage: one group of carrier samples
 * and residuals in, twice as many output samples per channel out.
 *
 * Per group of `count` carrier samples the stage
 *
 *   1. seeds the output buffers with dither: two draws per sample per
 *      channel from a pair of dither LCGs (lcg.h) scaled by the stage's
 *      gain record, the pair reseeded from a salt and the sample counter
 *      at every 2048-sample boundary (mqa_dither_source);
 *   2. runs the reconstruction filter (reconstruct.h) once per carrier
 *      sample with the residuals from the residual stage, turning each
 *      dither pair into the two output samples;
 *   3. clamps the outputs to 24 bits;
 *   4. accumulates the outputs into a CRC-32 register (the standard
 *      reflected polynomial 0xedb88320, clocked with four zero bytes
 *      then XORed with each sample), which the stream's sync messages
 *      can later be checked against; and
 *   5. advances the carrier-history ring position by the group.
 *
 * It also keeps a mirrored ring of carrier pairs (scaled by a gain the
 * stream's variant chooses) and a fractional-rate read pointer into a
 * fixed table. The ring warms whichever reconstruction filter a restart
 * installs: the short one (reconstruct.h) replays it a tap at a time,
 * the alternative kernel (recon2.h) primes with it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_OUTPUT_STAGE_H
#define MQA_DECODE_OUTPUT_STAGE_H

#include <stdint.h>
#include "mqa/resampler.h"
#include "mqa/lifting.h"
#include "mqa/reconstruct.h"
#include "mqa/recon2.h"

#define MQA_OUTPUT_UNSUPPORTED (-1)
#define MQA_OUTPUT_PAIR_RING   66     /* pairs in the carrier pair ring */
#define MQA_DITHER_RESEED      2048   /* samples per dither reseed      */

/* The stage's dither generator. */
struct mqa_dither_source {
	uint32_t lcg[2];        /* left, right                              */
	uint32_t salt;          /* reseed salt; 0 means silence             */
	uint32_t counter;       /* samples drawn so far                     */
};

/* Fill l[2n], r[2n] with dither for n samples, scaled by `scale`. */
void mqa_dither_fill(struct mqa_dither_source *d, uint32_t scale,
		     int32_t *l, int32_t *r, unsigned n);

/* The output descriptor: status bytes the reference keeps for its
 * reader; only the bit fields the stage touches are documented. */
struct mqa_output_descriptor {
	uint8_t bytes[16];      /* [0..8) current, [8..16) previous         */
};


struct mqa_output_stage {
	struct mqa_gain_params gain;
	struct mqa_recon_coeffs coeffs;
	struct mqa_recon_state recon;
	struct mqa_recon2_state recon2;   /* the alternative kernel's history */
	struct mqa_dither_source dither;  /* its counter counts the group's samples */
	uint8_t skip;                     /* carrier samples the output skips */
	uint8_t lookahead;                /* extra dither samples after group */
	uint8_t kernel;                   /* 0: this filter; else alternative */
	uint8_t variant;                  /* the stream's variant byte, as last applied */
	uint32_t format_field;            /* 5-bit field copied to the descriptor */

	/* output CRC */
	uint32_t crc_table[256];
	uint32_t crc, crc_expect;
	int crc_armed;
	int indicator, indicator_enable, indicator_hold;   /* indicator flags */
	int notify;                       /* set when the reference would notify */
	unsigned restarts;                /* restarts applied so far (for observers) */

	/* the carrier pair ring: the passthrough resampler's history, also
	 * read by the alternative kernel */
	int32_t pairs[2 * 2 * MQA_OUTPUT_PAIR_RING];   /* ring + mirror     */
	unsigned pair_wpos;               /* pair index of the last write     */
	int32_t pair_gain;
	const struct mqa_resampler_spec *spec;   /* the record in force      */
	int32_t rate_acc, rate_inc, rate_period, rate_step;   /* its phase   */
	unsigned rate_pos;                /* coefficient row (words) in spec->table */
	int rs_mode;                      /* 2: 1:1, 1: decimating, 3: interpolating, 0: rational */
	uint32_t rs_dither[2];            /* the resampler's dither generators */
	struct mqa_rate_status status;

	struct mqa_output_descriptor desc;
};

/*
 * Reconstruct one group. a/b: the group's refined carrier samples; p/q:
 * its residuals; l/r receive 2 * count samples each. `format` is the
 * stream's format code and `check_crc` asks for the sync-message CRC
 * comparison.
 *
 * `ending` says whether this is a stream's last group: 0 no, 1 the last
 * packet's short pass, 2 a group finished after the packets have run
 * out. Both non-zero forms draw the extra dither the alternative kernel
 * needs; form 2 also flushes that kernel, pushing the eight carrier
 * samples at tail_a/tail_b (the ones just past the group) through it so
 * that the output catches up with the input, which adds sixteen samples
 * per channel to the result. tail_a/tail_b may be NULL otherwise.
 *
 * Returns the number of output samples per channel, or a negative code.
 */
int mqa_output_stage_group(struct mqa_output_stage *s,
			   const int32_t *a, const int32_t *b, unsigned count,
			   const int32_t *p, const int32_t *q,
			   int32_t *l, int32_t *r,
			   unsigned format, int check_crc, int ending,
			   const int32_t *tail_a, const int32_t *tail_b,
			   unsigned *ring_pos);

/* Build the CRC table; the rest of the state is the caller's to fill. */
void mqa_output_stage_init(struct mqa_output_stage *s);
/*
 * The descriptor the decoder is opened with: the input's rate code in
 * byte 1 (bits 2..6) and byte 3 (bits 1..5), and byte 6's bit 4 marking
 * the output rate as fixed, so no resampling applies. Before it is
 * installed the reference holds a descriptor with every rate field
 * unset, which its first group pushes into the previous half.
 */
void mqa_output_stage_open(struct mqa_output_stage *s, unsigned rate_code);


/*
 * A parameter record restarts the stage. Both forms first apply the
 * stream parameters (gain from the scale, dither salt and reseed, the
 * kernel choice and a filter reset); form 0 then loads the filter's
 * prediction and correction history from the record's part B, form 1
 * (also used at a packet start) warms the filter instead by replaying
 * the 16 most recent carrier pairs from the pair ring, oldest first,
 * through the filter's linear half (mqa_recon_replay_tap). Form 1 skips
 * the parameter step when the position, scale and variant are unchanged.
 */
struct mqa_output_restart {
	uint8_t flavour;          /* record byte 0 (0: this form)             */
	uint8_t valid;            /* record byte 1 (0: only the pair gain)    */
	uint16_t descriptor;      /* bit fields copied into the descriptor    */
	uint8_t variant;          /* 1: the short filter; 0: recon2.h         */
	uint32_t salt_select;     /* 0 silence, 1 or other: two fixed salts   */
	int32_t scale;            /* the gain record's scale                  */
	uint32_t kernel_param;    /* selects the alternative kernel's table   */
	uint32_t format_field;
	uint32_t position;        /* used by flavour 1 only                   */
	uint8_t part_b[32];       /* the filter history                       */
};

/* Returns 0, or MQA_OUTPUT_UNSUPPORTED for unknown forms. */
int mqa_output_stage_restart(struct mqa_output_stage *s, const struct mqa_output_restart *r);

/* Building block, exposed for the tests. */
/*
 * Pass `count` carrier samples through (a stream that is over, or was
 * never MQA): the descriptor is refreshed, the output-rate status and
 * the resampler follow it (resampler.h), and the samples are copied out
 * when no resampling applies. Returns the samples written per channel,
 * or MQA_OUTPUT_UNSUPPORTED when they would have to be resampled.
 */
int mqa_output_stage_passthrough(struct mqa_output_stage *s,
				 const int32_t *a, const int32_t *b, unsigned count,
				 int32_t *l, int32_t *r, unsigned format, unsigned *ring_pos);

void mqa_output_crc_accumulate(struct mqa_output_stage *s, const int32_t *l, const int32_t *r,
			       unsigned n, int check);

#endif
