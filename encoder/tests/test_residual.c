/*
 * The residual stage encoder against the decoder's stage.
 *
 * A block of residual targets is encoded; the bytes go into a symbol
 * ring; the library's own residual stage decodes them. What it produces
 * must be exactly what the encoder said it would, and close to what was
 * asked for -- the second is the interesting number, because the
 * predictor's two records are coded one after the other while it
 * consumes them in pairs, so the targets are computed from ideal values
 * and this is where that shows.
 */
#include <math.h>
#include "mqae/residual.h"
#include "mqa/residual_stage.h"
#include "util.h"

#define GROUPS (MQAE_RESIDUAL_BLOCK / MQAE_RESIDUAL_GROUP)

#define OVER (2 * MQAE_RESIDUAL_WARM)

static int32_t want_p[MQAE_RESIDUAL_BLOCK + OVER], want_q[MQAE_RESIDUAL_BLOCK + OVER];
static int32_t said_p[MQAE_RESIDUAL_BLOCK], said_q[MQAE_RESIDUAL_BLOCK];
static int32_t dec_p[MQA_RESIDUAL_GROUP], dec_q[MQA_RESIDUAL_GROUP];
static uint8_t ringbuf[65536];

int main(void)
{
	static struct mqae_residual enc;
	static struct mqa_residual_stage dec;
	static struct mqa_byte_ring ring;
	const struct mqa_carrier_class *cls = &mqa_carrier_classes[0];
	const uint8_t *bytes;
	size_t len;
	unsigned g, i;
	double err = 0, sig = 0;
	int exact = 1;

	for (i = 0; i < MQAE_RESIDUAL_BLOCK + OVER; i++) {
		double t = i;

		want_p[i] = (int32_t)(9000.0 * sin(0.017 * t) + 3000.0 * sin(0.7 * t));
		want_q[i] = (int32_t)(7000.0 * sin(0.011 * t + 2.0));
	}
	if (mqae_residual_init(&enc, 25, cls) < 0)
		return 1;
	bytes = mqae_residual_block(&enc, want_p, want_q, MQAE_RESIDUAL_BLOCK,
				    said_p, said_q, &len);
	if (enc.coder.failed && enc.coder.why)
		printf("  coder: %s\n", enc.coder.why);
	CHECK_EQ("the block came out", enc.coder.failed, 0);
	printf("  %u samples in %u bytes (%.2f bits a sample), %lu symbols clipped\n",
	       MQAE_RESIDUAL_BLOCK, (unsigned)len, len * 8.0 / MQAE_RESIDUAL_BLOCK, enc.clipped);

	/* now decode it exactly as a stream would */
	memcpy(ringbuf, bytes, len);
	ring.data = ringbuf;
	ring.size = sizeof ringbuf;
	ring.cursor = 0;
	ring.wpos = (unsigned)len;
	mqa_residual_stage_init(&dec, &ring, cls);
	mqa_residual_stage_setup(&dec, cls, 25, 0, 0);
	/* the order a stream does it in: the packet start puts the stage in
	 * place, and the parameter record that follows configures it */
	mqa_residual_stage_start(&dec, 0, NULL);
	mqa_residual_stage_configure(&dec, 25, 25, -1, NULL, 0);
	for (g = 0; g < GROUPS; g++) {
		int status = mqa_residual_stage_group(&dec, MQA_RESIDUAL_GROUP, NULL, dec_p, dec_q);

		if (status) {
			printf("  group %u: stage status %d\n", g, status);
			CHECK_EQ("the stage decoded the block", 0, 1);
			break;
		}
		for (i = 0; i < MQA_RESIDUAL_GROUP; i++) {
			unsigned k = g * MQA_RESIDUAL_GROUP + i;

			if (exact && (dec_p[i] != said_p[k] || dec_q[i] != said_q[k])) {
				printf("  sample %u: decoded (%d, %d), encoder said (%d, %d)\n",
				       k, dec_p[i], dec_q[i], said_p[k], said_q[k]);
				exact = 0;
			}
			err += (double)(dec_p[i] - want_p[k]) * (dec_p[i] - want_p[k]);
			sig += (double)want_p[k] * want_p[k];
		}
	}
	CHECK_EQ("the stage produced what the encoder said", exact, 1);
	CHECK_EQ("the bytes ran out exactly", (int)ring.cursor, (int)len);
	printf("  residual rms error %.1f against an rms signal of %.0f (%.1f dB)\n",
	       sqrt(err / MQAE_RESIDUAL_BLOCK), sqrt(sig / MQAE_RESIDUAL_BLOCK),
	       10 * log10(sig / (err > 0 ? err : 1e-9)));
	CHECK_EQ("the residuals came back close", (int)(sqrt(err) < 0.1 * sqrt(sig)), 1);
	mqae_residual_free(&enc);
	printf("encoder residual: %s\n", test_fails ? "FAILED" : "ok");
	return test_fails != 0;
}
