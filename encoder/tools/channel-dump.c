/*
 * channel-dump -- what a real MQA stream's data channel says.
 *
 * The encoder has to produce this byte stream, so this reads one: the
 * tool finds the stream in a file, descrambles the low
 * bytes of the carrier from the frame the stream opens at, and prints
 * the framed messages it finds, their types, sizes and check nibbles.
 *
 * It is a diagnostic, not part of the encoder: everything it knows comes
 * from mqa/descrambler.h and mqa/stream.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqa/bitstream.h"
#include "mqa/descrambler.h"
#include "mqa/stream.h"
#include "audio_io.h"

static const char *msg_name(unsigned type)
{
	switch (type) {
	case 0: return "idle";
	case 1: return "type-1";
	case 2: return "auxiliary";   /* refinement */
	case 3: return "sync";
	case 4: return "record";
	case 5: return "symbols";
	default: return "other";
	}
}

int main(int argc, char **argv)
{
	struct audio_reader r;
	struct mqa_bitstream bs;
	struct mqa_descrambler ks;
	int32_t *lr;
	uint8_t *chan;
	uint64_t frames, sync, n, i;
	unsigned long limit = 64, shown = 0;
	unsigned long counts[16];
	size_t off, len;

	if (argc < 2) {
		fprintf(stderr, "usage: channel-dump FILE [MESSAGES]\n");
		return 2;
	}
	if (argc > 2)
		limit = strtoul(argv[2], NULL, 0);
	if (audio_open(&r, argv[1]) < 0) {
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 1;
	}
	frames = r.frames ? r.frames : 8u << 20;
	lr = malloc((size_t)frames * 2 * sizeof *lr);
	if (!lr)
		return 1;
	{
		long got, total = 0;

		while ((got = audio_read(&r, lr + 2 * total, (size_t)(frames - total))) > 0) {
			total += got;
			if ((uint64_t)total >= frames)
				break;
		}
		frames = (uint64_t)total;
	}
	audio_close(&r);

	/* find the stream: the scanner reports the frame the magic began at */
	mqa_bitstream_init(&bs, -1, NULL, NULL);
	if (!mqa_bitstream_feed_interleaved(&bs, lr, (size_t)frames)) {
		printf("%s: no MQA stream\n", argv[1]);
		return 1;
	}
	sync = bs.sync_frame;
	printf("%s: %u Hz, %llu frames; stream at frame %llu, channel bit %d\n",
	       argv[1], r.rate, (unsigned long long)frames,
	       (unsigned long long)sync, bs.xbit);

	/* the data channel: every sample's low byte from there, descrambled */
	n = (frames - sync) * 2;
	chan = malloc((size_t)n);
	if (!chan)
		return 1;
	for (i = 0; i < frames - sync; i++) {
		chan[2 * i] = (uint8_t)(lr[2 * (sync + i)] >> 8);
		chan[2 * i + 1] = (uint8_t)(lr[2 * (sync + i) + 1] >> 8);
	}
	mqa_descrambler_init(&ks);
	mqa_descrambler_start(&ks, 0);
	mqa_descrambler_fill_bytes(&ks, chan, chan, 0, (unsigned)(n & ~1u));

	/* walk the framing */
	memset(counts, 0, sizeof counts);
	off = 0;
	while (off + 8 < n) {
		unsigned type = chan[off] & 0xf, nibble = chan[off] >> 4;
		unsigned extra, payload = 0, k;
		uint32_t reg = (uint32_t)off;

		if (type == 4)
			extra = 5;
		else if (type == 0)
			extra = 0;
		else if (type == 3)
			extra = (chan[off + 1] & 2) ? ((chan[off + 1] & 1) ? 7 : 5)
						   : ((chan[off + 1] & 1) ? 3 : 1);
		else
			extra = 1;
		if (type == 4)
			payload = (unsigned)chan[off + 4] + chan[off + 5];
		else if (type != 0 && type != 3)
			payload = chan[off + 1];
		len = 1 + extra + payload;
		for (k = 1; k <= extra; k++)
			reg = mqa_stream_check_update(reg, chan[off + k]);
		for (k = 0; k < payload; k++)
			reg = mqa_stream_check_update(reg, chan[off + 1 + extra + k]);
		counts[type]++;
		if (shown < limit) {
			printf("  %8lu  type %-2u %-10s header %u payload %-4u check %s",
			       (unsigned long)off, type, msg_name(type), extra, payload,
			       (reg & 0xf) == nibble ? "ok" : "BAD");
			if (type == 4) {
				uint32_t id = (uint32_t)chan[off + 1] | (uint32_t)chan[off + 2] << 8 |
					      (uint32_t)chan[off + 3] << 16;

				printf("  id %06x (scales %u/%u/%u) A %u B %u",
				       id, (id >> 6) & 63, (id >> 12) & 63, (id >> 18) & 63,
				       chan[off + 4], chan[off + 5]);
			}
			putchar('\n');
			shown++;
		}
		if ((reg & 0xf) != nibble) {
			printf("  check failed at %lu: stopping\n", (unsigned long)off);
			break;
		}
		off += len;
	}
	printf("  messages:");
	for (i = 0; i < 16; i++)
		if (counts[i])
			printf(" %s(%u)=%lu", msg_name((unsigned)i), (unsigned)i, counts[i]);
	printf("\n  bytes walked: %lu of %llu\n", (unsigned long)off, (unsigned long long)n);
	free(chan);
	free(lr);
	mqa_bitstream_free(&bs);
	return 0;
}
