/*
 * The control bitstream writer, round-tripped through the decoder's own
 * scanner: everything written here is parsed back and compared field by
 * field, checksums included. If the two disagree, one of them is wrong
 * about the format, and the decoder is the one verified against the
 * original.
 */
#include "mqae/bits.h"
#include "mqa/bitstream.h"
#include "util.h"

#define CHANNEL_BIT 3            /* bit 11 of the 24-bit sample */

static struct mqa_bs_packet got[16];
static unsigned ngot;

static void on_packet(void *user, const struct mqa_bs_packet *p)
{
	(void)user;
	if (ngot < sizeof got / sizeof got[0])
		got[ngot++] = *p;
}

/* The bits as a carrier would carry them: one per frame, in one bit of
 * the left sample, with the right sample silent. */
static void feed_as_frames(struct mqa_bitstream *bs, const struct mqae_bits *w)
{
	static int32_t l[8192], r[8192];
	uint64_t i;

	for (i = 0; i < w->nbits && i < 8192; i++) {
		l[i] = (int32_t)(mqae_bits_at(w, i) << (CHANNEL_BIT + 8 + 8));
		r[i] = 0;
	}
	mqa_bitstream_feed(bs, l, r, (size_t)(w->nbits < 8192 ? w->nbits : 8192));
}

int main(void)
{
	struct mqae_bits w;
	struct mqa_bitstream bs;
	struct mqae_datasync ds;
	static const uint8_t meta[5] = { 'h', 'e', 'l', 'l', 'o' };
	unsigned i;

	if (mqae_bits_init(&w) < 0)
		return 1;

	/* the stream opens with a datasync carrying its parameters */
	memset(&ds, 0, sizeof ds);
	ds.orig_rate = 10;                    /* 352.8 kHz */
	ds.src_rate = 0;                      /* 44.1 kHz  */
	ds.render_filter = 8;
	ds.render_bitdepth = 2;
	ds.auth_info = 0;
	ds.auth_level = 9;                    /* studio    */
	ds.nitems = 2;
	ds.item[0].type = 0;
	ds.item[0].u.base.stage2_dither = 2;
	ds.item[0].u.base.gain_index = 0;
	ds.item[0].u.base.level = 15;
	ds.item[0].u.base.lag = 127;
	ds.item[1].type = 1;
	ds.item[1].u.params.scale_index = 25;
	ds.item[1].u.params.carrier_class = 1;
	ds.item[1].u.params.variant = 0;
	ds.item[1].u.params.salt_select = 1;
	mqae_bits_datasync(&w, &ds);

	mqae_bits_hole(&w, 24);
	mqae_bits_metadata(&w, 3, 1, 0, meta, sizeof meta);
	mqae_bits_authentication(&w, 9, NULL);

	/* a later datasync: it carries a position, which rebases the
	 * checksum seed for everything after it */
	ds.with_position = 1;
	ds.position = 4096;
	ds.item[0].u.base.start_pos = 4096 / 32 + 8;
	ds.item[1].u.params.sync_mode = 1;
	ds.item[1].u.params.offset = -37;
	mqae_bits_datasync(&w, &ds);

	{
		uint64_t at = w.nbits;

		mqae_bits_terminate(&w, 1024);
		/* writers place this packet by its own length, so pin it */
		CHECK_EQ("a terminate packet's length", (int)(w.nbits - at), MQAE_BITS_TERMINATE);
	}
	CHECK_EQ("writer did not run out of memory", w.failed, 0);

	/* read it all back with the decoder's scanner */
	mqa_bitstream_init(&bs, -1, on_packet, NULL);
	feed_as_frames(&bs, &w);

	CHECK_EQ("stream found", bs.xbit, CHANNEL_BIT + 8);
	CHECK_EQ("packets parsed", ngot, 6);
	CHECK_EQ("no parse errors", bs.errors, 0);
	for (i = 0; i < ngot; i++) {
		char what[48];

		snprintf(what, sizeof what, "packet %u checksum", i);
		CHECK_EQ(what, got[i].checksum_ok, 1);
	}
	if (ngot >= 6) {
		const struct mqa_bs_datasync *d = &got[0].u.datasync;
		const struct mqa_bs_datasync *d2 = &got[4].u.datasync;

		CHECK_EQ("datasync type", got[0].type, MQA_BS_DATASYNC);
		CHECK_EQ("original rate", d->orig_rate, 10);
		CHECK_EQ("carrier rate", d->src_rate, 0);
		CHECK_EQ("render filter", d->render_filter, 8);
		CHECK_EQ("render bit depth", d->render_bitdepth, 2);
		CHECK_EQ("auth level", d->auth_level, 9);
		CHECK_EQ("item count", d->item_count, 2);
		CHECK_EQ("stage-2 dither", d->item[0].u.base.stage2_dither, 2);
		CHECK_EQ("level", d->item[0].u.base.level, 15);
		CHECK_EQ("lag", d->item[0].u.base.lag, 127);
		CHECK_EQ("scale index", d->item[1].u.low.scale_index, 25);
		CHECK_EQ("carrier class", d->item[1].u.low.carrier_class, 1);
		CHECK_EQ("variant", d->item[1].u.low.variant, 0);
		CHECK_EQ("salt selector", d->item[1].u.low.salt_select, 1);

		CHECK_EQ("hole type", got[1].type, MQA_BS_HOLE);
		CHECK_EQ("hole size", got[1].u.sized.size, 24);

		CHECK_EQ("metadata type", got[2].type, MQA_BS_METADATA);
		CHECK_EQ("metadata kind", got[2].u.metadata.metadata_type, 3);
		CHECK_EQ("metadata last", got[2].u.metadata.is_last, 1);
		CHECK_EQ("metadata size", got[2].u.metadata.size, sizeof meta);
		CHECK_EQ("metadata content", memcmp(got[2].u.metadata.data, meta, sizeof meta), 0);

		CHECK_EQ("authentication type", got[3].type, MQA_BS_AUTHENTICATION);
		CHECK_EQ("authentication level", got[3].u.auth.auth_level, 9);

		CHECK_EQ("second datasync", got[4].type, MQA_BS_DATASYNC);
		CHECK_EQ("its position flag", d2->stream_pos_flag, 1);
		CHECK_EQ("its position", d2->stream_position, 4096);
		CHECK_EQ("its start position", d2->item[0].u.base.start_pos, 4096 / 32 + 8);
		CHECK_EQ("its sync mode", d2->item[1].u.low.sync_mode, 1);
		CHECK_EQ("its offset", d2->item[1].u.low.offset, -37);

		CHECK_EQ("terminate type", got[5].type, MQA_BS_TERMINATE);
		CHECK_EQ("frames to the end", got[5].u.terminate.bits_to_end, 1024);
	}
	mqa_bitstream_free(&bs);
	mqae_bits_free(&w);
	printf("encoder bits: %s\n", test_fails ? "FAILED" : "ok");
	return test_fails != 0;
}
