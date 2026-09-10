/*
 * mqa/residual_stage.h -- the residual stage: the 32 P and 32 Q values a
 * group's reconstruction consumes, produced by the two channels' residual
 * decoders (residuals.h) under one controller.
 *
 * The controller owns the range coder that drains the symbol ring and
 * does, per group: distribute a newly announced gain record at the next
 * block boundary; at every 4096-sample block boundary restart the coder
 * and read the block header through it, re-seeding every random source
 * from the block index; set the entropy decoders' level bounds from the
 * carrier class; and run the decode, zeroing the outputs while the stage
 * is inactive. It reports a status per group.
 *
 * Section 7 of docs/mqa-stage1-spec.md specifies all of it: the shape
 * (7.1), the coder (7.2), blocks and gain records (7.3), the level
 * bounds (7.4), the carrier-digit FIFO (7.8), resynchronisation (7.9)
 * and the status codes (7.10).
 *
 * The digit FIFO is for carrier classes that keep part of their residual
 * data in the carrier's own digits: the digits are packed several to a
 * byte, and the leading records of each channel read them through a
 * second range coder whose base is the class's radix. The FIFO holds at
 * most `size - 1` bytes and drops the oldest on overrun, which is what
 * keeps its read position a fixed depth behind the carrier.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_RESIDUAL_STAGE_H
#define MQA_DECODE_RESIDUAL_STAGE_H

#include <stdint.h>
#include "mqa/carrier.h"
#include "mqa/residuals.h"

#define MQA_RESIDUAL_RESYNC       4
#define MQA_RESIDUAL_WINDOW       0xe
#define MQA_RESIDUAL_RESET        0x11
#define MQA_RESIDUAL_ABORT        0x12
#define MQA_RESIDUAL_UNSUPPORTED  (-1)

#define MQA_RESIDUAL_GROUP        32     /* carrier samples per group        */
#define MQA_RESIDUAL_BLOCK        4096   /* samples per coder restart        */

extern const uint16_t mqa_residual_scale_table[64];
extern const uint16_t mqa_residual_level_table[4];
extern const uint16_t mqa_residual_rate_table[4];

struct mqa_residual_stage {
	struct mqa_range_decoder coder;          /* shared by all four decoders  */
	struct mqa_byte_ring *ring;              /* the symbol ring              */
	struct mqa_residual_decoder ch[2];

	const struct mqa_carrier_class *cls;
	struct mqa_gain_params record;           /* the announced gain record    */
	unsigned record_index;                   /* which scale it is for        */
	uint8_t index[2];                        /* stream id: scale indices     */
	int fresh;                               /* record awaits distribution   */
	int32_t seek;                            /* ring position to adopt, -1   */
	int32_t spread;                          /* widens the level bounds      */
	unsigned header_bits;                    /* last block header, low 2 bits*/

	int mode;                                /* 0 unconfigured, 1 configured, 2 running */
	int active;
	int block_seen;
	int reset_request, abort_request;
	uint32_t counter;                        /* samples processed            */

	/* resynchronisation: a packet's sync position arms it (limit, the
	 * mode byte in resync) when enabled; it takes effect at the next
	 * block boundary past the limit */
	int resync_enable;
	uint32_t limit;
	int32_t resync;

	/*
	 * The carrier-digit FIFO: classes with more than one digit level
	 * carry part of the residual data in the carrier's own digits, and
	 * the first `param[0]` records of each channel are decoded from
	 * them. The digits are packed `param[1]` to a byte in base
	 * `levels`, which makes each byte a symbol of radix `param[2] + 1`,
	 * and the FIFO is the source `digit_coder` reads. Its write and
	 * read positions are the stage's window.
	 *
	 * The FIFO holds at most `size - 1` bytes: a write into a full one
	 * drops the oldest, which is what keeps the read position a fixed
	 * depth behind the carrier once a stream is running. A stream is
	 * joined with a group of zero digits (the join sets the write
	 * position and the packing phase, not the contents); a
	 * realignment hands it the real ones.
	 */
	struct mqa_range_decoder digit_coder;
	struct mqa_byte_ring digit_ring;
	uint8_t digit_data[256];
	uint32_t digit_carry, digit_phase;       /* the part-packed byte, and
						  * the digits it still wants */
	/* the stream parameters as installed at the packet start */
	uint32_t scale_index, kernel_param, shift;
	int unsupported;              /* a path the stage cannot drive was met:
				       * every group from here is declined */
};

/* Gain record from a scale (the 2^(32+k) / 2 scale recipe). */
void mqa_gain_from_scale(struct mqa_gain_params *g, int32_t scale);

/* Prepare a zeroed stage: wires the decoders to the shared coder. */
void mqa_residual_stage_init(struct mqa_residual_stage *s, struct mqa_byte_ring *ring,
			     const struct mqa_carrier_class *cls);

/*
 * A parameter record or sync message arrived: adopt the stream id's scale
 * indices, the ring position to resume from (`start`, -1 for none) and,
 * the first time, the predictor seeds carried in the record's part A.
 */
void mqa_residual_stage_configure(struct mqa_residual_stage *s, unsigned index0,
				  unsigned index1, int32_t start,
				  const uint8_t *part_a, unsigned a_len);

/*
 * Produce residuals for `count` carrier samples (32 for a whole group,
 * fewer at a packet's end); returns 0 or a status code. `digits` are the
 * group's 2*count carrier digits, which multi-level classes decode from
 * (NULL, or anything, for single-level classes, which ignore them).
 */
int mqa_residual_stage_group(struct mqa_residual_stage *s, unsigned count, const uint8_t *digits,
			     int32_t p[MQA_RESIDUAL_GROUP], int32_t q[MQA_RESIDUAL_GROUP]);

void mqa_residual_stage_reset(struct mqa_residual_stage *s);

/*
 * Packet start: install the stream's class and parameters (the gain
 * record follows from the scale index), then either start the stage
 * (active, counter 0) or place it at a position with a limit and a
 * resynchronisation mode (inactive until it resynchronises). Both reset
 * the decoders to their initial adaptation state; `with_digits` says
 * whether the packet's leading carrier digits were supplied, in which
 * case `digits` are the 32 of them (they only matter for multi-level
 * classes).
 */
int mqa_residual_stage_setup(struct mqa_residual_stage *s, const struct mqa_carrier_class *cls,
			     unsigned scale_index, uint32_t kernel_param, uint32_t shift);
void mqa_residual_stage_start(struct mqa_residual_stage *s, int with_digits, const uint8_t *digits);
void mqa_residual_stage_position(struct mqa_residual_stage *s, uint32_t counter, uint32_t limit,
				 unsigned resync_mode, int with_digits, const uint8_t *digits);

/* A packet carrying a sync position: arm resynchronisation. */
void mqa_residual_stage_sync(struct mqa_residual_stage *s, uint32_t position, unsigned mode);

/* Building blocks, exposed for the tests. */
int mqa_residual_decoder_block_init(struct mqa_entropy_decoder *d, uint32_t block, int32_t rate);
int32_t mqa_residual_class_level(const struct mqa_carrier_class *cls, int32_t at);

#endif
