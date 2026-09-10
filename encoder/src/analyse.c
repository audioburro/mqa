/*
 * The reconstruction filter run backwards -- see mqae/analyse.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include "mqae/analyse.h"
#include "mqa/reconstruct.h"

void mqae_analysis_init(struct mqae_analysis *an)
{
	int i;

	memset(an, 0, sizeof *an);
	for (i = 0; i < 4; i++)
		an->c[i] = (double)(int32_t)mqa_recon_coeff_table[i] / 4294967296.0;
}

/*
 * One half of the split: the even output samples interpolate n, the odd
 * ones t, and each is its own independent recursion. The two values it
 * starts from are guesses (the output two taps on, which is most of
 * what the value is), and the caller's overhang covers the few taps it
 * takes for that to stop mattering.
 */
static void half(const int32_t *y, unsigned off, unsigned total, double c_a, double c_b,
		 double *x)
{
	double later[2];
	unsigned i;

	if (total < 3)
		return;
	later[0] = (double)y[2 * (total - 1) + off];
	later[1] = (double)y[2 * (total - 2) + off];
	for (i = total; i-- > 2; ) {
		double h = c_a * (later[0] - (double)y[2 * (i - 2) + off])
			 + c_b * (later[1] - (double)y[2 * (i - 1) + off]);

		x[i - 2] = (double)y[2 * i + off] - 2.0 * h;
		later[0] = later[1];
		later[1] = x[i - 2];
	}
}

void mqae_analyse(struct mqae_analysis *an, const int32_t *y, unsigned taps, unsigned warm,
		  double *carrier, double *split)
{
	unsigned total = taps + warm, i;
	double *n = malloc(total * sizeof *n), *t = malloc(total * sizeof *t);

	if (!n || !t) {
		free(n);
		free(t);
		return;
	}
	half(y, 0, total, an->c[0], an->c[1], n);
	half(y, 1, total, an->c[2], an->c[3], t);
	for (i = 0; i < taps; i++) {
		carrier[i] = n[i] + t[i];
		split[i] = t[i];
	}
	free(n);
	free(t);
}
