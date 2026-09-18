/*
 * mqa/intake.h -- from input samples to packet records: the front of the
 * decoder.
 *
 * The decoder proper (decoder.h) works a group at a time on samples that
 * are already in its carrier rings, guided by a record (struct
 * mqa_packet) that says where the stream stands. This module produces
 * those records from the samples themselves:
 *
 *   * it finds the control bitstream (bitstream.h) and follows it 480
 *     frames ahead of the group being decoded, turning packets into
 *     stream state: the opening datasync fixes the stream's parameters,
 *     a later one announces a resync, a terminate packet fixes the end;
 *   * for the group's own 32 frames it captures every sample's low byte
 *     into the record (the data channel the decoder will descramble) and
 *     clears it from the sample;
 *   * it runs the conditioner (conditioner.h) over the group, which
 *     refills the cleared byte with shaped dither and re-embeds the
 *     control bits, and checksums the result;
 *   * it keeps the stream position, the group's frame count and the
 *     flags the decoder reads. A stream joined at a resync datasync
 *     counts from the position the datasync announces (section 3.1).
 *
 * Sections 3, 4 and 12 of docs/mqa-stage1-spec.md cover the timing, the
 * packets and what is known of authentication. The authentication
 * hashing itself and metadata delivery are not implemented.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_INTAKE_H
#define MQA_DECODE_INTAKE_H

#include <stdint.h>
#include "mqa/bitstream.h"
#include "mqa/decoder.h"
#include "mqa/crc32.h"
#include "mqa/conditioner.h"

#define MQA_INTAKE_LOOKAHEAD 480      /* frames the bitstream runs ahead */
/*
 * The authentication runs over 64K-frame blocks; one that passes keeps
 * the stream authenticated for this long, and the decoder starts out
 * with a full window. (The library does not hash: it takes the blocks
 * as passing, which is what the reference's own checks do on authentic
 * material. The reference ends a stream whose block has no verified
 * packet to match, an unsigned stream at its first boundary; this
 * library plays on. Spec section 12.)
 */
#define MQA_INTAKE_AUTH_WINDOW  327680u
#define MQA_INTAKE_AUTH_BLOCK   0x10000u

struct mqa_intake {
	struct mqa_bitstream bs;          /* the control channel */

	/* the stream */
	int active;                       /* a stream is open */
	int indicator;                    /* authentication indicator */
	int indicator_enable;
	uint32_t stream_pos;              /* frames since the stream start */
	uint32_t end_pos;                 /* where it ends (all ones: unknown) */
	uint32_t until_end;               /* frames left before the end */
	int32_t pending_sync;             /* a sync position not yet reached (-1: none) */
	uint32_t pending_sync_at;         /* stream position when announced */
	int xbit;                         /* the channel bit, 0..7 above bit 8 */
	uint32_t crc;                     /* CRC-32 of the samples */
	uint32_t auth_state;
	uint32_t auth_pending;            /* blocks until the next authentication */
	uint32_t auth_byte;               /* the datasync's authentication fields */

	/* the record under construction */
	struct mqa_packet pkt;
	uint8_t prefix[2 * MQA_GROUP];    /* the group's low bytes */
	int just_started;                 /* the stream started this call */
	int aligned;
	int synced_once;                  /* a sync was announced before */
	int decode_disabled;              /* datasync said no decoding */
	int have_params;                  /* datasync items seen */
	int no_decode;                    /* decoding is off until a datasync */
	int ds_flag;                      /* low bit of the datasync's last field */
	uint32_t first_sync;              /* this group announced the first sync */
	int rate_changed;                 /* a datasync announced another rate */
	int joined;                       /* the stream was joined at a resync */
	uint32_t joined_at;               /* the position it was joined at */

	/* datasync fields, kept as the datasync gives them */
	uint32_t ds_word[16];             /* the item words */
	uint32_t render_bitdepth, render_filter, rate_field, orig_rate;
	uint32_t auth_level, auth_info;
	uint32_t level_record[4];         /* refinement level record */

	/* the sample conditioner and the bits it re-embeds: the
	 * reconstruction packets' payloads, appended as their frames are
	 * extracted */
	struct mqa_conditioner cond;
	struct mqa_bitring ring;
	uint32_t channel[MQA_BITRING_BITS / 32]; /* the channel's bits by stream position */
	uint64_t extracted;               /* frames extracted since the stream start */
#define MQA_INTAKE_SPANS 8
	struct { uint64_t next, end; } span[MQA_INTAKE_SPANS];
	unsigned nspans;
};

/* rate_hz: the input's sample rate; a stream at another rate is refused. */
void mqa_intake_init(struct mqa_intake *in, unsigned rate_hz);
unsigned mqa_intake_rate_code(unsigned hz);

/*
 * One group. `a`/`b` point at the group's slot in the carrier rings (the
 * decoder's position, plus 32 frames once primed), `available` says how
 * many frames the rings hold from there. The record for the decoder is
 * left in in->pkt. Returns 1 when the decoder should run its group with
 * it, 0 when nothing can be done yet (not enough frames for the
 * lookahead).
 */
int mqa_intake_group(struct mqa_intake *in, int32_t *a, int32_t *b, unsigned available);

#endif
