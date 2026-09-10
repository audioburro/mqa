/*
 * mqa/bitstream.h -- the control bitstream: where it hides in the audio,
 * and its packets.
 *
 * The control channel is one bit per stereo frame, the exclusive-or of
 * one bit of the left sample with the same bit of the right, for one bit
 * in 8..15 of the 24-bit samples. Read least significant bit first it is
 * a sequence of packets: a 4-bit type, a payload, a 4-bit checksum. A
 * stream is recognised by the datasync packet that opens it, type 5
 * followed by the 36-bit magic 0x11319207d, so the 40-bit pattern
 * 0x11319207d5 marks a stream.
 *
 * Section 4 of docs/mqa-stage1-spec.md specifies the search, the
 * framing, the checksum and every packet type; 4.4 the datasync and its
 * items, with a worked example.
 *
 * The scanner finds the stream by trying every candidate bit, parses
 * packets as frames arrive and hands each one to a callback. It is what
 * `mqad scan` and `mqad info` use, and the front half of the decoder's
 * intake. Packet types and fields follow the reference decoder's own
 * names, from strings in the binary; the two datasync header fields
 * whose purpose is still unknown keep numbers instead.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_BITSTREAM_H
#define MQA_DECODE_BITSTREAM_H

#include <stdint.h>
#include <stddef.h>

#define MQA_BS_MAGIC          0x11319207d5ull      /* type 5 + 36-bit magic */
#define MQA_BS_MAGIC_BITS     40
/* the channel bit, numbered within the 24-bit sample; samples are handled
 * left-justified in 32-bit words, so it is word bit 16..23 */
#define MQA_BS_MIN_BIT        8
#define MQA_BS_MAX_BIT        15
#define MQA_BS_WORD_SHIFT     8

enum mqa_bs_type {
	MQA_BS_HOLE = 0,
	MQA_BS_RECONSTRUCTION = 1,
	MQA_BS_DATA = 2,
	MQA_BS_TERMINATE = 3,
	MQA_BS_AUTHENTICATION = 4,
	MQA_BS_DATASYNC = 5,
	MQA_BS_PACKET_6 = 6,
	MQA_BS_METADATA = 7,
	MQA_BS_KEY = 8,
	MQA_BS_NTYPES
};

/* A datasync's additional data items. */
#define MQA_BS_MAX_ITEMS 16
struct mqa_bs_item {
	unsigned type, size;                      /* size in bits              */
	union {
		struct {                          /* type 0: base band         */
			unsigned stage2_dither;   /* the conditioner's mode    */
			unsigned gain_index;      /* output gain, 1/32 octave  */
			unsigned level;           /* the refinement level      */
			unsigned lag;             /* feedback strength; 127: none */
			uint32_t start_pos;       /* only with stream_pos_flag */
		} base;
		struct {                          /* types 1 and 2: decoding parameters */
			unsigned scale_index;     /* residual scale, 0..63 (A.3)    */
			unsigned carrier_class;   /* 0..3, type 1 only (A.1)        */
			unsigned variant;         /* 1 short filter, 0 long kernel; type 1 only */
			unsigned salt_select;     /* dither salt: 0 silence, 1, 2   */
			/* with stream_pos_flag: how a decoder joins here (section 4.4) */
			unsigned sync_mode;       /* the digit FIFO depth to re-base to */
			unsigned consumed_lo;     /* low bit of the data channel's byte offset */
			int32_t offset;           /* its high part, relative to the sync frame */
		} low;
		struct {                          /* type 3: cipher            */
			uint64_t iv;
			uint32_t start_pos;
		} cipher;
	} u;
};

struct mqa_bs_datasync {
	unsigned stream_pos_flag;                 /* 0: the stream's first     */
	unsigned orig_rate, src_rate;             /* 5-bit rate codes          */
	unsigned render_filter, unknown_1, render_bitdepth, unknown_2;
	unsigned auth_info, auth_level;
	uint32_t stream_position;                 /* frames, when flagged      */
	unsigned item_count;
	unsigned items_at;                        /* bit offset of item 0 in the packet */
	struct mqa_bs_item item[MQA_BS_MAX_ITEMS];
};

#define MQA_BS_METADATA_MAX 256
struct mqa_bs_metadata {
	unsigned metadata_type, is_last, fragment_number, size;
	uint8_t data[MQA_BS_METADATA_MAX];
};

struct mqa_bs_packet {
	enum mqa_bs_type type;
	uint64_t offset;                          /* bit offset in the channel */
	unsigned bits;                            /* its length in bits        */
	unsigned checksum, checksum_ok;
	unsigned checksum_seed;                   /* the register's start: the packet's
						   * stream position, low four bits */
#define MQA_BS_RAW_WORDS 16
	uint32_t raw[MQA_BS_RAW_WORDS];           /* the packet's first 512 bits, LSB first */
	union {
		struct mqa_bs_datasync datasync;
		struct { uint32_t bits_to_end; } terminate;
		struct { unsigned size, unknown; } sized;   /* hole, reconstruction, data, packet 6, key */
		struct { unsigned auth_level; uint8_t data[384]; } auth;
		struct mqa_bs_metadata metadata;
	} u;
};

/* Sample-rate codes: 2-bit base rate index, 3-bit power-of-two scale. */
static inline unsigned mqa_bs_rate_hz(unsigned code)
{
	static const unsigned base[4] = { 44100, 48000, 64000, 0 };

	return base[code >> 3] << (code & 7);
}

struct mqa_bitstream;
typedef void (*mqa_bs_packet_fn)(void *user, const struct mqa_bs_packet *p);

struct mqa_bitstream {
	int xbit;                                 /* the channel's bit, or -1  */
	unsigned word_shift;                      /* bit 0 of the 24-bit sample within the word (8: left-justified) */
	uint64_t sync[8];                         /* candidate shift registers */
	uint64_t frames;                          /* frames seen               */
	uint64_t sync_frame;                      /* where the stream began    */

	uint8_t *buf;                             /* channel bits since sync,  */
	size_t cap;                               /* LSB first, byte packed    */
	uint64_t wpos, rpos;                      /* bit positions in buf      */
	uint64_t base;                            /* channel bit offset of buf */
	uint64_t abspos;                          /* next packet's position in the stream (bits) */
	int seeded;                               /* abspos known for this stream */
	int lost;                                 /* resynchronising           */

	mqa_bs_packet_fn on_packet;
	/* optional: a packet's type as soon as it is known, before its body */
	void (*on_header)(void *user, enum mqa_bs_type type, uint64_t offset);
	uint64_t header_at;                       /* offset of the last header reported */
	/* Optional: a reconstruction packet's payload span [first, end), as
	 * soon as its size is known (before the packet is complete). */
	void (*on_span)(void *user, uint64_t first, uint64_t end);
	uint64_t span_at;
	/* Optional: a datasync's item as soon as its bits are in, before the
	 * packet's later items, padding and checksum (the reference acts on
	 * each item then); the packet is reported again through the packet
	 * callback once complete. */
	void (*on_item)(void *user, const struct mqa_bs_packet *p, unsigned index);
	uint64_t item_at;                         /* offset+1 and index of the last item reported */
	unsigned item_index;
	void *user;
	unsigned long packets, errors;
};

/* xbit: the channel bit if known, -1 to search bits 8..15. Samples are
 * taken as left-justified 32-bit words unless word_shift is changed. */
int mqa_bitstream_init(struct mqa_bitstream *bs, int xbit, mqa_bs_packet_fn fn, void *user);
void mqa_bitstream_free(struct mqa_bitstream *bs);
/* Forget the stream and search again from the next frame. */
void mqa_bitstream_reset(struct mqa_bitstream *bs);

/* Feed n frames (separate left/right arrays of 24-bit samples in the
 * top 24 bits of int32, as the decoder sees them). Packets are reported
 * through the callback as they complete. Returns 1 once a stream has
 * been found. */
int mqa_bitstream_feed(struct mqa_bitstream *bs, const int32_t *l, const int32_t *r, size_t n);

/* Frames interleaved L R L R ... */
int mqa_bitstream_feed_interleaved(struct mqa_bitstream *bs, const int32_t *lr, size_t n);

/* Human-readable packet type. */
const char *mqa_bs_type_name(enum mqa_bs_type t);

#endif
