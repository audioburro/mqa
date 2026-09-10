/*
 * mqae/auth.h -- provenance signalling, and where a key would go.
 *
 * A stream claims its provenance in two places. Every datasync carries
 * an authentication level and an info nibble, from which a decoder
 * derives the indicator it shows: studio material gets the "blue light",
 * material whose provenance was asserted further down the chain a plainer
 * one. Separately, type-4 authentication packets carry 384 bytes each,
 * about every 65536 frames, and a decoder that can verify them keeps the
 * stream authenticated for 327680 frames after each one.
 *
 * What is in those 384 bytes was not recovered: the decoder hashes and
 * verifies them with material this project does not have. 384 bytes is
 * the size of a 3072-bit signature, but nothing here depends on that
 * reading.
 *
 * This header writes the packets and lets the caller supply their
 * contents. Given a signer it calls it once per block with the block's
 * index and carrier samples and puts the 384 bytes returned into the
 * stream verbatim. Without one the packets carry zeros and no real
 * decoder will authenticate the result.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQAE_AUTH_H
#define MQAE_AUTH_H

#include <stddef.h>
#include <stdint.h>
#include "mqae/bits.h"

/* Authentication levels seen in the wild. The decoder turns the pair
 * (info, level) into its indicator; 9 is what studio-authenticated
 * material carries. */
#define MQAE_AUTH_LEVEL_NONE    0
#define MQAE_AUTH_LEVEL_STUDIO  9

/* Frames per authentication block, and how long one keeps a stream
 * authenticated. Both are the decoder's own constants. */
#define MQAE_AUTH_BLOCK   65536u
#define MQAE_AUTH_WINDOW  327680u

/*
 * Called once per block. `index` counts blocks from the stream's start
 * and `l`/`r` are the block's carrier samples as they will be written
 * (24-bit values, left-justified in 32-bit words), so a signer can hash
 * whatever it needs to. Write MQAE_AUTH_BYTES bytes into `out` and
 * return 0; return non-zero to have the encoder leave the packet out.
 */
typedef int (*mqae_signer_fn)(void *user, uint64_t index,
			      const int32_t *l, const int32_t *r, size_t frames,
			      uint8_t out[MQAE_AUTH_BYTES]);

struct mqae_auth {
	unsigned level;          /* what the stream claims                  */
	unsigned info;           /* the datasync's other nibble             */
	mqae_signer_fn sign;     /* NULL: the packets carry zeros           */
	void *user;
};

/* Set up an unsigned stream: the packets are written, but empty. */
void mqae_auth_none(struct mqae_auth *a);

/*
 * Write one authentication packet for `index`, calling the signer if
 * there is one. Returns 1 if a packet was written, 0 if the signer
 * declined.
 */
int mqae_auth_packet(struct mqae_bits *w, const struct mqae_auth *a, uint64_t index,
		     const int32_t *l, const int32_t *r, size_t frames);

#endif
