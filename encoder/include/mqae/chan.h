/*
 * mqae/chan.h -- the data channel: framed messages, hidden in the
 * carrier's low bytes.
 *
 * Every carrier sample gives its low byte to a byte stream (two bytes
 * per frame, left then right) scrambled by XOR with an LCG keystream.
 * Descrambled, it is a sequence of messages: a byte whose low nibble is
 * the type and high nibble a check, a type-dependent header, and a
 * payload. Section 5 of docs/mqa-stage1-spec.md has the detail;
 * mqa/stream.h is the reader this has to satisfy.
 *
 * The check is a 2-bit CRC register seeded with the message's byte
 * offset in the channel, so a message cannot be moved once written; the
 * writer keeps the offset as it goes. A stream must open with a
 * parameter record, and every byte after it belongs to a message, so
 * where there is nothing to say the channel idles with one-byte type-0
 * messages.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQAE_CHAN_H
#define MQAE_CHAN_H

#include <stddef.h>
#include <stdint.h>

#include "mqa/descrambler.h"

struct mqae_chan {
	uint8_t *data;          /* the channel, in stream order           */
	size_t cap, len;
	int failed;             /* out of memory                          */

	/* the scrambler, which runs over the bytes as they are handed out:
	 * everything below `scrambled` is on the wire, above it plaintext */
	struct mqa_descrambler ks;
	size_t scrambled;
};

int mqae_chan_init(struct mqae_chan *c);
void mqae_chan_free(struct mqae_chan *c);

/*
 * The opening parameter record. `scale_index` is the stream's residual
 * scale, which the record repeats in all three of the id's 6-bit fields.
 * A decoder compares them against the datasync's and falls back to a
 * token gain if they disagree. `seeds` is part A, the predictors' two
 * 16-bit symbol memories per channel (8 bytes), and `state` part B, the
 * reconstruction filter's history (32 bytes); either may be NULL for a
 * stream that starts from silence.
 */
void mqae_chan_record(struct mqae_chan *c, unsigned scale_index,
		      const uint8_t seeds[8], const uint8_t state[32]);

/* One message of `type` carrying `size` payload bytes (0..255): type 5
 * is symbol data for the residual decoders, type 2 the refinement's. */
void mqae_chan_message(struct mqae_chan *c, unsigned type, const uint8_t *payload, unsigned size);

/*
 * A sync message: a restart point for a decoder joining mid-stream.
 * `kind` 2 restarts the refinement at the auxiliary ring's current fill,
 * 5 the residual stage at the symbol ring's. With `crc` non-NULL it also
 * carries a CRC-32 of the output so far, which a decoder checks.
 */
void mqae_chan_sync(struct mqae_chan *c, unsigned kind, unsigned scale_index, const uint32_t *crc);

/* Idle for `n` bytes (n one-byte type-0 messages). */
void mqae_chan_idle(struct mqae_chan *c, size_t n);

/* Idle until the channel is `len` bytes long. */
void mqae_chan_pad_to(struct mqae_chan *c, size_t len);

/*
 * The channel as the carrier carries it: byte 2i in the left sample of
 * frame i and 2i+1 in the right, counting from the frame the stream
 * opens at. Scrambles up to `upto` bytes in place (in pairs, so an odd
 * request stops one short) and returns the buffer. Bytes already
 * scrambled are left alone, so a writer can keep appending.
 */
const uint8_t *mqae_chan_wire(struct mqae_chan *c, size_t upto);

/* The check nibble of a message at `offset` with these bytes after its
 * first: exposed for tests and for writers building messages by hand. */
unsigned mqae_chan_check(size_t offset, const uint8_t *after, size_t n);

#endif
