/*
 * The analysis against the filter it inverts.
 *
 * A target at the output rate is analysed into a carrier and a split,
 * the carrier and residual are rounded to integers as a file would hold
 * them, and the library's own reconstruction filter is run over them.
 * What comes back has to be the target again, to within the quantiser's
 * step, and the carrier has to look like a carrier: the target's lower
 * half and nothing of its upper.
 */
#include <math.h>
#include "mqae/analyse.h"
#include "mqa/reconstruct.h"
#include "mqa/residual_stage.h"
#include "util.h"

#define TAPS 4096
#define WARM MQAE_ANALYSE_WARMUP
#define SETTLE 32           /* taps the filter takes to forget its empty start */

static int32_t target[2 * (TAPS + WARM)];
static double carrier[TAPS + WARM], split[TAPS + WARM];
static int32_t a[TAPS], p[TAPS], got[2 * TAPS];

/* A signal with content above the carrier's Nyquist, which is the whole
 * point: without it the residual would be nothing and any filter would
 * pass the test. */
static void make_target(double amp)
{
	unsigned i;

	for (i = 0; i < 2 * (TAPS + WARM); i++) {
		double t = i;
		double v = 0.55 * sin(0.013 * t) + 0.25 * sin(0.41 * t + 0.7)
			 + 0.20 * sin(2.31 * t + 1.9);        /* well above half rate */

		target[i] = (int32_t)(amp * v);
	}
}

/* The amplitude of one frequency in a signal, by direct summation. */
static double tone(const int32_t *x, unsigned n, double w)
{
	double re = 0, im = 0;
	unsigned i;

	for (i = SETTLE; i < n; i++) {
		re += x[i] * cos(w * i);
		im += x[i] * sin(w * i);
	}
	return 2 * sqrt(re * re + im * im) / (n - SETTLE);
}

static double run(double amp, int report)
{
	struct mqae_analysis an;
	struct mqa_recon_state st;
	struct mqa_recon_coeffs k;
	struct mqa_gain_params gp;
	double err = 0, sig = 0;
	unsigned i;
	int sign = 1;

	make_target(amp);
	mqae_analysis_init(&an);
	mqae_analyse(&an, target, TAPS, WARM, carrier, split);
	for (i = 0; i < TAPS; i++) {
		a[i] = (int32_t)(carrier[i] < 0 ? carrier[i] - 0.5 : carrier[i] + 0.5);
		sign = -sign;
		p[i] = (int32_t)(sign * (split[i] - (double)(a[i] >> 1)) +
				 (split[i] >= 0 ? 0.5 : -0.5));
	}

	memset(&gp, 0, sizeof gp);
	mqa_gain_from_scale(&gp, mqa_residual_scale_table[25]);
	mqa_recon_coeffs_default(&k);
	mqa_recon_init(&st);
	for (i = 0; i < TAPS; i++) {
		int32_t carrier_pair[2] = { a[i], a[i] }, residual[2] = { p[i], p[i] };
		int32_t dl[2] = { 0, 0 }, dr[2] = { 0, 0 };

		mqa_recon_tap(&st, &gp, &k, carrier_pair, residual, dl, dr);
		got[2 * i] = dl[0];
		got[2 * i + 1] = dl[1];
	}
	/*
	 * The filter starts from silence, so the first output pairs are
	 * whatever its empty history makes them -- no carrier can produce
	 * them. The transient is gone within a few taps; the file's own
	 * beginning is the encoder's problem, not the analysis's.
	 */
	for (i = 2 * SETTLE; i < 2 * TAPS; i++) {
		double e = (double)got[i] - target[i];

		err += e * e;
		sig += (double)target[i] * target[i];
	}
	err = sqrt(err / (2 * (TAPS - SETTLE)));
	sig = sqrt(sig / (2 * (TAPS - SETTLE)));
	if (report)
		printf("  amplitude %8.0f: rms error %7.1f (%.1f dB SNR)\n",
		       amp, err, 20 * log10(sig / (err > 0 ? err : 1e-9)));
	return err;
}

int main(void)
{
	double step = mqa_residual_scale_table[25];
	double small = run(10000, 1);
	double mid = run(1000000, 1);
	double big = run(4000000, 1);
	unsigned i;

	printf("  the filter's own step is %.0f\n", step);
	CHECK_EQ("a small signal reconstructs to the step", (int)(small < step), 1);
	CHECK_EQ("a mid signal reconstructs to the step", (int)(mid < step), 1);
	CHECK_EQ("a loud signal reconstructs to the step", (int)(big < step), 1);

	/*
	 * The carrier has to be the signal's lower half and nothing else --
	 * a player that never unfolds it hears this, so what is above the
	 * carrier's own Nyquist must not fold down into it. The target's
	 * three tones are at 0.013, 0.41 and 2.31 radians a sample; at half
	 * the rate the first two sit at 0.026 and 0.82, and the third would
	 * alias to 1.66 if the split were leaking.
	 */
	{
		double low = tone(a, TAPS, 0.026), mid = tone(a, TAPS, 0.82);
		double alias = tone(a, TAPS, 4.62 - 2 * 3.14159265358979);

		printf("  carrier tones: %.0f at 0.026, %.0f at 0.82, %.0f where the top would alias\n",
		       low, mid, alias);
		CHECK_EQ("the carrier carries the low tone", (int)(low > 100000), 1);
		CHECK_EQ("the carrier carries the middle tone", (int)(mid > 100000), 1);
		CHECK_EQ("nothing of the top folded into it", (int)(alias < 0.02 * mid), 1);
	}
	printf("encoder analysis: %s\n", test_fails ? "FAILED" : "ok");
	return test_fails != 0;
}
