/*
 * The symbol encoder against the decoder it is written from.
 *
 * The encoder and a decoder are set up in the same state; the encoder is
 * asked for a run of target symbols and produces bytes; the library's
 * entropy decoder then reads them. Two things have to hold: the decoder
 * must produce exactly what the encoder said it would (or the two have
 * drifted apart, and everything downstream is worthless), and what it
 * produces must be close to what was asked for (or the encoder is
 * choosing badly).
 */
#include <math.h>
#include "mqae/entropy.h"
#include "mqa/residual_stage.h"
#include "mqa/lcg.h"
#include "util.h"

#define SYMBOLS 4096

static int32_t target[SYMBOLS], got[SYMBOLS], back[SYMBOLS];
static uint8_t ringbuf[1 << 20];

/* The state a fresh decoder starts a block in -- the residual stage's
 * decoder_initial() followed by its gain distribution. */
static void initial(struct mqa_entropy_decoder *d, uint32_t scale, uint32_t salt)
{
	static const uint16_t decay[MQA_ENTROPY_TAPS] = { 0xf000, 0xe100, 0xd2f0, 0xc5c1 };

	memset(d, 0, sizeof *d);
	d->level = 0xe6;
	d->level_max = 0xe6;
	d->level_min = 0x39;
	d->level_rate = 0x108f;
	d->variance = 0x10000000;
	d->scale2 = scale;
	d->head = 4;
	memcpy(d->decay, decay, sizeof d->decay);
	mqa_gain_from_scale(&d->gain, (int32_t)scale);
	d->rng = salt * salt;
}

static double run(double amplitude, int report)
{
	static struct mqae_coder c;
	static struct mqae_entropy e;
	static struct mqa_entropy_decoder d;
	static struct mqa_range_decoder rc;
	static struct mqa_byte_ring ring;
	uint32_t scale = mqa_residual_scale_table[25];
	const uint8_t *bytes;
	size_t len;
	unsigned i;
	double err = 0;

	for (i = 0; i < SYMBOLS; i++)
		target[i] = (int32_t)(amplitude * sin(i * 0.021) * (0.6 + 0.4 * sin(i * 0.0013)));

	if (mqae_coder_init(&c) < 0)
		return -1;
	mqae_coder_start(&c);
	mqae_entropy_attach(&e, &c);
	initial(&e.d, scale, 0xa1e24bbau + 32);
	e.clipped = 0;
	/* a block header: level index 0, variance index 0x40 */
	mqae_entropy_block_init(&e, 0, mqa_residual_rate_table[0], 0, 0x40);
	e.d.level_max = 0xe6;
	e.d.level_min = 0x39;
	e.d.level = 0xe6;
	mqae_entropy_encode(&e, target, got, SYMBOLS);
	CHECK_EQ("the coder held up", c.failed, 0);
	bytes = mqae_coder_finish(&c, &len);
	if (c.failed && c.why)
		printf("  coder: %s\n", c.why);
	CHECK_EQ("the backward pass held up", c.failed, 0);

	memcpy(ringbuf, bytes, len);
	ring.data = ringbuf;
	ring.size = sizeof ringbuf;
	ring.cursor = 0;
	ring.wpos = (unsigned)len;
	rc.src = &ring;
	rc.base = 256;
	rc.value = 0;
	rc.range = 1;
	initial(&d, scale, 0xa1e24bbau + 32);
	d.coder = &rc;
	d.residual = &rc;
	mqa_residual_decoder_block_init(&d, 0, mqa_residual_rate_table[0]);
	d.level_max = 0xe6;
	d.level_min = 0x39;
	d.level = 0xe6;
	mqa_entropy_decode(&d, back, SYMBOLS, 1);

	for (i = 0; i < SYMBOLS; i++) {
		double e2 = (double)back[i] - target[i];

		err += e2 * e2;
		if (back[i] != got[i]) {
			printf("  symbol %u: decoded %d, encoder expected %d\n", i, back[i], got[i]);
			CHECK_EQ("the decoder produced what the encoder said", 0, 1);
			break;
		}
	}
	err = sqrt(err / SYMBOLS);
	if (report)
		printf("  amplitude %8.0f: %zu bytes (%.2f bits/symbol), rms error %.1f, %lu clipped\n",
		       amplitude, len, len * 8.0 / SYMBOLS, err, e.clipped);
	mqae_coder_free(&c);
	return err;
}

int main(void)
{
	/*
	 * The reachable symbols are spaced by the record's scale, so an
	 * encoder that always picks the nearest one has an error of a
	 * uniform quantiser: scale / sqrt(12), whatever the signal. That
	 * is the floor, and hitting it means nothing else is being lost.
	 */
	double floor_rms = mqa_residual_scale_table[25] / 3.4641016;
	double small = run(2000, 1);
	double mid = run(50000, 1);
	double big = run(1000000, 1);

	printf("  the quantiser's own floor is %.1f\n", floor_rms);
	CHECK_EQ("small signals reach the floor", (int)(small < floor_rms * 1.05), 1);
	CHECK_EQ("mid signals reach the floor", (int)(mid < floor_rms * 1.05), 1);
	CHECK_EQ("large signals reach the floor", (int)(big < floor_rms * 1.05), 1);
	printf("encoder symbols: %s\n", test_fails ? "FAILED" : "ok");
	return test_fails != 0;
}
