/*
 * mqa/stream.h -- the framed message stream in the data channel.
 *
 * The bytes the descrambler recovers (descrambler.h) form messages: a
 * type-and-check byte, a type-dependent header, and for most types a
 * payload. This parser frames them, verifies each message's check nibble
 * and routes the payloads: type 5 to the entropy decoders' symbol ring,
 * type 2 to the refinement's auxiliary ring, type 4 (parameter records)
 * and type 3 (sync messages) to callbacks. Everything else is checked
 * and dropped; type 1 is the commonest of those.
 *
 * Sections 5.2 to 5.4 of docs/mqa-stage1-spec.md specify the framing,
 * the check register (a 2-bit CRC clocked a byte at a time and seeded
 * with the message's byte offset) and the routing.
 *
 * The parser keeps a 128-byte FIFO. The caller tops it up with
 * mqa_stream_push (64 bytes per group, in practice) and drains it with
 * mqa_stream_parse, which handles as many complete messages as it holds
 * and keeps partial state across calls. A failed check takes the rings
 * offline until a sync message with a matching stream id; a stream must
 * open at byte 0 with a parameter record.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_STREAM_H
#define MQA_DECODE_STREAM_H

#include <stdint.h>
#include "mqa/entropy.h"    /* struct mqa_byte_ring */

#define MQA_STREAM_FIFO       128
#define MQA_STREAM_RECORD_A     8   /* parameter record, first part (max) */
#define MQA_STREAM_RECORD_B    32   /* second part (max)                  */

/* A type-4 parameter record. */
struct mqa_stream_record {
	uint32_t id;               /* the 24-bit stream id from the header */
	uint8_t a[MQA_STREAM_RECORD_A];
	unsigned a_len;
	uint8_t b[MQA_STREAM_RECORD_B];
	unsigned b_len;
};

/* A type-3 sync message, as handed to the sync callback. */
struct mqa_stream_sync {
	unsigned pos;              /* the caller's position argument        */
	uint32_t id;               /* current stream id                     */
	unsigned kind;             /* header byte 1 bits 2-5 (2 or 5)       */
	int mode2;                 /* the decoder's mode field == 2         */
	int flag;                  /* header byte 1 bit 1                   */
	uint32_t word;             /* four further header bytes, if flag     */
};

struct mqa_stream_parser {
	uint8_t fifo[MQA_STREAM_FIFO];
	unsigned avail;            /* bytes waiting in the FIFO             */
	uint32_t consumed;         /* stream offset of fifo[0]              */

	int type;                  /* message being handled, -1 = none      */
	unsigned remaining;        /* payload bytes still to come           */
	unsigned rec_len_a;        /* type 4: length of the record's part A */
	unsigned nibble;           /* the message's check nibble            */
	uint32_t check;            /* running check register                */
	unsigned items;            /* messages completed                    */

	int started;               /* cleared when the stream fails to open  */
	                           /*  with a record; set by the decoder     */
	int rings_enabled;         /* payloads are being routed             */
	int check_pending;         /* alternative check finalisation (never  */
	uint32_t check_expect;     /*  observed armed; kept for fidelity)    */

	uint32_t stream_id;        /* 24-bit id from type 3/4 headers       */
	int mode2;                 /* decoder mode == 2 (reported to sync)  */
	uint8_t resync_mode;       /* passed to the failure callback        */

	struct mqa_byte_ring *ring_sym;   /* type-5 payloads */
	struct mqa_byte_ring *ring_aux;   /* type-2 payloads */

	void (*on_record)(void *user, const struct mqa_stream_record *rec);
	void (*on_sync)(void *user, const struct mqa_stream_sync *sync);
	void (*on_sync_mismatch)(void *user);
	void (*on_check_fail)(void *user, uint8_t resync_mode);
	void *user;
};

void mqa_stream_init(struct mqa_stream_parser *p,
		     struct mqa_byte_ring *ring_sym, struct mqa_byte_ring *ring_aux);

/* Append bytes to the FIFO; returns how many fitted. */
unsigned mqa_stream_push(struct mqa_stream_parser *p, const uint8_t *bytes, unsigned n);

/*
 * Parse whatever the FIFO holds. `pos` is passed through to the sync
 * callback; `sync_enabled` selects whether type-3 messages are acted on
 * (the decoder passes 1 once its output position is established).
 */
void mqa_stream_parse(struct mqa_stream_parser *p, unsigned pos, int sync_enabled);

/* One step of the message check (exposed for test encoders). */
uint32_t mqa_stream_check_update(uint32_t reg, uint8_t byte);

#endif
