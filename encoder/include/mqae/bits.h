/*
 * mqae/bits.h -- writing the control bitstream.
 *
 * The inverse of mqa/bitstream.h: one bit per stereo frame, least
 * significant bit first, a sequence of packets each ending in a four-bit
 * checksum. Section 4 of docs/mqa-stage1-spec.md specifies the packets;
 * the round-trip test parses everything written here back with the
 * decoder's own scanner.
 *
 * The checksum is seeded with the packet's bit position in the stream,
 * which is also its frame position, so the writer keeps that count as it
 * goes. mqae_bits_seek() sets it when a datasync announces a position.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQAE_BITS_H
#define MQAE_BITS_H

#include <stddef.h>
#include <stdint.h>
#include "mqa/bitstream.h"

struct mqae_bits {
	uint8_t *data;          /* the channel's bits, LSB first in each byte */
	size_t cap;             /* bytes allocated                            */
	uint64_t nbits;         /* bits written                               */
	uint64_t pos;           /* the next packet's position, in bits        */

	/* the packet being written */
	uint64_t start;
	uint32_t csum;
	int open;
	int failed;             /* out of memory                              */
};

int mqae_bits_init(struct mqae_bits *w);
void mqae_bits_free(struct mqae_bits *w);

/* The position the next packet's checksum is seeded from. A stream that
 * opens at frame 0 needs no call; a datasync that announces a position
 * sets it here as well as in its own field. */
void mqae_bits_seek(struct mqae_bits *w, uint64_t position);

/* One packet: begin with its type, put its fields, end to close it. */
void mqae_bits_packet(struct mqae_bits *w, enum mqa_bs_type type);
void mqae_bits_put(struct mqae_bits *w, uint64_t value, unsigned n);
void mqae_bits_end(struct mqae_bits *w);

/* Bits written so far in the packet in progress (its type field
 * included), for a writer that has to declare a length before it. */
unsigned mqae_bits_in_packet(const struct mqae_bits *w);

/* --- the packets a stage-1 stream needs ---------------------------------- */

/* A datasync's items. `size` is filled in by the writer. */
struct mqae_item {
	unsigned type;
	union {
		struct {                     /* type 0: the base band       */
			unsigned stage2_dither;   /* the conditioner's mode      */
			unsigned gain_index;      /* 1/32-octave output gain     */
			unsigned level;           /* the refinement level        */
			unsigned lag;             /* 127: no feedback term       */
			uint32_t start_pos;       /* resync position / 32        */
		} base;
		struct {                     /* type 1: class and parameters */
			unsigned scale_index;     /* residual scale table index   */
			unsigned carrier_class;   /* 0..3 (mqa_carrier_classes)   */
			unsigned variant;         /* 1: short filter, 0: kernel B */
			unsigned salt_select;     /* dither salt: 0 silence, 1, 2 */
			unsigned sync_mode;
			unsigned flag;
			int32_t offset;           /* signed, 12 bits              */
		} params;
		struct {                     /* type 2: parameters, no class */
			unsigned scale_index;
			unsigned salt_select;
			unsigned sync_mode;
			unsigned flag;
			int32_t offset;
		} params2;
		struct {                     /* type 3: the cipher's nonce   */
			uint64_t iv;
			uint32_t start_pos;
		} cipher;
	} u;
};

#define MQAE_MAX_ITEMS 8

struct mqae_datasync {
	unsigned orig_rate;          /* rate code of the source material     */
	unsigned src_rate;           /* rate code of the carrier             */
	unsigned render_filter;      /* what the renderer downstream applies */
	unsigned render_bitdepth;
	unsigned auth_info;
	unsigned auth_level;         /* 9 on studio-authenticated material   */
	unsigned unknown_1, unknown_2;

	int with_position;           /* every datasync but the first         */
	uint32_t position;           /* frames since the stream opened       */

	unsigned nitems;
	struct mqae_item item[MQAE_MAX_ITEMS];
};

void mqae_bits_datasync(struct mqae_bits *w, const struct mqae_datasync *d);

/* A hole: `size` bits of padding, which is how a stream idles between
 * packets. Sizes of 15 and above take the long form. */
void mqae_bits_hole(struct mqae_bits *w, unsigned size);

/*
 * The stream ends `frames` frames after this packet's last bit, so the
 * packet's own length matters to the caller. A terminate packet is
 * always MQAE_BITS_TERMINATE bits long.
 */
#define MQAE_BITS_TERMINATE 25
void mqae_bits_terminate(struct mqae_bits *w, uint32_t frames);

/* The same, said the way a writer means it: the stream ends at frame
 * `end`, counted from where it opened. */
void mqae_bits_terminate_at(struct mqae_bits *w, uint64_t end);

/*
 * An authentication packet: `level`, then 384 bytes, the size of a
 * 3072-bit signature. `data` may be NULL, which writes
 * zeros; see mqae/auth.h for supplying real ones.
 */
#define MQAE_AUTH_BYTES 384
void mqae_bits_authentication(struct mqae_bits *w, unsigned level, const uint8_t *data);

/* One fragment of metadata. `size` is 1..256 bytes. */
void mqae_bits_metadata(struct mqae_bits *w, unsigned type, int last,
			unsigned fragment, const uint8_t *data, unsigned size);

/* --- reading the result --------------------------------------------------- */

/* Bit `i` of the stream, for a carrier writer to embed. */
static inline unsigned mqae_bits_at(const struct mqae_bits *w, uint64_t i)
{
	return i < w->nbits ? (w->data[i / 8] >> (i % 8)) & 1u : 0u;
}

#endif
