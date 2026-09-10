/*
 * The refinement encoder against the decoder's refinement.
 *
 * A carrier and the carrier the encoder wishes it had; the encoder
 * picks a correction for every sample and codes it; the library's own
 * refinement then reads those bytes over the same carrier. It has to
 * produce exactly what the encoder said, sample for sample, and what
 * it produces has to be closer to the target than the carrier was.
 */
#include <math.h>
#include "mqae/refine.h"
#include "mqa/residual_stage.h"
#include "mqa/carrier.h"
#include "util.h"

#define GROUPS 96
#define N      (GROUPS * 32)

static int32_t a[N], b[N], enc_a[N], enc_b[N];
static double want_a[N], want_b[N];
static uint8_t ring_data[4096];

int main(void)
{
	static struct mqae_refine enc;
	static struct mqa_refine dec;
	static struct mqa_byte_ring ring;
	int32_t step = mqa_residual_scale_table[25];
	/* the numbers a packet start hands the refinement for a 48 kHz
	 * stream on the short filter, carrier class 0 */
	int32_t level = (int32_t)(0x30000 - (15u << 8));
	int32_t gain = mqa_refine_gain_from_level(level + (int32_t)0xffff0000);
	const uint8_t *bytes;
	size_t len = 0;
	unsigned g, i;
	double before = 0, after = 0, energy = 0;
	int exact = 1;

	/*
	 * The refinement scales the carrier by sixteen times its gain
	 * before correcting it, so the target is on that scale. What it has
	 * to correct is the carrier's own error -- a low byte's worth,
	 * which is what the data channel took.
	 */
	{
		double scale = 16.0 * (double)(-gain) / 4294967296.0;

		for (i = 0; i < N; i++) {
			double t = i;
			double v = 3000000.0 * sin(0.01 * t) + 200000.0 * sin(0.31 * t + 1.0);
			double u = 0.8 * v + 90000.0;

			want_a[i] = scale * v;
			want_b[i] = scale * u;
			a[i] = (int32_t)(v + 130.0 * sin(2.9 * t));
			b[i] = (int32_t)(u + 130.0 * sin(1.7 * t + 2));
			before += (want_a[i] - scale * a[i]) * (want_a[i] - scale * a[i]);
		}
	}
	memcpy(enc_a, a, sizeof a);
	memcpy(enc_b, b, sizeof b);

	if (mqae_refine_init(&enc, -gain, 1, 1, 0x3895afe1u, step) < 0)
		return 1;
	mqae_refine_block_start(&enc);
	for (g = 0; g < GROUPS; g++)
		mqae_refine_group(&enc, enc_a + 32 * g, enc_b + 32 * g,
				  want_a + 32 * g, want_b + 32 * g, 32);
	if (enc.failed && enc.why)
		printf("  encoder: %s\n", enc.why);
	CHECK_EQ("the refinement coded the block", enc.failed, 0);
	bytes = mqae_refine_block_end(&enc, &len);
	CHECK_EQ("its backward pass held up", enc.failed, 0);
	printf("  %u samples in %u bytes (%.2f bits a sample), %lu clipped\n",
	       2 * N, (unsigned)len, len * 8.0 / (2 * N), enc.clipped);

	/* now the decoder's own refinement over the same carrier */
	memcpy(ring_data, bytes, len < sizeof ring_data ? len : sizeof ring_data);
	ring.data = ring_data;
	ring.size = sizeof ring_data;
	ring.cursor = 0;
	ring.wpos = (unsigned)len;
	memset(&dec, 0, sizeof dec);
	dec.ring = &ring;
	dec.ch[0].coef = dec.ch[1].coef = mqa_refine_coefs_default;
	dec.lcg[0] = mqa_nr_lcg_step(0);
	dec.lcg[1] = mqa_nr_lcg_step(dec.lcg[0]);
	mqa_refine_setup(&dec, -gain, 1, 1, 0x3895afe1u);
	mqa_refine_set_step(&dec, step);
	for (g = 0; g < GROUPS; g++)
		mqa_refine_group(&dec, a + 32 * g, b + 32 * g, 32);

	for (i = 0; i < N; i++) {
		if (exact && (a[i] != enc_a[i] || b[i] != enc_b[i])) {
			printf("  sample %u: decoded (%d, %d), encoder said (%d, %d)\n",
			       i, a[i], b[i], enc_a[i], enc_b[i]);
			exact = 0;
		}
		after += (want_a[i] - a[i]) * (want_a[i] - a[i]);
		energy += want_a[i] * want_a[i];
	}
	CHECK_EQ("the decoder refined it as the encoder said", exact, 1);
	CHECK_EQ("the bytes ran out exactly", (int)ring.cursor, (int)len);
	printf("  the carrier was %.1f dB from the target and the refinement made it %.1f dB\n",
	       10 * log10(energy / (before > 0 ? before : 1e-9)),
	       10 * log10(energy / (after > 0 ? after : 1e-9)));
	CHECK_EQ("the refinement improved the carrier", (int)(after < before / 4), 1);
	mqae_refine_free(&enc);
	printf("encoder refinement: %s\n", test_fails ? "FAILED" : "ok");
	return test_fails != 0;
}
