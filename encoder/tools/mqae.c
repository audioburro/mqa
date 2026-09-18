/*
 * mqae -- make MQA streams, and check them by decoding them.
 *
 *   encode  take an 88.2 or 96 kHz stereo file and make an MQA carrier
 *           at half its rate: analyse it into the carrier the format's
 *           filter implies and the residual that goes with it, hide the
 *           residual in the carrier's low bits, and write the result.
 *           A decoder unfolds it back to the original rate.
 *
 *   wrap    take a 44.1 or 48 kHz stereo file and give it an MQA stream
 *           without encoding anything: the signalling and a data
 *           channel, over the audio it already had. What a decoder
 *           unfolds from it is that carrier interpolated, because there
 *           is no higher-rate original here to encode.
 *
 *   verify  decode a file with the library and report what the stream
 *           said and what came out.
 *
 * Nothing here can authenticate: see encoder/README.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mqae/encode.h"
#include "mqa/stream_decoder.h"
#include "mqa/bitstream.h"
#include "audio_io.h"

#define CHUNK 4096

static int usage(void)
{
	fprintf(stderr,
		"usage: mqae encode [options] IN OUT  encode a 88.2/96 kHz file to an MQA carrier\n"
		"       mqae wrap [options] IN OUT    give a carrier an MQA stream, unencoded\n"
		"       mqae verify FILE              decode it back and say what happened\n"
		"\n"
		"options:\n"
		"  --orig HZ        what the stream claims to encode (wrap: default 2x the input)\n"
		"  --bit N          the control channel's bit, 0..7 above bit 8 (default 0)\n"
		"  --scale N        residual scale index, 0..63 (default 25)\n"
		"  --class N        carrier class, 0..3 (default 0: the data channel)\n"
		"  --variant N      reconstruction kernel: 1 short filter, 0 long (default 1)\n"
		"  --level N        refinement level, 0..127 (default 15)\n"
		"  --gain N         output gain index, 0..15; the default finds what the\n"
		"                   carrier needs so as not to clip\n"
		"  --salt N         dither salt: 0 silence, 1, 2 (default 1)\n"
		"  --filter N       render filter, 0..31 (default 8)\n"
		"  --depth N        render bit depth, 0..3 (default 2)\n"
		"  --auth N         authentication level the stream claims (default 0)\n"
		"  --resync N       a resync point every N blocks of 4096 frames, where a\n"
		"                   player can join the stream (default 16: one a block\n"
		"                   into each 65536-frame authentication block; 0 none)\n");
	return 2;
}

/* The options both encoders share. Returns 0, or 2 for a usage error. */
static int options(int argc, char **argv, struct mqae_config *cfg, unsigned *orig,
		   const char **in, const char **out, int *gain_given)
{
	int i;

	if (gain_given)
		*gain_given = 0;

	mqae_config_default(cfg);
	*orig = 0;
	*in = *out = NULL;
	for (i = 0; i < argc; i++) {
		const char *a = argv[i];

		if (a[0] != '-') {
			if (!*in)
				*in = a;
			else if (!*out)
				*out = a;
			else
				return usage();
			continue;
		}
		if (i + 1 >= argc)
			return usage();
#define OPT(name, field) if (!strcmp(a, name)) { field = (unsigned)strtoul(argv[++i], NULL, 0); continue; }
		OPT("--orig", *orig)
		OPT("--bit", cfg->xbit)
		OPT("--scale", cfg->scale_index)
		OPT("--class", cfg->carrier_class)
		OPT("--variant", cfg->variant)
		OPT("--level", cfg->level)
		if (!strcmp(a, "--gain")) {
			cfg->gain_index = (unsigned)strtoul(argv[++i], NULL, 0);
			if (gain_given)
				*gain_given = 1;
			continue;
		}
		OPT("--salt", cfg->salt_select)
		OPT("--filter", cfg->render_filter)
		OPT("--depth", cfg->render_bitdepth)
		OPT("--auth", cfg->auth.level)
		OPT("--resync", cfg->resync_blocks)
#undef OPT
		return usage();
	}
	return *in && *out ? 0 : usage();
}

#define ENCODE_CHUNK (4 * MQAE_ENCODE_BLOCK)

static int encode(int argc, char **argv)
{
	struct mqae_config cfg;
	struct mqae_encode e;
	struct audio_reader r;
	struct audio_writer w;
	const char *in, *out;
	int32_t *src = NULL, *car = NULL;
	unsigned orig;
	uint64_t done = 0;
	long n;
	int rc = 1, gain_given = 0;

	if (options(argc, argv, &cfg, &orig, &in, &out, &gain_given) != 0)
		return 2;
	if (audio_open(&r, in) < 0) {
		fprintf(stderr, "mqae: cannot read %s\n", in);
		return 1;
	}
	if (r.channels != 2) {
		fprintf(stderr, "mqae: %s is not stereo\n", in);
		audio_close(&r);
		return 1;
	}
	if (r.rate % 2) {
		fprintf(stderr, "mqae: %u Hz cannot be halved\n", r.rate);
		audio_close(&r);
		return 1;
	}
	cfg.src_rate = r.rate / 2;
	cfg.orig_rate = orig ? orig : r.rate;
	if (cfg.src_rate != 44100 && cfg.src_rate != 48000)
		fprintf(stderr, "mqae: warning: %u Hz makes a %u Hz carrier, which is not an MQA rate\n",
			r.rate, cfg.src_rate);
	/*
	 * A full-scale source makes a full-scale carrier, and whatever is
	 * above the source's own Nyquist pushes it over. The datasync's
	 * gain index is the format's answer: the encoder attenuates and
	 * the decoder puts it back. So unless the caller has chosen one,
	 * find the peak first and take as much headroom as it needs. The
	 * measuring pass does the analysis and nothing else, which is a
	 * fraction of the cost of encoding.
	 */
	if (!gain_given) {
		struct mqae_encode m;
		int32_t *buf = malloc(2 * ENCODE_CHUNK * 2 * sizeof *buf);
		long got;

		if (!buf || mqae_encode_open(&m, &cfg, r.frames / 2) < 0) {
			free(buf);
			audio_close(&r);
			return 1;
		}
		m.measure_only = 1;
		while ((got = audio_read(&r, buf, 2 * ENCODE_CHUNK)) >= 0) {
			size_t produced = 0;

			if (mqae_encode_push(&m, buf, (size_t)got, got == 0, NULL, 0, &produced) < 0)
				break;
			if (got == 0)
				break;
		}
		cfg.gain_index = mqae_encode_headroom(m.carrier_peak, cfg.gain_index);
		printf("  the carrier peaks at %.2f dBFS: gain index %u of headroom\n",
		       20 * log10((m.carrier_peak > 0 ? m.carrier_peak : 1) / 8388608.0),
		       cfg.gain_index);
		mqae_encode_close(&m);
		free(buf);
		audio_close(&r);
		if (audio_open(&r, in) < 0) {
			fprintf(stderr, "mqae: cannot re-read %s\n", in);
			return 1;
		}
	}
	if (mqae_encode_open(&e, &cfg, r.frames / 2) < 0) {
		audio_close(&r);
		return 1;
	}
	if (audio_create(&w, out, audio_format_of(out), 2, cfg.src_rate, 24, &r.meta,
			 r.frames / 2) < 0) {
		fprintf(stderr, "mqae: cannot write %s\n", out);
		goto done;
	}
	src = malloc(2 * ENCODE_CHUNK * 2 * sizeof *src);
	car = malloc(2 * ENCODE_CHUNK * 2 * sizeof *car);
	if (!src || !car)
		goto done;
	for (;;) {
		size_t produced = 0;
		int end;

		n = audio_read(&r, src, 2 * ENCODE_CHUNK);
		if (n < 0)
			break;
		end = n == 0;
		if (mqae_encode_push(&e, src, (size_t)n, end, car,
				     2 * ENCODE_CHUNK, &produced) < 0) {
			fprintf(stderr, "mqae: encoding failed\n");
			break;
		}
		if (produced && audio_write(&w, car, produced) < 0) {
			fprintf(stderr, "mqae: write failed\n");
			break;
		}
		done += produced;
		if (end && !produced)
			break;
	}
	if (audio_finish(&w) < 0)
		fprintf(stderr, "mqae: could not finish %s\n", out);
	printf("%s: %llu carrier frames at %u Hz from %u Hz\n", out,
	       (unsigned long long)done, cfg.src_rate, r.rate);
	printf("  data channel: %llu bytes of residual and %llu of refinement"
	       " (%.1f of the 16 bits a frame it has)\n",
	       (unsigned long long)e.bytes, (unsigned long long)e.aux_bytes,
	       done ? (e.bytes + e.aux_bytes) * 8.0 / done : 0.0);
	if (e.chan_peak)
		printf("  the channel fell behind by at most %u bytes%s\n", (unsigned)e.chan_peak,
		       e.chan_peak > 900 ? " -- too far: a decoder's rings will overrun."
					   " Use a coarser --scale" : "");
	if (e.counted) {
		printf("  loss: the carrier the decoder sees is %.1f dB from the one the"
		       " analysis wanted (rms %.0f of %.0f),\n",
		       10 * log10(e.carrier_energy /
				  (e.carrier_error > 0 ? e.carrier_error : 1e-9)),
		       sqrt(e.carrier_error / e.counted), sqrt(e.carrier_energy / e.counted));
		printf("        the refinement corrected it to %.1f dB, %lu of its"
		       " symbols clipped\n",
		       10 * log10(e.refine.want_energy /
				  (e.refine.error > 0 ? e.refine.error : 1e-9)),
		       e.refine.clipped);
		printf("        the residuals are coded %.1f dB down"
		       " (their symbols %.1f dB), %lu clipped\n",
		       10 * log10(e.residual_energy / (e.coding_error > 0 ? e.coding_error : 1e-9)),
		       10 * log10(e.res.symbol_energy /
				  (e.res.symbol_error > 0 ? e.res.symbol_error : 1e-9)),
		       e.clipped);
	}
	if (e.clipped_carrier)
		printf("  %lu carrier samples clipped even so: the source has more above its\n"
		       "  own Nyquist than the format's 2.8 dB of headroom can hold\n",
		       e.clipped_carrier);
	if (e.counted)
		printf("  the encoder expects %.1f dB (rms error %.0f), left channel\n",
		       10 * log10(e.out_energy / (e.out_error > 0 ? e.out_error : 1e-9)),
		       sqrt(e.out_error / (2 * e.counted)));
	printf("  unauthenticated: an MQA decoder shows no indicator for it, and ends it\n"
	       "  at its first 65536-frame block; this library's decoder plays it all\n");
	rc = 0;
done:
	free(src);
	free(car);
	mqae_encode_close(&e);
	audio_close(&r);
	return rc;
}

static int wrap(int argc, char **argv)
{
	struct mqae_config cfg;
	struct mqae_encoder e;
	struct audio_reader r;
	struct audio_writer w;
	int32_t *buf;
	const char *in, *out;
	unsigned orig;
	uint64_t done = 0;
	long n;
	int rc = 1;

	if (options(argc, argv, &cfg, &orig, &in, &out, NULL) != 0)
		return 2;
	if (audio_open(&r, in) < 0) {
		fprintf(stderr, "mqae: cannot read %s\n", in);
		return 1;
	}
	if (r.channels != 2) {
		fprintf(stderr, "mqae: %s is not stereo\n", in);
		audio_close(&r);
		return 1;
	}
	if (r.rate != 44100 && r.rate != 48000)
		fprintf(stderr, "mqae: warning: %u Hz is not an MQA carrier rate\n", r.rate);
	cfg.src_rate = r.rate;
	cfg.orig_rate = orig ? orig : r.rate * 2;
	if (mqae_encoder_open(&e, &cfg, r.frames) < 0) {
		audio_close(&r);
		return 1;
	}
	if (audio_create(&w, out, audio_format_of(out), 2, r.rate, 24, &r.meta, r.frames) < 0) {
		fprintf(stderr, "mqae: cannot write %s\n", out);
		goto done;
	}
	buf = malloc(CHUNK * 2 * sizeof *buf);
	if (!buf)
		goto done;
	while ((n = audio_read(&r, buf, CHUNK)) > 0) {
		mqae_encoder_write(&e, buf, (size_t)n, 0);
		if (e.failed) {
			fprintf(stderr, "mqae: out of memory\n");
			break;
		}
		if (audio_write(&w, buf, (size_t)n) < 0) {
			fprintf(stderr, "mqae: write failed\n");
			break;
		}
		done += (uint64_t)n;
	}
	free(buf);
	if (audio_finish(&w) < 0)
		fprintf(stderr, "mqae: could not finish %s\n", out);
	printf("%s: %llu frames at %u Hz, claiming %u Hz\n", out,
	       (unsigned long long)done, r.rate, cfg.orig_rate);
	printf("  control channel %llu bits, data channel %llu bytes\n",
	       (unsigned long long)e.bits.nbits, (unsigned long long)e.chan.len);
	printf("  unauthenticated: an MQA decoder shows no indicator for it, and ends it\n"
	       "  at its first 65536-frame block; this library's decoder plays it all\n");
	rc = 0;
done:
	mqae_encoder_close(&e);
	audio_close(&r);
	return rc;
}

static int verify(int argc, char **argv)
{
	static struct mqa_stream_decoder sd;
	struct audio_reader r;
	int32_t *in, *out;
	uint64_t frames = 0, produced = 0;
	long n;

	if (argc != 1)
		return usage();
	if (audio_open(&r, argv[0]) < 0) {
		fprintf(stderr, "mqae: cannot read %s\n", argv[0]);
		return 1;
	}
	in = malloc(CHUNK * 2 * sizeof *in);
	out = malloc((2 * CHUNK + MQA_STREAM_DECODER_GROUP_MAX) * 2 * sizeof *out);
	if (!in || !out)
		return 1;
	mqa_stream_decoder_init(&sd, r.rate);
	while ((n = audio_read(&r, in, CHUNK)) > 0) {
		size_t fed = 0;

		frames += (uint64_t)n;
		while (fed < (size_t)n) {
			unsigned space = mqa_stream_decoder_space(&sd);
			size_t take = (size_t)n - fed < space ? (size_t)n - fed : space;

			if (take)
				fed += mqa_stream_decoder_feed(&sd, in + 2 * fed, take);
			produced += mqa_stream_decoder_run(&sd, out,
					2 * CHUNK + MQA_STREAM_DECODER_GROUP_MAX);
			if (!take && !space)
				break;
		}
	}
	mqa_stream_decoder_finish(&sd);
	for (;;) {
		size_t got = mqa_stream_decoder_run(&sd, out, 2 * CHUNK + MQA_STREAM_DECODER_GROUP_MAX);

		if (!got)
			break;
		produced += got;
	}
	printf("%s: %u Hz, %llu frames\n", argv[0], r.rate, (unsigned long long)frames);
	if (sd.in.bs.xbit < 0) {
		printf("  no MQA stream\n");
	} else {
		printf("  stream at frame %llu, channel bit %d, original %u Hz\n",
		       (unsigned long long)sd.in.bs.sync_frame, sd.in.bs.xbit - MQA_BS_MIN_BIT,
		       mqa_bs_rate_hz(sd.in.orig_rate));
		printf("  render filter %u, bit depth %u, auth level %u%s\n",
		       sd.in.render_filter, sd.in.render_bitdepth, sd.in.auth_level,
		       sd.in.auth_level >= 8 ? " (claims studio)" : "");
	}
	printf("  %lu groups, %lu declined, %llu frames out (%lu passed through)\n",
	       sd.groups, sd.declined, (unsigned long long)produced, sd.dec.passed);
	if (sd.dec.unsupported_why)
		printf("  stopped: %s\n", sd.dec.unsupported_why);
	printf("  control bitstream: %lu packets, %lu errors\n", sd.in.bs.packets, sd.in.bs.errors);
	free(in);
	free(out);
	audio_close(&r);
	return sd.declined != 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return usage();
	if (!strcmp(argv[1], "encode"))
		return encode(argc - 2, argv + 2);
	if (!strcmp(argv[1], "wrap"))
		return wrap(argc - 2, argv + 2);
	if (!strcmp(argv[1], "verify"))
		return verify(argc - 2, argv + 2);
	return usage();
}
