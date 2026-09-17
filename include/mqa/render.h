/*
 * mqa/render.h -- the second unfold: the renderer's upsampler and its
 * requantiser.
 *
 * An MQA renderer takes the first unfold's output (88.2 or 96 kHz) and
 * interpolates it by 2 or 4 with a short polyphase filter the stream
 * chose, one of sixteen per ratio (the datasync's render filter). It
 * carries no further data: the "unfold" is the filter. Samples that are
 * not part of a stream go through a generic interpolator instead, one
 * per ratio.
 *
 * Each output sample is the filter's 64-bit accumulation plus a dither
 * word, shifted down to 24 bits. The dither is the sum of two
 * generators stepped once per output frame: a 24-bit shift register
 * (the CRC-24 of crc24.h clocked with zeros) and the Numerical Recipes
 * LCG, whose high 24 bits are added to the left channel and subtracted
 * from the right. Both start at 1.
 *
 * A renderer then requantises a stream's output to the datasync's render
 * bit depth with a noise shaper: the quantisation error, less the
 * dither, is kept in a short history and filtered by a fixed row of
 * taps (five at 2x, eight at 4x, the last two of the eight applied to
 * the shaper's own past outputs). The dither here is a second pair of
 * the same generators, both seeded 0x2346. The step is 2^8 at 2x and
 * 2^10 at 4x: the datasync's render bit depth did not change it on any
 * stream tried (depth indices 1 and 2). This library leaves the
 * requantiser off unless asked, since it only costs precision.
 *
 * The parameters come from the stream, not from any signalling in the
 * samples (section 11 of the spec describes what a stand-alone
 * renderer would read; a decoder that renders does not need it).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_RENDER_H
#define MQA_DECODE_RENDER_H

#include <stddef.h>
#include <stdint.h>
#include "mqa/crc24.h"
#include "mqa/resampler.h"

#define MQA_RENDER_MAX_TAPS 16
#define MQA_RENDER_MAX_RATIO 4

struct mqa_render {
	unsigned ratio;                   /* 1 (copy), 2 or 4                  */
	const struct mqa_resampler_spec *spec;
	int32_t hist[2 * MQA_RENDER_MAX_TAPS]; /* pairs, newest first        */
	struct mqa_crc24 crc;
	uint32_t lfsr, lcg;               /* the dither generators             */
	int signalled;                    /* a stream's filter is in force     */
	unsigned filter;                  /* its render filter, 0..15          */

	/* the requantiser */
	int requantise;                   /* apply it to a stream's output     */
	unsigned shift;                   /* the output step is 2^shift        */
	const int32_t *shaping;           /* the shaper's taps, Q24            */
	unsigned taps, errors;            /* taps in all; taps over the errors */
	int32_t ring[16][2];              /* errors and shaper outputs         */
	unsigned head;
	uint32_t lfsr2, lcg2;
};

/* Start rendering at `ratio` times the input rate (2 or 4; 1 copies). */
void mqa_render_init(struct mqa_render *r, unsigned ratio);

/*
 * Which filter to use from here on: the stream's, by its render filter
 * index, or the generic one when no stream is in force. The history is
 * kept across a change; the requantiser restarts when a stream comes
 * into force.
 */
void mqa_render_set_stream(struct mqa_render *r, int signalled, unsigned filter);

/* Requantise a stream's output to its render bit depth (off by default). */
void mqa_render_set_requantise(struct mqa_render *r, int on);

/* Render `n` frames (24-bit samples); writes `n * ratio` frames. */
void mqa_render_run(struct mqa_render *r, const int32_t *l, const int32_t *rr, size_t n,
		    int32_t *ol, int32_t *orr);

#endif
