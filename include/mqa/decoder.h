/*
 * mqa/decoder.h -- the stage-1 decoder below the packet intake, driven
 * one 32-sample group at a time.
 *
 * The decoder owns the two carrier rings the intake fills (832 words
 * each, with a 64-word tail mirroring the head so any group can be read
 * contiguously), the descrambler and message parser that recover the
 * data channel ahead of reconstruction, and the three stages: carrier
 * refinement (refine.h), the residual stage (residual_stage.h) and the
 * output stage (output_stage.h).
 *
 * Per group it descrambles and parses the data channel up to 480 samples
 * ahead of the reconstruction position, extracts the carrier digits of
 * the window half a group ahead, refines the group's samples in place,
 * decodes its residuals, reconstructs 64 output samples per channel and
 * advances. Section 3 of docs/mqa-stage1-spec.md specifies the order and
 * the offsets, which matter: the stages are adaptive and a different
 * read-ahead gives different output.
 *
 * What the intake hands over per group is struct mqa_packet: the fields
 * of the packet record that this path reads. Paths no real stream
 * took (the two-pass end-of-packet group, the sync-message output
 * realignment, the record's filter-state transfer) return
 * MQA_DECODER_UNSUPPORTED rather than guess.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_DECODER_H
#define MQA_DECODE_DECODER_H

#include <stdint.h>
#include "mqa/descrambler.h"
#include "mqa/stream.h"
#include "mqa/refine.h"
#include "mqa/residual_stage.h"
#include "mqa/output_stage.h"

#define MQA_DECODER_UNSUPPORTED  (-1)
#define MQA_RING_WORDS           832
#define MQA_RING_TAIL            64
#define MQA_GROUP                32
#define MQA_DESCRAMBLE_AHEAD     0x1e0

/* What the packet intake hands the decoder for a group. */
struct mqa_packet {
	uint32_t count;          /* 0x00 carrier samples the packet still holds */
	uint8_t present;         /* 0x04 a packet is present at all */
	uint8_t flag5;           /* 0x05 stream continues at the packet start */
	uint32_t format;         /* 0x0c output format code (descriptor field) */
	uint32_t format_field;   /* 0x10 the output stage's format field */
	uint16_t descriptor;     /* 0x14 descriptor bit fields */
	uint8_t variant;         /* 0x19 stream variant (1: this filter) */
	uint32_t salt_select;    /* 0x1c dither salt selector */
	uint32_t length;         /* 0x20 packet length in samples, ~0 = open */
	uint8_t decoding;        /* 0x24 the packet carries a decodable stream */
	uint8_t alt_mode;        /* 0x25 the mode when it does not */
	uint8_t cls;             /* 0x26 carrier digit class */
	uint32_t shift;          /* 0x28 carrier digit shift */
	uint32_t level;          /* 0x2c level word (refinement gain) */
	uint8_t scale_index;     /* 0x30 scale index */
	uint8_t fresh;           /* 0x31 packet-start flag */
	int32_t sync;            /* 0x34 sync position, -1 for none */
	uint8_t sync_mode;       /* 0x38 goes with it */
	uint8_t has_consumed;    /* 0x39 the consumed count below is valid */
	uint32_t consumed_hi;    /* 0x3c consumed = 2 * consumed_hi + consumed_lo */
	uint8_t consumed_lo;     /* 0x40 */
	const uint8_t *prefix;   /* the packet's first 64 data-channel bytes */
};

struct mqa_decoder {
	/* carrier history rings (A = left, B = right) */
	int32_t ring_a[MQA_RING_WORDS + MQA_RING_TAIL];
	int32_t ring_b[MQA_RING_WORDS + MQA_RING_TAIL];
	unsigned pos;                 /* next group's start in the rings */
	unsigned fill;                /* where the intake writes next */
	unsigned stream_pos;          /* samples consumed from the stream */

	/* the data channel */
	struct mqa_descrambler descrambler;
	unsigned descramble_pos;      /* ring index descrambled up to */
	struct mqa_stream_parser parser;   /* its `started` is the stream's */
	struct mqa_byte_ring sym_ring, aux_ring;
	uint8_t sym_data[2048], aux_data[1024];

	/* stream parameters the packet-start handler installs and records
	 * are checked against */
	struct {
		uint32_t kernel_param;
		uint32_t format_field;
		uint16_t descriptor;
		uint8_t variant;
		uint32_t salt_select;
		uint32_t cls;             /* carrier class index */
		uint32_t scale_index;
		uint32_t shift;           /* carrier digit shift */
	} params;

	/* the stages */
	struct mqa_refine refine;
	struct mqa_residual_stage residual;
	struct mqa_output_stage output;

	/* run state */
	int mode;                     /* 0 idle, 1 aligning, 2 decoding */
	int primed;                   /* first decoding group done */
	int resync_mode;
	unsigned flush_pos;           /* ring position to flush up to, or all ones */
	int notify_enable;
	int ended;                    /* the stream's end was announced */
	unsigned input_end;           /* ring position where the input ends (all ones: not yet) */
	unsigned passthrough_pending; /* samples of a packet-less group still to pass through */
	uint8_t flags;                /* the output descriptor's rate byte: the
				       * input's rate code in bits 1..5, which
				       * picks the kernel and shaping tables */
	int unsupported;              /* an unimplemented path was requested */
	const char *unsupported_why;  /* which one, for diagnostics */

	/* research hook: the hidden layer as decoded for each group, the
	 * carrier samples before (a0, b0) and after (a, b) refinement, and
	 * the residuals (p, q) the reconstruction combines, `count` of each;
	 * called before the output stage runs */
	void (*on_layer)(void *user, const int32_t *a0, const int32_t *b0,
			 const int32_t *a, const int32_t *b,
			 const int32_t *p, const int32_t *q, unsigned count);
	void *layer_user;

	/* output */
	unsigned output_ring_pos;     /* the output stage's ring position */
	int32_t *out_l, *out_r;       /* where the next group's samples go */
	unsigned out_count;
	/*
	 * Frames the decoder passed through rather than unfolded: a file
	 * that is not MQA, the run-in before a stream opens, and whatever
	 * follows one that has ended. An owner that has to keep output
	 * time (a media framework's filter, say) needs this, because a
	 * passed-through frame yields one output frame where an unfolded
	 * one yields two.
	 */
	unsigned long passed;
};

void mqa_decoder_init(struct mqa_decoder *d, const struct mqa_carrier_class *cls);

/*
 * The rate the decoder is opened for: it picks the kernel and shaping
 * tables and fills the output descriptor. Call it after init, before
 * the first group.
 */
void mqa_decoder_set_input_rate(struct mqa_decoder *d, unsigned rate_code);

/* Decode one group; returns 1 when output was produced, 0 when not, or
 * MQA_DECODER_UNSUPPORTED. */
int mqa_decoder_group(struct mqa_decoder *d, const struct mqa_packet *pkt);

#endif
