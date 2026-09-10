/*
 * The data channel, round-tripped through the decoder's own message
 * parser and descrambler: everything written is scrambled as the carrier
 * would carry it, descrambled again, parsed, and compared.
 */
#include "mqae/chan.h"
#include "mqa/stream.h"
#include "mqa/descrambler.h"
#include "util.h"

static struct mqa_stream_record rec;
static unsigned nrecords, nsyncs, nfails;
static struct mqa_stream_sync last_sync;

static void on_record(void *user, const struct mqa_stream_record *r)
{
	(void)user;
	rec = *r;
	nrecords++;
}

static void on_sync(void *user, const struct mqa_stream_sync *s)
{
	(void)user;
	last_sync = *s;
	nsyncs++;
}

static void on_check_fail(void *user, uint8_t mode)
{
	(void)user; (void)mode;
	nfails++;
}

int main(void)
{
	struct mqae_chan c;
	struct mqa_stream_parser p;
	struct mqa_byte_ring sym, aux;
	struct mqa_descrambler ks;
	static uint8_t sym_data[2048], aux_data[1024], wire[8192];
	static uint8_t symbols[255], refine[64], seeds[8], state[32];
	uint32_t crc = 0xdeadbeefu;
	unsigned i;
	size_t off;

	for (i = 0; i < sizeof symbols; i++)
		symbols[i] = (uint8_t)(i * 7 + 3);
	for (i = 0; i < sizeof refine; i++)
		refine[i] = (uint8_t)(200 - i);
	for (i = 0; i < sizeof seeds; i++)
		seeds[i] = (uint8_t)(i + 1);
	for (i = 0; i < sizeof state; i++)
		state[i] = (uint8_t)(0x40 + i);

	if (mqae_chan_init(&c) < 0)
		return 1;
	mqae_chan_record(&c, 25, seeds, state);
	mqae_chan_message(&c, 5, symbols, sizeof symbols);
	mqae_chan_message(&c, 2, refine, sizeof refine);
	mqae_chan_idle(&c, 5);
	mqae_chan_sync(&c, 5, 25, NULL);
	mqae_chan_sync(&c, 2, 25, &crc);
	mqae_chan_message(&c, 1, NULL, 200);          /* the type a decoder drops */
	mqae_chan_pad_to(&c, 1024);
	CHECK_EQ("channel written", c.failed, 0);
	CHECK_EQ("channel padded", (int)c.len, 1024);

	/* as the carrier carries it, and back */
	memcpy(wire, c.data, c.len);
	CHECK_EQ("scrambled to the end", (int)(mqae_chan_wire(&c, c.len) == c.data), 1);
	CHECK_EQ("all of it", (int)c.scrambled, (int)c.len);
	CHECK_EQ("the wire differs from the plaintext", memcmp(c.data, wire, c.len) != 0, 1);
	memcpy(wire, c.data, c.len);
	mqa_descrambler_init(&ks);
	mqa_descrambler_start(&ks, 0);
	mqa_descrambler_fill_bytes(&ks, wire, wire, 0, (unsigned)c.len);
	{
		static uint8_t plain[8192];
		size_t k;

		/* rebuild the plaintext to compare against */
		mqae_chan_free(&c);
		if (mqae_chan_init(&c) < 0)
			return 1;
		mqae_chan_record(&c, 25, seeds, state);
		mqae_chan_message(&c, 5, symbols, sizeof symbols);
		mqae_chan_message(&c, 2, refine, sizeof refine);
		mqae_chan_idle(&c, 5);
		mqae_chan_sync(&c, 5, 25, NULL);
		mqae_chan_sync(&c, 2, 25, &crc);
		mqae_chan_message(&c, 1, NULL, 200);
		mqae_chan_pad_to(&c, 1024);
		for (k = 0; k < c.len; k++)
			plain[k] = c.data[k];
		CHECK_EQ("descrambles back", memcmp(wire, plain, c.len), 0);
	}

	/* parse it as the decoder does: 64 bytes a group */
	sym.data = sym_data; sym.size = sizeof sym_data; sym.cursor = sym.wpos = 0;
	aux.data = aux_data; aux.size = sizeof aux_data; aux.cursor = aux.wpos = 0;
	mqa_stream_init(&p, &sym, &aux);
	p.on_record = on_record;
	p.on_sync = on_sync;
	p.on_check_fail = on_check_fail;
	p.started = 1;
	p.consumed = 0;
	for (off = 0; off < c.len; ) {
		unsigned n = mqa_stream_push(&p, wire + off, (unsigned)(c.len - off < 64 ? c.len - off : 64));

		off += n;
		mqa_stream_parse(&p, 0, 1);
		if (n == 0)
			break;
	}
	CHECK_EQ("no check failed", nfails, 0);
	CHECK_EQ("the whole channel parsed", (int)(off == c.len), 1);
	CHECK_EQ("still online", p.rings_enabled, 1);
	CHECK_EQ("one record", nrecords, 1);
	CHECK_EQ("record id", rec.id, 25u << 6 | 25u << 12 | 25u << 18);
	CHECK_EQ("record part A", rec.a_len, 8);
	CHECK_EQ("record part B", rec.b_len, 32);
	CHECK_EQ("part A content", memcmp(rec.a, seeds, sizeof seeds), 0);
	CHECK_EQ("part B content", memcmp(rec.b, state, sizeof state), 0);
	CHECK_EQ("symbols routed", memcmp(sym_data, symbols, sizeof symbols), 0);
	CHECK_EQ("symbol ring fill", sym.wpos, sizeof symbols);
	CHECK_EQ("refinement routed", memcmp(aux_data, refine, sizeof refine), 0);
	CHECK_EQ("auxiliary ring fill", aux.wpos, sizeof refine);
	CHECK_EQ("two syncs", nsyncs, 2);
	CHECK_EQ("sync kind", last_sync.kind, 2);
	CHECK_EQ("sync carried a word", last_sync.flag, 1);
	CHECK_EQ("sync word", last_sync.word, crc);

	mqae_chan_free(&c);
	printf("encoder channel: %s\n", test_fails ? "FAILED" : "ok");
	return test_fails != 0;
}
