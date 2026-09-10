/*
 * mqae/analyse.h -- the reconstruction filter, run backwards.
 *
 * The decoder's filter (mqa/reconstruct.h) splits each carrier sample
 * into a prediction n and a correction t, with the residual deciding
 * where the split falls, and produces two output samples per tap whose
 * ideal values are
 *
 *     out0[i] = n[i-2] + 2 * h1[i],   h1 = c0*(n[i]   - out0[i-2])
 *                                        + c1*(n[i-1] - out0[i-1])
 *     out1[i] = t[i-2] + 2 * h2[i],   h2 = c2*(t[i]   - out1[i-2])
 *                                        + c3*(t[i-1] - out1[i-1])
 *
 * The decoder's replay path computes the same thing, which is how this
 * was checked. The two halves are independent: the even output samples
 * interpolate n and the odd ones t; the carrier is their sum and the
 * residual comes from their difference.
 *
 * Each equation gives n[i-2] from out0[i] and the two later values
 * n[i-1] and n[i]. That is a recursion backwards through the file with
 * no feedback. An error in its starting values decays by about a factor
 * of three per tap and is gone within twenty, so a file can be analysed
 * in chunks, each run a little past its end with the overhang discarded.
 *
 * The result is the carrier the encoder should write and the residual
 * that goes with it, both unquantised. The carrier then loses its low
 * byte to the data channel and the residual is only codeable to within
 * the record's scale; the rest of the encoder deals with that.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQAE_ANALYSE_H
#define MQAE_ANALYSE_H

#include <stdint.h>

/* Taps of overhang to run past the end of a chunk before keeping
 * anything: the recursion forgets where it started well inside this. */
#define MQAE_ANALYSE_WARMUP 32

struct mqae_analysis {
	double c[4];            /* the filter's coefficients, as fractions */
};

void mqae_analysis_init(struct mqae_analysis *an);

/*
 * Analyse one channel over a window of `taps + warm` taps: `y` is that
 * many output pairs, and `carrier` and `split` receive the first `taps`
 * values of the carrier a = n + t and of the correction t. The window
 * needs nothing from before it, every equation the recursion uses
 * lies inside it, so a caller walks a file by moving the window on by
 * `taps` and keeping `warm` taps of overhang each time.
 *
 * The residual follows from the carrier the encoder finally writes,
 * which is not this one: the split says t = sign * p + (a >> 1), so
 *
 *     p = sign * (t - (a >> 1))
 *
 * with whatever `a` the decoder will really see. Doing it that way
 * round is what closes the loop over everything the carrier loses on
 * its way into the file.
 */
void mqae_analyse(struct mqae_analysis *an, const int32_t *y, unsigned taps, unsigned warm,
		  double *carrier, double *split);

#endif
