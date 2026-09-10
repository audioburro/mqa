/*
 * adapter-demo -- the smallest possible host for mqa_adapter: raw stereo
 * int32 in, raw stereo int32 out, the decision reported on stderr.
 *
 *     adapter-demo [-p hold|drop|raw] [-s] [-r 44100|48000] < in.raw > out.raw
 *
 * With `-p raw` its output is what `mqad decode` produces, byte for
 * byte, which is how the adapter is tested.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqa_adapter.h"

int main(int argc, char **argv)
{
	struct mqa_adapter a;
	static int32_t in[2 * 4096], out[2 * 8192];
	unsigned rate = 44100;
	enum mqa_adapter_passthrough policy = MQA_ADAPTER_HOLD;
	int signalling = 0, i, reported = 0;
	unsigned long long frames_in = 0, frames_out = 0;
	size_t n;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-p") && i + 1 < argc) {
			const char *p = argv[++i];

			policy = !strcmp(p, "drop") ? MQA_ADAPTER_DROP
			       : !strcmp(p, "raw") ? MQA_ADAPTER_RAW : MQA_ADAPTER_HOLD;
		} else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
			rate = (unsigned)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-s")) {
			signalling = 1;
		} else {
			fprintf(stderr, "usage: %s [-p hold|drop|raw] [-s] [-r RATE] < in.raw > out.raw\n",
				argv[0]);
			return 2;
		}
	}
	mqa_adapter_init(&a, rate, 0);
	mqa_adapter_set_passthrough(&a, policy);
	if (signalling)
		mqa_adapter_set_signalling(&a, 1);

	while ((n = fread(in, 8, 4096, stdin)) > 0) {
		frames_in += n;
		if (mqa_adapter_push(&a, in, n) < 0) {
			fprintf(stderr, "adapter: the decoder declined the stream\n");
			return 1;
		}
		while ((n = mqa_adapter_pull(&a, out, 8192)) > 0) {
			fwrite(out, 8, n, stdout);
			frames_out += n;
		}
		if (!reported && mqa_adapter_state(&a) != MQA_ADAPTER_SNIFFING) {
			reported = 1;
			fprintf(stderr, "%s at %u Hz (after %llu frames)\n",
				mqa_adapter_state(&a) == MQA_ADAPTER_DECODING ? "MQA" : "not MQA",
				mqa_adapter_output_rate(&a), frames_in);
		}
	}
	mqa_adapter_drain(&a);
	while ((n = mqa_adapter_pull(&a, out, 8192)) > 0) {
		fwrite(out, 8, n, stdout);
		frames_out += n;
	}
	fprintf(stderr, "%llu frames in, %llu out (%s)\n", frames_in, frames_out,
		mqa_adapter_state(&a) == MQA_ADAPTER_DECODING ? "decoded" : "passed through");
	mqa_adapter_clear(&a);
	return 0;
}
