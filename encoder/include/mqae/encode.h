/*
 * mqae/encode.h -- the whole encoder: source at the higher rate in,
 * carrier out.
 *
 * A decoder takes a carrier's low byte for the data channel, conditions
 * what is left (mqa/conditioner.h), refines it from the auxiliary
 * channel (mqa/refine.h) and reconstructs two output samples per carrier
 * sample from it and a residual (mqa/reconstruct.h). The encoder works
 * back along that chain from what the decoder will actually see: it
 * keeps a model of the decoder's front end (the library's own
 * conditioner and refinement, set up as a packet start sets them up) and
 * runs the carrier it is about to write through it before choosing the
 * residual.
 *
 * Per block of 4096 carrier samples:
 *
 *   1. analyse the source into the carrier it implies and the split
 *      (mqae/analyse.h);
 *   2. round the carrier into sample words, with the control bit placed
 *      and the low byte left for the data channel, taking whatever
 *      headroom the datasync's gain index allows so nothing clips;
 *   3. run the conditioner, and the refinement encoder (mqae/refine.h),
 *      which codes the correction that brings the carrier back to what
 *      the analysis wanted and leaves the carrier the decoder will see;
 *   4. choose the residual that puts the split back given that carrier,
 *      sharing the carrier's remaining error between the two halves;
 *   5. code the block (mqae/residual.h) and pace its bytes and the
 *      refinement's into the data channel over the block's own frames.
 *
 * tests/test_encode.c checks the model against a real decoder's own
 * report of its carrier, sample for sample, and measures what comes
 * back.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQAE_ENCODE_H
#define MQAE_ENCODE_H

#include <stddef.h>
#include <stdint.h>
#include "mqae/encoder.h"
#include "mqae/analyse.h"
#include "mqae/residual.h"
#include "mqa/conditioner.h"
#include "mqae/refine.h"
#include "mqa/reconstruct.h"

/* Carrier samples an encoding step covers: the coder's block. */
#define MQAE_ENCODE_BLOCK MQAE_RESIDUAL_BLOCK
/* Carrier samples of source the block's solve needs past its end. */
#define MQAE_ENCODE_AHEAD (2 * MQAE_RESIDUAL_WARM + MQAE_ANALYSE_WARMUP)

struct mqae_encode {
	struct mqae_encoder out;              /* the container            */
	struct mqae_analysis an;
	struct mqae_residual res;

	/* the model of what the decoder will make of the carrier */
	struct mqa_conditioner cond;
	struct mqa_bitring cond_bits;
	struct mqae_refine refine;
	int sign;                             /* the filter's tap sign    */
	/*
	 * What the model does to the carrier's level before the filter
	 * sees it: the conditioner's level gain and the refinement's
	 * scaling multiplied together. It comes to almost exactly two,
	 * which is the doubling the filter's split then undoes, so the
	 * carrier a file holds is the analysis's divided by this.
	 */
	double model_gain;

	/* source frames at twice the carrier rate, waiting to be encoded */
	int32_t *src;
	size_t src_len, src_cap;
	size_t src_real;                      /* of those, actual source
					       * rather than the silence the
					       * last block is padded with */
	uint64_t taps;                        /* carrier samples produced */
	int ended;

	/* what it cost */
	unsigned long clipped;
	size_t chan_peak;                     /* furthest the data channel
					       * ever ran ahead of the carrier */
	uint64_t bytes;                       /* residual data written     */
	uint64_t aux_bytes;                   /* refinement data written   */
	double residual_energy, coding_error; /* summed over the file      */
	/*
	 * How far the carrier the decoder will really see is from the one
	 * the analysis asked for: the low byte the data channel took, the
	 * conditioner's shaped dither in its place, and the refinement
	 * working from an auxiliary channel nothing writes yet. Half of it
	 * lands in each of the tap's two output samples, so this is the
	 * floor everything else sits on.
	 */
	double carrier_energy, carrier_error;
	/*
	 * The largest carrier sample the analysis asked for, against the
	 * 24 bits there are to hold it. A full-scale source makes a
	 * full-scale carrier, and anything above the source's Nyquist
	 * pushes it over. The datasync's gain index is the answer: the
	 * encoder attenuates and the decoder puts it back.
	 * mqae_encode_headroom() turns this into the index to use.
	 */
	double carrier_peak;
	unsigned long clipped_carrier;
	int measure_only;              /* just find the peak: no coding    */
	/*
	 * What the encoder expects the decoder to produce, measured
	 * against the source it was given: the filter run forwards over
	 * the carrier the model says the decoder will see and the
	 * residuals it will really decode. It is the encoder's own
	 * account of what it has done, and a decode of the file should
	 * agree with it.
	 */
	struct mqa_recon_state check;
	struct mqa_gain_params check_gain;
	double out_energy, out_error;
	uint64_t counted;
	int failed;

	/*
	 * Research hook: the carrier the model says the decoder will
	 * reconstruct from, per group. A decoder's own on_layer hook
	 * reports the same thing, and the two must agree exactly. If
	 * they do not, the encoder has been working back from a carrier
	 * that does not exist.
	 */
	void (*on_carrier)(void *user, const int32_t *a0, const int32_t *b0,
			   const int32_t *a, const int32_t *b, unsigned n);
	void *carrier_user;
};

/*
 * Open an encoder. `cfg` is the stream's parameters (mqae/encoder.h);
 * `total` is the carrier's length in frames when it is known, which is
 * half the source's.
 */
int mqae_encode_open(struct mqae_encode *e, const struct mqae_config *cfg, uint64_t total);

/*
 * The gain index a carrier of this peak needs so as not to clip, given
 * the index it was measured at. 0 when it already fits; 15 is as much
 * headroom as the format has (about 2.8 dB), and a peak past that
 * clips whatever is chosen.
 */
unsigned mqae_encode_headroom(double carrier_peak, unsigned measured_at);
void mqae_encode_close(struct mqae_encode *e);

/*
 * Push source frames (interleaved, at twice the carrier rate) and take
 * carrier frames out. `out` receives at most `max` carrier frames;
 * `*produced` says how many. Call with `frames` 0 and `end` set once the
 * source is over, until it produces nothing more.
 */
int mqae_encode_push(struct mqae_encode *e, const int32_t *src, size_t frames,
		     int end, int32_t *out, size_t max, size_t *produced);

#endif
