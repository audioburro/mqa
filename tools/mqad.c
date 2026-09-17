/*
 * mqad -- MQA files: find them, describe their streams, decode them.
 *
 *   mqad scan   PATH...            list MQA files under the paths (one line each)
 *   mqad stats  PATH...            summarise the MQA content found
 *   mqad info   FILE               describe one file's stream and packets
 *   mqad decode IN OUT             first-unfold decode to OUT (.flac or .wav)
 *
 * Input files are FLAC or WAV (stereo); raw interleaved 32-bit files
 * (24-bit samples in the top bits, 48 kHz assumed) are accepted too.
 * Decoding writes the unfolded audio at twice the input rate, 24-bit,
 * carrying the input's tags and pictures over.
 *
 * Detection and description use the library's bitstream scanner
 * (mqa/bitstream.h); decoding is the library's stream decoder
 * (mqa/stream_decoder.h), which produces the vendor decoder's output
 * bit for bit on the material it has been verified on.
 *
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include "audio_io.h"
#include "mqa/bitstream.h"
#include "mqa/resampler.h"
#include "mqa/stream_decoder.h"

/* --- describing a stream ------------------------------------------------- */

struct stream_info {
	int found;
	int xbit;
	uint64_t sync_frame;
	struct mqa_bs_datasync first;             /* the opening datasync      */
	unsigned long counts[MQA_BS_NTYPES];
	unsigned long packets, errors, resyncs;
	uint64_t terminate_at, bits_to_end;       /* the terminate packet      */
	int terminated;
	unsigned long auth_packets;
	/* metadata reassembly (type 1 = ID3v2) */
	uint8_t *metadata;
	size_t metadata_size, metadata_cap;
	unsigned metadata_type;
	int metadata_complete;
	int verbose;
	FILE *out;
};

static const char *rate_name(unsigned code, char *buf, size_t n)
{
	unsigned hz = mqa_bs_rate_hz(code);

	if (!hz)
		snprintf(buf, n, "code %u", code);
	else if (hz % 1000)
		snprintf(buf, n, "%.1f kHz", hz / 1000.0);
	else
		snprintf(buf, n, "%u kHz", hz / 1000);
	return buf;
}

static void print_datasync(FILE *f, const struct mqa_bs_datasync *d)
{
	char a[32], b[32];
	unsigned i;

	fprintf(f, "  datasync: %s, original %s, stream %s, render filter %u/%u, render bits %u, auth level %u (%s), auth info %u, %u items\n",
		d->stream_pos_flag ? "resync" : "start", rate_name(d->orig_rate, a, sizeof a),
		rate_name(d->src_rate, b, sizeof b), d->render_filter, d->unknown_1,
		(unsigned[]){ 20, 18, 16, 15 }[d->render_bitdepth & 3], d->auth_level,
		d->auth_level >= 9 ? "studio" : "green", d->auth_info, d->item_count);
	if (d->stream_pos_flag)
		fprintf(f, "    stream position %u\n", d->stream_position);
	for (i = 0; i < d->item_count && i < MQA_BS_MAX_ITEMS; i++) {
		const struct mqa_bs_item *it = &d->item[i];

		switch (it->type) {
		case 0:
			fprintf(f, "    item 0 (base band): stage2 dither %u, gain index %u, level %u, lag %u",
				it->u.base.stage2_dither, it->u.base.gain_index, it->u.base.level, it->u.base.lag);
			if (d->stream_pos_flag)
				fprintf(f, ", start %u", it->u.base.start_pos);
			fprintf(f, "\n");
			break;
		case 1:
		case 2:
			fprintf(f, "    item %u (parameters): scale %u, class %u, variant %u, salt %u", it->type, it->u.low.scale_index,
				it->u.low.carrier_class, it->u.low.variant, it->u.low.salt_select);
			if (d->stream_pos_flag)
				fprintf(f, ", sync mode %u, consumed bit %u, offset %d", it->u.low.sync_mode, it->u.low.consumed_lo,
					it->u.low.offset);
			fprintf(f, "\n");
			break;
		case 3:
			fprintf(f, "    item 3 (cipher): iv %016llx, start %u\n", (unsigned long long)it->u.cipher.iv,
				it->u.cipher.start_pos);
			break;
		default:
			fprintf(f, "    item type %u, %u bits\n", it->type, it->size);
		}
	}
}

static void on_packet(void *user, const struct mqa_bs_packet *p)
{
	struct stream_info *si = user;

	si->packets++;
	if (p->type < MQA_BS_NTYPES)
		si->counts[p->type]++;
	switch (p->type) {
	case MQA_BS_DATASYNC:
		if (!si->found) {
			si->found = 1;
			si->first = p->u.datasync;
		} else {
			si->resyncs++;
		}
		if (si->verbose) {
			fprintf(si->out, "%10llu: datasync\n", (unsigned long long)p->offset);
			print_datasync(si->out, &p->u.datasync);
		}
		break;
	case MQA_BS_TERMINATE:
		si->terminated = 1;
		si->terminate_at = p->offset;
		si->bits_to_end = p->u.terminate.bits_to_end;
		if (si->verbose)
			fprintf(si->out, "%10llu: terminate, %u bits to end\n", (unsigned long long)p->offset,
				p->u.terminate.bits_to_end);
		break;
	case MQA_BS_AUTHENTICATION:
		si->auth_packets++;
		if (si->verbose)
			fprintf(si->out, "%10llu: authentication, level %u\n", (unsigned long long)p->offset,
				p->u.auth.auth_level);
		break;
	case MQA_BS_METADATA: {
		const struct mqa_bs_metadata *m = &p->u.metadata;

		if (si->verbose)
			fprintf(si->out, "%10llu: metadata type %u fragment %u%s, %u bytes\n",
				(unsigned long long)p->offset, m->metadata_type, m->fragment_number,
				m->is_last ? " (last)" : "", m->size);
		if (m->fragment_number == 0) {
			si->metadata_size = 0;
			si->metadata_type = m->metadata_type;
			si->metadata_complete = 0;
		}
		if (si->metadata_size + m->size > si->metadata_cap) {
			size_t ncap = si->metadata_cap * 2 + m->size + 1024;
			uint8_t *nb = realloc(si->metadata, ncap);

			if (!nb)
				break;
			si->metadata = nb;
			si->metadata_cap = ncap;
		}
		memcpy(si->metadata + si->metadata_size, m->data, m->size);
		si->metadata_size += m->size;
		if (m->is_last)
			si->metadata_complete = 1;
		break;
	}
	default:
		if (si->verbose > 1)
			fprintf(si->out, "%10llu: %s, %u bits\n", (unsigned long long)p->offset,
				mqa_bs_type_name(p->type), p->bits);
		break;
	}
}

/* Scan a file's audio; `limit` frames at most (0: all). */
static int scan_stream(const char *path, struct stream_info *si, uint64_t limit, struct audio_reader *rd)
{
	struct mqa_bitstream bs;
	int32_t buf[2 * 4096];
	uint64_t done = 0;
	long n;

	if (audio_open(rd, path) < 0)
		return -1;
	mqa_bitstream_init(&bs, -1, on_packet, si);
	while ((n = audio_read(rd, buf, 4096)) > 0) {
		mqa_bitstream_feed_interleaved(&bs, buf, (size_t)n);
		done += (uint64_t)n;
		if (limit && done >= limit && !si->found)
			break;
		if (limit && done >= limit && si->found && !si->verbose)
			break;
	}
	si->xbit = bs.xbit;
	si->sync_frame = bs.sync_frame;
	si->errors = bs.errors;
	mqa_bitstream_free(&bs);
	return 0;
}

/* --- walking a tree -------------------------------------------------------- */

typedef void (*file_fn)(const char *path, void *user);

static void walk(const char *path, file_fn fn, void *user)
{
	struct stat st;

	if (stat(path, &st) < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return;
	}
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		struct dirent *e;
		char **names = NULL;
		size_t n = 0, i;

		if (!d)
			return;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.')
				continue;
			names = realloc(names, (n + 1) * sizeof *names);
			names[n++] = strdup(e->d_name);
		}
		closedir(d);
		/* deterministic order */
		for (i = 1; i < n; i++) {
			size_t j = i;

			while (j > 0 && strcmp(names[j - 1], names[j]) > 0) {
				char *t = names[j - 1];

				names[j - 1] = names[j];
				names[j] = t;
				j--;
			}
		}
		for (i = 0; i < n; i++) {
			char *full = malloc(strlen(path) + strlen(names[i]) + 2);

			sprintf(full, "%s/%s", path, names[i]);
			walk(full, fn, user);
			free(full);
			free(names[i]);
		}
		free(names);
		return;
	}
	if (audio_format_of(path) != AUDIO_UNKNOWN)
		fn(path, user);
}

/* --- scan ------------------------------------------------------------------ */

struct scan_ctx {
	unsigned long files, mqa, studio;
	unsigned long by_rate[32];
	unsigned long by_auth[16];
	unsigned long by_bit[16];
	unsigned long full;
	int quiet;
};

static void scan_one(const char *path, void *user)
{
	struct scan_ctx *sc = user;
	struct stream_info si;
	struct audio_reader rd;
	char a[32], b[32];

	memset(&si, 0, sizeof si);
	si.out = stdout;
	sc->files++;
	if (scan_stream(path, &si, sc->full ? 0 : 5 * 48000, &rd) < 0) {
		fprintf(stderr, "%s: cannot read (%s)\n", path, strerror(errno));
		return;
	}
	if (si.found) {
		sc->mqa++;
		sc->by_rate[si.first.orig_rate & 31]++;
		sc->by_auth[si.first.auth_level & 15]++;
		sc->by_bit[si.xbit & 15]++;
		if (si.first.auth_level >= 9)
			sc->studio++;
		if (!sc->quiet)
			printf("MQA  %-8s %-9s %-6s bit %2d  %s\n", rate_name(si.first.orig_rate, a, sizeof a),
			       rate_name(si.first.src_rate, b, sizeof b), si.first.auth_level >= 9 ? "studio" : "green",
			       si.xbit, path);
	} else if (!sc->quiet) {
		printf("     %-8s %-9s %-6s         %s\n", "-", "-", "-", path);
	}
	free(si.metadata);
	audio_close(&rd);
}

static int cmd_scan(int argc, char **argv, int stats_only)
{
	struct scan_ctx sc;
	int i;

	memset(&sc, 0, sizeof sc);
	sc.quiet = stats_only;
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-f")) {
			sc.full = 1;
			continue;
		}
		walk(argv[i], scan_one, &sc);
	}
	if (stats_only || sc.files > 1) {
		unsigned k;

		printf("%lu audio files, %lu MQA (%lu studio, %lu green)\n", sc.files, sc.mqa, sc.studio,
		       sc.mqa - sc.studio);
		for (k = 0; k < 32; k++)
			if (sc.by_rate[k]) {
				char a[32];

				printf("  original rate %-9s %lu\n", rate_name(k, a, sizeof a), sc.by_rate[k]);
			}
		for (k = 0; k < 16; k++)
			if (sc.by_auth[k])
				printf("  auth level %-2u          %lu\n", k, sc.by_auth[k]);
		for (k = 0; k < 16; k++)
			if (sc.by_bit[k])
				printf("  channel bit %-2u         %lu\n", k, sc.by_bit[k]);
	}
	return 0;
}

/* --- info ------------------------------------------------------------------ */

static int cmd_info(int argc, char **argv)
{
	struct stream_info si;
	struct audio_reader rd;
	const char *path = NULL, *mdfile = NULL;
	char a[32], b[32];
	int i, verbose = 1;
	unsigned k;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-v"))
			verbose = 2;
		else if (!strcmp(argv[i], "-q"))
			verbose = 0;
		else if (!strcmp(argv[i], "-m") && i + 1 < argc)
			mdfile = argv[++i];
		else
			path = argv[i];
	}
	if (!path) {
		fprintf(stderr, "usage: mqad info [-v|-q] [-m metadata.bin] FILE\n");
		return 2;
	}
	memset(&si, 0, sizeof si);
	si.out = stdout;
	si.verbose = verbose;
	if (scan_stream(path, &si, 0, &rd) < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return 1;
	}
	printf("%s\n", path);
	printf("  %u Hz, %u channels, %u bits, %llu frames", rd.rate, rd.channels, rd.bits,
	       (unsigned long long)rd.frames);
	for (i = 0; i < (int)rd.meta.ntags; i++)
		if (!strcasecmp(rd.meta.tags[i].name, "ARTIST") || !strcasecmp(rd.meta.tags[i].name, "TITLE") ||
		    !strcasecmp(rd.meta.tags[i].name, "ALBUM"))
			printf("; %s=%s", rd.meta.tags[i].name, rd.meta.tags[i].value);
	printf("\n");
	if (!si.found) {
		printf("  no MQA stream found\n");
		audio_close(&rd);
		return 1;
	}
	printf("  MQA stream: channel bit %d, starts at frame %llu; original %s, stream %s, %s (auth level %u)\n",
	       si.xbit, (unsigned long long)si.sync_frame, rate_name(si.first.orig_rate, a, sizeof a),
	       rate_name(si.first.src_rate, b, sizeof b), si.first.auth_level >= 9 ? "MQA Studio" : "MQA",
	       si.first.auth_level);
	if (!verbose)
		print_datasync(stdout, &si.first);
	printf("  %lu packets, %lu checksum errors, %lu resyncs, %lu authentication packets%s\n", si.packets,
	       si.errors, si.resyncs, si.auth_packets, si.terminated ? "" : ", no terminate packet");
	if (si.terminated)
		printf("  terminate at channel bit %llu, %llu bits to the end (stream ends at frame %llu)\n",
		       (unsigned long long)si.terminate_at, (unsigned long long)si.bits_to_end,
		       (unsigned long long)(si.sync_frame + si.terminate_at + 21 + si.bits_to_end));
	for (k = 0; k < MQA_BS_NTYPES; k++)
		if (si.counts[k])
			printf("    %-15s %lu\n", mqa_bs_type_name((enum mqa_bs_type)k), si.counts[k]);
	if (si.metadata_size) {
		printf("  stream metadata: type %u (%s), %zu bytes%s\n", si.metadata_type,
		       si.metadata_type == 1 ? "ID3v2" : "unknown", si.metadata_size,
		       si.metadata_complete ? "" : " (incomplete)");
		if (mdfile) {
			FILE *f = fopen(mdfile, "wb");

			if (f) {
				fwrite(si.metadata, 1, si.metadata_size, f);
				fclose(f);
				printf("  written to %s\n", mdfile);
			}
		}
	}
	free(si.metadata);
	audio_close(&rd);
	return 0;
}

/* --- decode ---------------------------------------------------------------- */

static struct audio_reader dec_in;
static struct audio_writer dec_out;
static int dec_error, dec_progress;
static uint64_t dec_frames_in, dec_frames_out, dec_bytes_out;
static struct mqa_bitstream dec_bs;             /* rides along for the data rate */
static uint64_t dec_stream_bits;                /* packet bits seen, holes excluded */
static struct mqa_stream_decoder dec_sd;

static void dec_packet(void *user, const struct mqa_bs_packet *p)
{
	(void)user;
	if (p->type != MQA_BS_HOLE)
		dec_stream_bits += p->bits;
}

static void show_progress(int final)
{
	double secs = dec_in.rate ? (double)dec_frames_in / dec_in.rate : 0;
	double pct = dec_in.frames ? 100.0 * (double)dec_frames_in / (double)dec_in.frames : 0;
	uint64_t bytes_in = dec_frames_in * dec_in.channels * (dec_in.bits / 8);

	fprintf(stderr, "\r%5.1f%%  in %.1f MB  out %.1f MB  MQA data %.1f kbit/s%s", pct,
		bytes_in / 1e6, dec_bytes_out / 1e6, secs > 0 ? dec_stream_bits / secs / 1000 : 0.0,
		final ? "\n" : "");
	fflush(stderr);
}

static void write_frames(const int32_t *frames, size_t count)
{
	if (audio_write(&dec_out, frames, count) < 0)
		dec_error = 1;
	dec_frames_out += count;
	dec_bytes_out += count * dec_out.channels * (dec_out.bits / 8);
}

/* --- the hidden layer (-r) and its contribution (-x) ------------------------- */

static struct audio_writer layer_out, extra_out;
static int layer_on, extra_on;
/* for -x: the same carrier reconstructed without the hidden data, by an
 * output stage of its own that follows the real one's parameters and
 * dither but keeps its own filter history */
static struct mqa_output_stage base_stage;
static int32_t base_l[4096], base_r[4096];
static unsigned base_n, base_ring_pos;
static uint32_t base_format;
static int base_steady, base_started;
static unsigned base_restarts;

static void on_layer(void *user, const int32_t *a0, const int32_t *b0, const int32_t *a, const int32_t *b,
		     const int32_t *p, const int32_t *q, unsigned count)
{
	const struct mqa_decoder *ld = &dec_sd.dec;
	int32_t lr[2 * MQA_GROUP];
	unsigned i;

	(void)user; (void)a; (void)b;
	if (layer_on) {
		for (i = 0; i < count && i < MQA_GROUP; i++) {
			lr[2 * i] = (int32_t)((uint32_t)p[i] << 8);
			lr[2 * i + 1] = (int32_t)((uint32_t)q[i] << 8);
		}
		if (audio_write(&layer_out, lr, count) < 0)
			dec_error = 1;
	}
	if (extra_on) {
		struct mqa_recon_state history = base_stage.recon;
		int32_t zero[MQA_GROUP] = { 0 };
		int n;

		base_stage = ld->output;                  /* parameters and dither as the real one */
		/* its own filter history, except where the real one was just
		 * (re)seeded by the stream: then it starts from the same */
		if (base_started && ld->output.restarts == base_restarts)
			base_stage.recon = history;
		base_started = 1;
		base_restarts = ld->output.restarts;
		base_ring_pos = ld->output_ring_pos;
		if (getenv("MQAD_XCHECK")) {              /* self-check: must reproduce the real output */
			n = mqa_output_stage_group(&base_stage, a, b, count, p, q, base_l, base_r,
						   base_format, 0, base_steady, NULL, NULL,
						   &base_ring_pos);
		} else {
			/* the carrier scaled as the refinement stage scales it,
			 * but without the refinement data */
			int32_t sa[MQA_GROUP], sb[MQA_GROUP];
			int32_t gain = ld->refine.gain;
			unsigned k;

			for (k = 0; k < count && k < MQA_GROUP; k++) {
				sa[k] = (int32_t)(((int64_t)(a0[k] << 4) * gain) >> 32);
				sb[k] = (int32_t)(((int64_t)(b0[k] << 4) * gain) >> 32);
			}
			n = mqa_output_stage_group(&base_stage, sa, sb, count, zero, zero, base_l, base_r,
						   base_format, 0, base_steady, NULL, NULL,
						   &base_ring_pos);
		}
		base_n = n > 0 ? (unsigned)n : 0;
	}
}

static void before_group(void *user, struct mqa_stream_decoder *sd)
{
	(void)user;
	base_format = sd->in.pkt.format;
	base_steady = sd->dec.primed;
	base_n = 0;
}

static void after_group(void *user, struct mqa_stream_decoder *sd, const int32_t *l, const int32_t *r, unsigned n)
{
	int32_t lr[2 * 4096];
	unsigned i;

	(void)user; (void)sd;
	if (!extra_on || n != base_n || n > 4096)
		return;
	/* the hidden data's contribution: the real output less the baseline */
	for (i = 0; i < n; i++) {
		lr[2 * i] = (int32_t)((uint32_t)(l[i] - base_l[i]) << 8);
		lr[2 * i + 1] = (int32_t)((uint32_t)(r[i] - base_r[i]) << 8);
	}
	if (audio_write(&extra_out, lr, n) < 0)
		dec_error = 1;
}

static const char *format_name(enum audio_format f)
{
	switch (f) {
	case AUDIO_FLAC: return "FLAC";
	case AUDIO_WAV: return "WAV";
	case AUDIO_RAW: return "raw";
	default: return "?";
	}
}

static int cmd_decode(int argc, char **argv)
{
	const char *in = NULL, *out = NULL, *layer = NULL, *extra = NULL;
	enum audio_format fmt;
	int i, quiet = 0, verbose = 0, signalling = 0, unfolds = 1, requantise = 0;
	unsigned ratio = 1, orig_hz = 0;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-q"))
			quiet = 1;
		else if (!strcmp(argv[i], "-p"))
			dec_progress = 1;
		else if (!strcmp(argv[i], "-v"))
			verbose = 1;
		else if (!strcmp(argv[i], "-r") && i + 1 < argc)
			layer = argv[++i];
		else if (!strcmp(argv[i], "-x") && i + 1 < argc)
			extra = argv[++i];
		else if (!strcmp(argv[i], "-s"))
			signalling = 1;
		else if (!strcmp(argv[i], "-u") && i + 1 < argc)
			unfolds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-R"))
			requantise = 1;
		else if (!in)
			in = argv[i];
		else
			out = argv[i];
	}
	if (!in || !out || (unfolds != 1 && unfolds != 2)) {
		fprintf(stderr, "usage: mqad decode [-q] [-p] [-v] [-s] [-u 1|2] [-R] [-r LAYER.wav] [-x EXTRA.wav] IN OUT\n");
		return 2;
	}
	fmt = audio_format_of(out);
	if (fmt == AUDIO_UNKNOWN) {
		fprintf(stderr, "%s: output must be .flac, .wav or .raw\n", out);
		return 2;
	}
	if (fmt == AUDIO_FLAC && !audio_have_flac()) {
		fprintf(stderr, "built without libFLAC: FLAC output is not available\n");
		return 2;
	}
	if (audio_open(&dec_in, in) < 0) {
		fprintf(stderr, "%s: %s\n", in, strerror(errno));
		return 1;
	}
	if (dec_in.channels != 2) {
		fprintf(stderr, "%s: only stereo is supported\n", in);
		return 1;
	}
	if (dec_in.rate != 48000 && dec_in.rate != 44100)
		fprintf(stderr, "%s: %u Hz input; the decoder has only been verified on 44.1 and 48 kHz material\n", in, dec_in.rate);
	if (unfolds == 2) {
		/* the second unfold goes to the original rate, which the
		 * opening datasync names: find it before the output is made */
		struct stream_info si;
		struct audio_reader rd;
		unsigned code;

		memset(&si, 0, sizeof si);
		if (scan_stream(in, &si, (uint64_t)dec_in.rate * 60, &rd) == 0)
			audio_close(&rd);
		if (si.found) {
			code = si.first.orig_rate & 31;
			if (code <= 23)
				orig_hz = (unsigned)mqa_rate_code_base[code >> 3] << (code & 7);
		}
		if (orig_hz >= 8 * dec_in.rate)
			ratio = 4;
		else if (orig_hz >= 4 * dec_in.rate)
			ratio = 2;
		if (ratio == 1 && !quiet)
			fprintf(stderr, "%s: %s; the first unfold is all there is to do\n", in,
				si.found ? "the original rate is the unfolded rate" : "no MQA stream found");
	}
	if (audio_create(&dec_out, out, fmt, 2, 2 * dec_in.rate * ratio, 24, &dec_in.meta, 2 * dec_in.frames * ratio) < 0) {
		fprintf(stderr, "%s: cannot create (%s)\n", out, strerror(errno));
		audio_close(&dec_in);
		return 1;
	}
	if (verbose) {
		printf("input:  %s, %u Hz, %u channels, %u bits, %llu frames, %zu tags, %zu pictures\n",
		       format_name(dec_in.format), dec_in.rate, dec_in.channels, dec_in.bits,
		       (unsigned long long)dec_in.frames, dec_in.meta.ntags, dec_in.meta.npictures);
		printf("output: %s, %u Hz, %u channels, %u bits -> %s\n", format_name(fmt), dec_out.rate,
		       dec_out.channels, dec_out.bits, out);
	}
	if (layer) {
		/* the residual layer at the input rate: P on the left, Q on the right */
		if (audio_create(&layer_out, layer, audio_format_of(layer), 2, dec_in.rate, 24, NULL, dec_in.frames) < 0) {
			fprintf(stderr, "%s: cannot create (%s)\n", layer, strerror(errno));
			return 1;
		}
		layer_on = 1;
	}
	if (extra) {
		/* what the hidden data adds, at the output rate */
		if (audio_create(&extra_out, extra, audio_format_of(extra), 2, 2 * dec_in.rate, 24, NULL, 2 * dec_in.frames) < 0) {
			fprintf(stderr, "%s: cannot create (%s)\n", extra, strerror(errno));
			return 1;
		}
		mqa_output_stage_init(&base_stage);
		extra_on = 1;
	}
	mqa_bitstream_init(&dec_bs, -1, dec_packet, NULL);
	mqa_stream_decoder_init(&dec_sd, dec_in.rate);
	if (signalling)
		mqa_stream_decoder_set_signalling(&dec_sd, 1);
	if (ratio > 1) {
		mqa_stream_decoder_set_render(&dec_sd, ratio, requantise);
		if (verbose)
			printf("render: to %u Hz (the original rate), %s\n", 2 * dec_in.rate * ratio,
			       requantise ? "requantised as a renderer would" : "at full precision");
	}
	if (layer_on || extra_on) {
		dec_sd.dec.on_layer = on_layer;
		dec_sd.before_group = before_group;
		dec_sd.after_group = after_group;
	}
	{
		static int32_t inbuf[2 * 4096], outbuf[2 * 8192];
		int eof = 0;

		while (!dec_error) {
			unsigned space = mqa_stream_decoder_space(&dec_sd);
			size_t produced;

			if (!eof && space > 0) {
				unsigned chunk = getenv("MQAD_CHUNK") ? (unsigned)strtoul(getenv("MQAD_CHUNK"), NULL, 0) : 4096;
				long n = audio_read(&dec_in, inbuf, space < chunk ? space : chunk);

				if (n < 0) {
					fprintf(stderr, "%s: read error\n", in);
					dec_error = 1;
					break;
				}
				if (n == 0) {
					eof = 1;
					mqa_stream_decoder_finish(&dec_sd);
				} else {
					uint64_t before = dec_frames_in, step = dec_in.rate / 2 + 1;

					dec_frames_in += (uint64_t)n;
					mqa_bitstream_feed_interleaved(&dec_bs, inbuf, (size_t)n);
					mqa_stream_decoder_feed(&dec_sd, inbuf, (size_t)n);
					if (dec_progress && before / step != dec_frames_in / step)
						show_progress(0);
				}
			}
			produced = mqa_stream_decoder_run(&dec_sd, outbuf, 8192);
			if (produced)
				write_frames(outbuf, produced);
			else if (eof)
				break;
			else if (dec_sd.declined)
				break;                    /* it cannot go on; the reason is reported below */
			else if (space == 0) {
				fprintf(stderr, "%s: the decoder stalled with full rings\n", in);
				dec_error = 1;
			}
		}
	}
	if (dec_progress)
		show_progress(1);
	if (layer_on && audio_finish(&layer_out) < 0)
		fprintf(stderr, "%s: write error\n", layer);
	if (extra_on && audio_finish(&extra_out) < 0)
		fprintf(stderr, "%s: write error\n", extra);
	audio_close(&dec_in);
	if (audio_finish(&dec_out) < 0 || dec_error) {
		fprintf(stderr, "%s: write error\n", out);
		return 1;
	}
	if (verbose) {
		char a[32];

		if (dec_bs.xbit >= 0)
			printf("stream: MQA on channel bit %d, %lu packets, %lu checksum errors, %.1f kbit/s of packet data\n",
			       dec_bs.xbit, dec_bs.packets, dec_bs.errors,
			       dec_frames_in ? dec_stream_bits * (double)dec_in.rate / dec_frames_in / 1000 : 0.0);
		else
			printf("stream: no MQA control channel found%s\n", rate_name(0, a, sizeof a)[0] ? "" : "");
	}
	mqa_bitstream_free(&dec_bs);
	if (dec_sd.declined)
		fprintf(stderr, "%s: stopped after %lu groups: %s\n", in, dec_sd.groups,
			dec_sd.dec.unsupported_why ? dec_sd.dec.unsupported_why : "a path the library does not implement");
	if (!quiet)
		printf("%s: %llu frames in, %llu frames out at %u Hz -> %s\n", in,
		       (unsigned long long)dec_frames_in, (unsigned long long)dec_out.frames, dec_out.rate, out);
	return 0;
}

/* --- main ------------------------------------------------------------------ */

static int usage(void)
{
	fprintf(stderr,
		"usage: mqad scan [-f] PATH...        list MQA files under PATH (-f: scan whole files)\n"
		"       mqad stats [-f] PATH...       summarise the MQA content found\n"
		"       mqad info [-v|-q] [-m FILE] FILE   describe a file's MQA stream (-m: save stream metadata)\n"
		"       mqad decode [-q] [-p] [-v] IN OUT   decode (first unfold) to OUT (.flac/.wav/.raw)\n"
		"                                     -u 2: the second unfold too, to the original rate\n"
		"                                     -R: requantise the rendered output as a renderer does\n"
		"                                     -p: progress, -v: show formats and the stream\n"
		"                                     -r LAYER.wav: write the encoded residual layer (P, Q) at the input rate\n"
		"                                     -x EXTRA.wav: write what the hidden data adds to the output, at the output rate\n");
	return 2;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return usage();
	if (!strcmp(argv[1], "scan"))
		return cmd_scan(argc - 2, argv + 2, 0);
	if (!strcmp(argv[1], "stats"))
		return cmd_scan(argc - 2, argv + 2, 1);
	if (!strcmp(argv[1], "info"))
		return cmd_info(argc - 2, argv + 2);
	if (!strcmp(argv[1], "decode"))
		return cmd_decode(argc - 2, argv + 2);
	return usage();
}
