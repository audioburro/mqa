/*
 * Writing the control bitstream -- see mqae/bits.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include "mqae/bits.h"

int mqae_bits_init(struct mqae_bits *w)
{
	memset(w, 0, sizeof *w);
	w->cap = 4096;
	w->data = calloc(1, w->cap);
	return w->data ? 0 : -1;
}

void mqae_bits_free(struct mqae_bits *w)
{
	free(w->data);
	w->data = NULL;
	w->cap = 0;
}

void mqae_bits_seek(struct mqae_bits *w, uint64_t position)
{
	w->pos = position;
}

static int room(struct mqae_bits *w, uint64_t more)
{
	size_t need = (size_t)((w->nbits + more) / 8 + 1);
	uint8_t *grown;

	if (need <= w->cap)
		return 0;
	while (w->cap < need)
		w->cap *= 2;
	grown = realloc(w->data, w->cap);
	if (!grown) {
		w->failed = 1;
		return -1;
	}
	memset(grown + (w->nbits / 8 + 1), 0, w->cap - (w->nbits / 8 + 1));
	w->data = grown;
	return 0;
}

/* One bit into the stream, and through the packet's checksum register. */
static void bit(struct mqae_bits *w, unsigned b)
{
	uint32_t x;

	if (room(w, 1) < 0)
		return;
	if (b)
		w->data[w->nbits / 8] |= (uint8_t)(1u << (w->nbits % 8));
	else
		w->data[w->nbits / 8] &= (uint8_t)~(1u << (w->nbits % 8));
	w->nbits++;
	if (!w->open)
		return;
	x = w->csum & 1;
	w->csum = (w->csum >> 1) | ((uint32_t)b << 31);
	if (x)
		w->csum ^= 3;
}

void mqae_bits_put(struct mqae_bits *w, uint64_t value, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++)
		bit(w, (unsigned)((value >> i) & 1));
}

void mqae_bits_packet(struct mqae_bits *w, enum mqa_bs_type type)
{
	w->start = w->nbits;
	w->csum = (uint32_t)(w->pos & 15);
	w->open = 1;
	mqae_bits_put(w, (uint64_t)type, 4);
}

unsigned mqae_bits_in_packet(const struct mqae_bits *w)
{
	return w->open ? (unsigned)(w->nbits - w->start) : 0;
}

void mqae_bits_end(struct mqae_bits *w)
{
	unsigned pad, i;
	uint32_t expect;

	if (!w->open)
		return;
	/* the checksum covers the packet padded with zero bits to a word;
	 * the register is clocked on, without input, for the padding */
	pad = (unsigned)((0u - (uint32_t)(w->nbits - w->start)) & 31u);
	for (i = 0; i < pad; i++) {
		uint32_t x = w->csum & 1;

		w->csum >>= 1;
		if (x)
			w->csum ^= 3;
	}
	expect = w->csum & 15;
	w->open = 0;                          /* the field is not checksummed */
	mqae_bits_put(w, expect, 4);
	w->pos += w->nbits - w->start;
}

/* --- the packets ---------------------------------------------------------- */

static unsigned item_bits(const struct mqae_item *it, int with_position)
{
	switch (it->type) {
	case 0: return with_position ? 48u : 20u;
	case 1: return with_position ? 32u : 11u;
	case 2: return with_position ? 24u : 8u;
	case 3: return 96u;
	default: return 0u;
	}
}

static void put_item(struct mqae_bits *w, const struct mqae_item *it, int with_position)
{
	switch (it->type) {
	case 0:
		mqae_bits_put(w, it->u.base.stage2_dither, 2);
		mqae_bits_put(w, it->u.base.gain_index, 4);
		mqae_bits_put(w, it->u.base.level, 7);
		mqae_bits_put(w, it->u.base.lag, 7);
		if (with_position) {
			mqae_bits_put(w, it->u.base.start_pos, 27);
			mqae_bits_put(w, 0, 1);
		}
		break;
	case 1:
		mqae_bits_put(w, it->u.params.scale_index, 6);
		mqae_bits_put(w, it->u.params.carrier_class, 2);
		mqae_bits_put(w, it->u.params.variant, 1);
		mqae_bits_put(w, it->u.params.salt_select, 2);
		if (with_position) {
			mqae_bits_put(w, it->u.params.sync_mode, 8);
			mqae_bits_put(w, it->u.params.flag, 1);
			mqae_bits_put(w, (uint64_t)(uint32_t)it->u.params.offset & 0xfffu, 12);
		}
		break;
	case 2:
		mqae_bits_put(w, it->u.params2.scale_index, 6);
		mqae_bits_put(w, it->u.params2.salt_select, 2);
		if (with_position) {
			mqae_bits_put(w, it->u.params2.sync_mode, 3);
			mqae_bits_put(w, it->u.params2.flag, 1);
			mqae_bits_put(w, (uint64_t)(uint32_t)it->u.params2.offset & 0xfffu, 12);
		}
		break;
	case 3:
		mqae_bits_put(w, it->u.cipher.iv & 0xffffffffu, 32);
		mqae_bits_put(w, it->u.cipher.iv >> 32, 32);
		mqae_bits_put(w, it->u.cipher.start_pos, 32);
		break;
	default:
		break;
	}
}

void mqae_bits_datasync(struct mqae_bits *w, const struct mqae_datasync *d)
{
	unsigned i;

	mqae_bits_packet(w, MQA_BS_DATASYNC);
	mqae_bits_put(w, MQA_BS_MAGIC >> 4, 36);      /* the type is its low nibble */
	mqae_bits_put(w, d->with_position ? 1 : 0, 1);
	mqae_bits_put(w, 1, 1);                        /* skipped by decoders; real streams set it */
	mqae_bits_put(w, d->orig_rate, 5);
	mqae_bits_put(w, d->src_rate, 5);
	mqae_bits_put(w, d->render_filter, 5);
	mqae_bits_put(w, d->unknown_1, 2);
	mqae_bits_put(w, d->render_bitdepth, 2);
	mqae_bits_put(w, d->unknown_2, 4);
	mqae_bits_put(w, d->auth_info, 4);
	mqae_bits_put(w, d->auth_level, 4);
	mqae_bits_put(w, d->nitems, 7);
	for (i = 0; i < d->nitems; i++)
		mqae_bits_put(w, item_bits(&d->item[i], d->with_position), 8);
	for (i = 0; i < d->nitems; i++)
		mqae_bits_put(w, d->item[i].type, 8);
	if (d->with_position)
		mqae_bits_put(w, d->position, 32);
	for (i = 0; i < d->nitems; i++)
		put_item(w, &d->item[i], d->with_position);
	mqae_bits_end(w);
	/* a datasync that announces a position rebases the checksum seed */
	if (d->with_position)
		w->pos = d->position + (w->nbits - w->start);
}

void mqae_bits_hole(struct mqae_bits *w, unsigned size)
{
	mqae_bits_packet(w, MQA_BS_HOLE);
	if (size < 15) {
		mqae_bits_put(w, size, 4);
	} else {
		mqae_bits_put(w, 15, 4);
		mqae_bits_put(w, size, 12);
	}
	mqae_bits_put(w, 0, size);
	mqae_bits_end(w);
}

void mqae_bits_terminate(struct mqae_bits *w, uint32_t frames)
{
	mqae_bits_packet(w, MQA_BS_TERMINATE);
	mqae_bits_put(w, frames, 17);
	mqae_bits_end(w);
}

void mqae_bits_terminate_at(struct mqae_bits *w, uint64_t end)
{
	uint64_t after = w->nbits + MQAE_BITS_TERMINATE;

	mqae_bits_terminate(w, (uint32_t)(end > after ? end - after : 0));
}

void mqae_bits_authentication(struct mqae_bits *w, unsigned level, const uint8_t *data)
{
	unsigned i;

	mqae_bits_packet(w, MQA_BS_AUTHENTICATION);
	mqae_bits_put(w, level, 4);
	for (i = 0; i < MQAE_AUTH_BYTES; i++)
		mqae_bits_put(w, data ? data[i] : 0, 8);
	mqae_bits_end(w);
}

void mqae_bits_metadata(struct mqae_bits *w, unsigned type, int last,
			unsigned fragment, const uint8_t *data, unsigned size)
{
	unsigned i;

	if (size < 1)
		size = 1;
	if (size > 256)
		size = 256;
	mqae_bits_packet(w, MQA_BS_METADATA);
	mqae_bits_put(w, type, 7);
	mqae_bits_put(w, last ? 1 : 0, 1);
	mqae_bits_put(w, fragment, 12);
	mqae_bits_put(w, size - 1, 8);
	for (i = 0; i < size; i++)
		mqae_bits_put(w, data ? data[i] : 0, 8);
	mqae_bits_end(w);
}
