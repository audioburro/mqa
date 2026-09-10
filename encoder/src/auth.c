/*
 * Provenance signalling -- see mqae/auth.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqae/auth.h"

void mqae_auth_none(struct mqae_auth *a)
{
	memset(a, 0, sizeof *a);
	a->level = MQAE_AUTH_LEVEL_NONE;
}

int mqae_auth_packet(struct mqae_bits *w, const struct mqae_auth *a, uint64_t index,
		     const int32_t *l, const int32_t *r, size_t frames)
{
	uint8_t payload[MQAE_AUTH_BYTES];

	memset(payload, 0, sizeof payload);
	if (a->sign && a->sign(a->user, index, l, r, frames, payload) != 0)
		return 0;
	mqae_bits_authentication(w, a->level, payload);
	return 1;
}
