/*
 * mqae/encoder.h -- assembling a stream over a carrier.
 *
 * A stream is three things in one PCM file: the control channel
 * (bits.h), one bit per frame, opening with a datasync and idling with
 * holes; the data channel (chan.h), two bytes per frame, opening with a
 * parameter record and carrying the residual and refinement payloads;
 * and the carrier, whose low bits both displace (carrier.h).
 *
 * The caller pushes carrier frames. The encoder generates as much of
 * each channel as those frames carry, embeds it and hands the frames
 * back modified in place. What goes into the data channel is the
 * caller's: it appends messages to `chan` between calls, and the encoder
 * fills the gaps with idles so a stream with nothing to say still
 * parses.
 *
 * Frames are interleaved (left, right) 32-bit words with the 24-bit
 * sample in the top 24 bits.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQAE_ENCODER_H
#define MQAE_ENCODER_H

#include <stddef.h>
#include <stdint.h>
#include "mqae/bits.h"
#include "mqae/chan.h"
#include "mqae/auth.h"

struct mqae_config {
	unsigned src_rate;         /* the carrier's rate in Hz: 44100 or 48000 */
	unsigned orig_rate;        /* the rate the stream claims to encode     */
	unsigned xbit;             /* the control channel's bit, 0..7          */

	unsigned render_filter;    /* what a renderer downstream applies       */
	unsigned render_bitdepth;
	unsigned gain_index;       /* output gain, 16 steps of 1/32 octave     */
	unsigned stage2_dither;    /* the conditioner's mode                   */
	unsigned level;            /* the refinement level                     */

	unsigned scale_index;      /* residual scale, 0..63                    */
	unsigned carrier_class;    /* 0: the data channel carries the residuals */
	unsigned variant;          /* 1: the short filter, 0: the long kernel  */
	unsigned salt_select;      /* dither salt: 0 silence, 1 or 2           */

	struct mqae_auth auth;     /* provenance, and the signer if there is one */
};

/* Sensible defaults for a 24-bit carrier: 48 kHz claiming 96 kHz, the
 * short filter, the data channel carrying the residuals. */
void mqae_config_default(struct mqae_config *cfg);

struct mqae_encoder {
	struct mqae_config cfg;
	struct mqae_bits bits;
	struct mqae_chan chan;

	uint64_t frames;           /* carrier frames written                  */
	uint64_t total;            /* the stream's length, 0 if not known yet */
	int terminated;            /* the terminate packet is written         */
	int failed;
};

/*
 * Open a stream. `total` is the file's length in frames when it is known
 * (which lets the stream say where it ends), 0 when it is not.
 */
int mqae_encoder_open(struct mqae_encoder *e, const struct mqae_config *cfg, uint64_t total);
void mqae_encoder_close(struct mqae_encoder *e);

/*
 * Embed both channels into `n` frames of carrier, in place. The caller
 * has already appended whatever the data channel should say for them;
 * anything it did not fill idles.
 *
 * `placed` says the frames already carry the control bit and have had
 * room made for the data byte (mqae_carrier_upper), so that only the
 * byte itself is filled in. An encoder that has modelled what a decoder
 * will make of its carrier must say so: rounding the sample again, now
 * that the byte is known, would move bits the model has already
 * committed to and leave the decoder reconstructing from a carrier the
 * encoder never saw.
 */
void mqae_encoder_write(struct mqae_encoder *e, int32_t *lr, size_t n, int placed);

/*
 * Generate the control channel out to `frames`, so that a caller can
 * read the bits it will carry before it places the carrier. An encoder
 * that models what a decoder will see must do this: the control bit is
 * part of the sample, so it has to be known before the sample is
 * rounded, not patched in afterwards.
 */
void mqae_encoder_reserve(struct mqae_encoder *e, uint64_t frames);

/* Data-channel bytes still unwritten, how far ahead the caller may
 * append before the carrier catches up. */
static inline size_t mqae_encoder_chan_ahead(const struct mqae_encoder *e)
{
	return e->chan.len > 2 * e->frames ? e->chan.len - (size_t)(2 * e->frames) : 0;
}

#endif
