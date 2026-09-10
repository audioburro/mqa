/*
 * mqa/watermark.h -- the renderer signalling in the output's parity.
 *
 * The vendor decoder finishes its output by nudging each frame by at most
 * one step so that the parity of the sample pair (left XOR right, bit 0)
 * carries one bit of a short message for the renderer in a DAC: a fixed
 * identifier, the stream's rates, render filter and bit depth, and one
 * record per renderer profile, each with a CRC. Section 11 of
 * docs/mqa-stage1-spec.md specifies the message and the nudge table.
 *
 * Which frame is nudged, and which way, comes from a table indexed by a
 * shift register the decoder walks per frame, so the disturbance is
 * spread rather than periodic. To anything but an MQA renderer it is
 * dither at about -138 dBFS.
 *
 * The library leaves it off by default: it adds noise and the audio is
 * otherwise identical. It is on when the aim is to reproduce the
 * vendor's delivered files bit for bit.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_WATERMARK_H
#define MQA_DECODE_WATERMARK_H

#include <stdint.h>

struct mqa_watermark {
	uint32_t state;                   /* the frame walk's shift register  */
	uint32_t reg;                     /* message bits, most significant first */
	unsigned bits;                    /* bits left in reg                 */
	unsigned pos;                     /* bytes of the record already sent */
	unsigned index;                   /* where the cycle stands           */
	uint8_t rec[8];                   /* the record being sent            */
	uint32_t config;                  /* record 1: the stream's word      */
	uint16_t config2;                 /* record 2                         */
	uint32_t walk[256], crc[256];
	int ready;
};

/* Start (or restart) the signalling; the message opens with record 0. */
void mqa_watermark_init(struct mqa_watermark *w);

/*
 * The stream's parameters, as the datasync gives them: the render filter
 * the renderer is asked for, the 5-bit rate code of the original
 * recording, the render bit depth, and whether the stream authenticated.
 * Takes effect at the next record 1.
 */
void mqa_watermark_set_stream(struct mqa_watermark *w, unsigned render_filter,
			      unsigned orig_rate_code, unsigned render_bitdepth,
			      int authenticated);

/* Embed the next n frames' worth of message into the samples in place. */
void mqa_watermark_apply(struct mqa_watermark *w, int32_t *l, int32_t *r, unsigned n);

#endif
