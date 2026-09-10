/*
 * audio_io.c -- FLAC (libFLAC), WAV and raw stereo files for mqad.
 *
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include "audio_io.h"

#ifdef HAVE_FLAC
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <FLAC/metadata.h>
#endif

int audio_have_flac(void)
{
#ifdef HAVE_FLAC
	return 1;
#else
	return 0;
#endif
}

enum audio_format audio_format_of(const char *path)
{
	const char *dot = strrchr(path, '.');

	if (!dot)
		return AUDIO_UNKNOWN;
	if (!strcasecmp(dot, ".flac"))
		return AUDIO_FLAC;
	if (!strcasecmp(dot, ".wav") || !strcasecmp(dot, ".wave"))
		return AUDIO_WAV;
	if (!strcasecmp(dot, ".raw") || !strcasecmp(dot, ".pcm"))
		return AUDIO_RAW;
	return AUDIO_UNKNOWN;
}

/* --- metadata ------------------------------------------------------------- */

static int meta_add_tag(struct audio_meta *m, const char *name, size_t nlen, const char *value)
{
	struct audio_tag *t = realloc(m->tags, (m->ntags + 1) * sizeof *t);

	if (!t)
		return -1;
	m->tags = t;
	t[m->ntags].name = malloc(nlen + 1);
	t[m->ntags].value = strdup(value);
	if (!t[m->ntags].name || !t[m->ntags].value)
		return -1;
	memcpy(t[m->ntags].name, name, nlen);
	t[m->ntags].name[nlen] = 0;
	m->ntags++;
	return 0;
}

const char *audio_tag_get(const struct audio_meta *m, const char *name)
{
	size_t i;

	for (i = 0; i < m->ntags; i++)
		if (!strcasecmp(m->tags[i].name, name))
			return m->tags[i].value;
	return NULL;
}

void audio_meta_free(struct audio_meta *m)
{
	size_t i;

	for (i = 0; i < m->ntags; i++) {
		free(m->tags[i].name);
		free(m->tags[i].value);
	}
	free(m->tags);
	for (i = 0; i < m->npictures; i++) {
		free(m->pictures[i].mime);
		free(m->pictures[i].description);
		free(m->pictures[i].data);
	}
	free(m->pictures);
	memset(m, 0, sizeof *m);
}

/* --- WAV ------------------------------------------------------------------ */

struct wav_priv {
	FILE *f;
	unsigned block_align;
	uint64_t left;                     /* frames left in the data chunk   */
	unsigned bits;
	long data_start;                   /* writer: where the data chunk begins */
	uint8_t *info;                     /* a LIST INFO chunk, kept verbatim */
	uint32_t info_size;
};

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int wav_open(struct audio_reader *r, FILE *f)
{
	struct wav_priv *w = calloc(1, sizeof *w);
	uint8_t hdr[12], ch[8];
	int have_fmt = 0;

	if (!w)
		return -1;
	w->f = f;
	if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
		goto bad;
	for (;;) {
		uint32_t size;

		if (fread(ch, 1, 8, f) != 8)
			goto bad;
		size = le32(ch + 4);
		if (!memcmp(ch, "fmt ", 4)) {
			uint8_t fmt[40];
			unsigned n = size < sizeof fmt ? size : sizeof fmt;
			unsigned tag;

			if (fread(fmt, 1, n, f) != n)
				goto bad;
			if (size > n && fseek(f, (long)(size - n), SEEK_CUR))
				goto bad;
			tag = le16(fmt);
			r->channels = le16(fmt + 2);
			r->rate = le32(fmt + 4);
			w->block_align = le16(fmt + 12);
			r->bits = le16(fmt + 14);
			if (tag == 0xfffe && n >= 26)
				tag = le16(fmt + 24);
			if (tag != 1 || r->channels != 2 || (r->bits != 16 && r->bits != 24 && r->bits != 32))
				goto bad;
			w->bits = r->bits;
			have_fmt = 1;
		} else if (!memcmp(ch, "data", 4)) {
			if (!have_fmt)
				goto bad;
			w->left = size / w->block_align;
			r->frames = w->left;
			break;
		} else if (!memcmp(ch, "LIST", 4)) {
			w->info = malloc(size + 8);
			if (!w->info)
				goto bad;
			memcpy(w->info, ch, 8);
			if (fread(w->info + 8, 1, size, f) != size)
				goto bad;
			w->info_size = size + 8;
			if (size & 1)
				(void)fgetc(f);
		} else {
			if (fseek(f, (long)(size + (size & 1)), SEEK_CUR))
				goto bad;
		}
	}
	r->format = AUDIO_WAV;
	r->priv = w;
	return 0;
bad:
	free(w->info);
	free(w);
	return -1;
}

static long wav_read(struct audio_reader *r, int32_t *lr, size_t n)
{
	struct wav_priv *w = r->priv;
	uint8_t buf[4096];
	size_t got = 0;

	while (got < n && w->left) {
		size_t frames = sizeof buf / w->block_align;
		size_t k, i;

		if (frames > n - got)
			frames = n - got;
		if (frames > w->left)
			frames = (size_t)w->left;
		k = fread(buf, w->block_align, frames, w->f);
		if (!k)
			break;
		for (i = 0; i < 2 * k; i++) {
			const uint8_t *p = buf + i * (w->bits / 8);
			int32_t v;

			switch (w->bits) {
			case 16: v = (int32_t)((uint32_t)le16(p) << 16); break;
			case 24: v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24); break;
			default: v = (int32_t)le32(p); break;
			}
			lr[2 * got + i] = v;
		}
		got += k;
		w->left -= k;
	}
	return (long)got;
}

static void put32(FILE *f, uint32_t v) { uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }
static void put16(FILE *f, unsigned v) { uint8_t b[2] = { v, v >> 8 }; fwrite(b, 1, 2, f); }

static int wav_create(struct audio_writer *wr, const char *path, unsigned channels, unsigned rate,
		      unsigned bits, const struct audio_meta *meta)
{
	struct wav_priv *w = calloc(1, sizeof *w);
	FILE *f;

	(void)meta;
	if (!w)
		return -1;
	f = fopen(path, "wb");
	if (!f) {
		free(w);
		return -1;
	}
	w->f = f;
	w->bits = bits;
	w->block_align = channels * bits / 8;
	fwrite("RIFF", 1, 4, f);
	put32(f, 0);                         /* patched at the end */
	fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f);
	put32(f, 16);
	put16(f, 1);
	put16(f, channels);
	put32(f, rate);
	put32(f, rate * w->block_align);
	put16(f, w->block_align);
	put16(f, bits);
	fwrite("data", 1, 4, f);
	put32(f, 0);
	w->data_start = ftell(f);
	wr->priv = w;
	wr->format = AUDIO_WAV;
	return 0;
}

static int wav_write(struct audio_writer *wr, const int32_t *lr, size_t n)
{
	struct wav_priv *w = wr->priv;
	uint8_t buf[4096];
	size_t i = 0;

	while (i < 2 * n) {
		size_t k = 0;

		while (i < 2 * n && k + 4 <= sizeof buf) {
			uint32_t v = (uint32_t)lr[i++];

			switch (w->bits) {
			case 16: buf[k++] = (uint8_t)(v >> 16); buf[k++] = (uint8_t)(v >> 24); break;
			case 24: buf[k++] = (uint8_t)(v >> 8); buf[k++] = (uint8_t)(v >> 16); buf[k++] = (uint8_t)(v >> 24); break;
			default: buf[k++] = (uint8_t)v; buf[k++] = (uint8_t)(v >> 8); buf[k++] = (uint8_t)(v >> 16); buf[k++] = (uint8_t)(v >> 24); break;
			}
		}
		if (fwrite(buf, 1, k, w->f) != k)
			return -1;
	}
	wr->frames += n;
	return 0;
}

static int wav_finish(struct audio_writer *wr)
{
	struct wav_priv *w = wr->priv;
	long end = ftell(w->f);
	uint32_t data = (uint32_t)(end - w->data_start);

	if (data & 1)
		fputc(0, w->f);
	fseek(w->f, w->data_start - 4, SEEK_SET);
	put32(w->f, data);
	fseek(w->f, 4, SEEK_SET);
	put32(w->f, (uint32_t)(end + (data & 1) - 8));
	fclose(w->f);
	free(w);
	return 0;
}

/* --- raw ----------------------------------------------------------------- */

struct raw_priv { FILE *f; };

static long raw_read(struct audio_reader *r, int32_t *lr, size_t n)
{
	struct raw_priv *p = r->priv;

	return (long)fread(lr, 8, n, p->f);
}

/* --- FLAC ---------------------------------------------------------------- */
#ifdef HAVE_FLAC

struct flac_priv {
	FLAC__StreamDecoder *dec;
	FLAC__StreamEncoder *enc;
	FLAC__StreamMetadata *blocks[4];
	unsigned nblocks;
	int32_t *pending;                  /* decoded frames not yet taken    */
	size_t pending_n, pending_pos;
	int eof, error;
	struct audio_reader *reader;
};

static FLAC__StreamDecoderWriteStatus flac_write_cb(const FLAC__StreamDecoder *dec,
		const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *client)
{
	struct flac_priv *p = client;
	unsigned n = frame->header.blocksize, shift = 32 - frame->header.bits_per_sample, i;
	int32_t *out;

	(void)dec;
	out = realloc(p->pending, (p->pending_n - p->pending_pos + n) * 8);
	if (!out) {
		p->error = 1;
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}
	if (p->pending_pos) {
		memmove(out, out + 2 * p->pending_pos, (p->pending_n - p->pending_pos) * 8);
		p->pending_n -= p->pending_pos;
		p->pending_pos = 0;
	}
	p->pending = out;
	for (i = 0; i < n; i++) {
		out[2 * (p->pending_n + i)] = (int32_t)((uint32_t)buffer[0][i] << shift);
		out[2 * (p->pending_n + i) + 1] = (int32_t)((uint32_t)buffer[1][i] << shift);
	}
	p->pending_n += n;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void flac_meta_cb(const FLAC__StreamDecoder *dec, const FLAC__StreamMetadata *m, void *client)
{
	struct flac_priv *p = client;
	struct audio_reader *r = p->reader;
	unsigned i;

	(void)dec;
	switch (m->type) {
	case FLAC__METADATA_TYPE_STREAMINFO:
		r->channels = m->data.stream_info.channels;
		r->rate = m->data.stream_info.sample_rate;
		r->bits = m->data.stream_info.bits_per_sample;
		r->frames = m->data.stream_info.total_samples;
		break;
	case FLAC__METADATA_TYPE_VORBIS_COMMENT:
		for (i = 0; i < m->data.vorbis_comment.num_comments; i++) {
			const FLAC__StreamMetadata_VorbisComment_Entry *e = &m->data.vorbis_comment.comments[i];
			const char *eq = memchr(e->entry, '=', e->length);

			if (!eq)
				continue;
			{
				char *v = malloc(e->length - (size_t)(eq + 1 - (const char *)e->entry) + 1);
				size_t vl = e->length - (size_t)(eq + 1 - (const char *)e->entry);

				if (!v)
					continue;
				memcpy(v, eq + 1, vl);
				v[vl] = 0;
				meta_add_tag(&r->meta, (const char *)e->entry, (size_t)(eq - (const char *)e->entry), v);
				free(v);
			}
		}
		break;
	case FLAC__METADATA_TYPE_PICTURE: {
		struct audio_picture *pic = realloc(r->meta.pictures, (r->meta.npictures + 1) * sizeof *pic);

		if (!pic)
			break;
		r->meta.pictures = pic;
		pic += r->meta.npictures;
		memset(pic, 0, sizeof *pic);
		pic->type = m->data.picture.type;
		pic->mime = strdup(m->data.picture.mime_type);
		pic->description = strdup((const char *)m->data.picture.description);
		pic->width = m->data.picture.width;
		pic->height = m->data.picture.height;
		pic->depth = m->data.picture.depth;
		pic->colors = m->data.picture.colors;
		pic->size = m->data.picture.data_length;
		pic->data = malloc(pic->size);
		if (pic->data)
			memcpy(pic->data, m->data.picture.data, pic->size);
		r->meta.npictures++;
		break;
	}
	default:
		break;
	}
}

static void flac_error_cb(const FLAC__StreamDecoder *dec, FLAC__StreamDecoderErrorStatus st, void *client)
{
	struct flac_priv *p = client;

	(void)dec; (void)st;
	p->error = 1;
}

static int flac_open(struct audio_reader *r, const char *path)
{
	struct flac_priv *p = calloc(1, sizeof *p);

	if (!p)
		return -1;
	p->reader = r;
	p->dec = FLAC__stream_decoder_new();
	if (!p->dec) {
		free(p);
		return -1;
	}
	FLAC__stream_decoder_set_metadata_respond_all(p->dec);
	if (FLAC__stream_decoder_init_file(p->dec, path, flac_write_cb, flac_meta_cb, flac_error_cb, p)
	    != FLAC__STREAM_DECODER_INIT_STATUS_OK ||
	    !FLAC__stream_decoder_process_until_end_of_metadata(p->dec) || r->channels != 2) {
		FLAC__stream_decoder_delete(p->dec);
		free(p);
		return -1;
	}
	r->format = AUDIO_FLAC;
	r->priv = p;
	return 0;
}

static long flac_read(struct audio_reader *r, int32_t *lr, size_t n)
{
	struct flac_priv *p = r->priv;
	size_t got = 0;

	while (got < n) {
		size_t avail = p->pending_n - p->pending_pos, k;

		if (!avail) {
			if (p->eof || p->error)
				break;
			if (FLAC__stream_decoder_get_state(p->dec) == FLAC__STREAM_DECODER_END_OF_STREAM ||
			    !FLAC__stream_decoder_process_single(p->dec)) {
				p->eof = 1;
				continue;
			}
			if (FLAC__stream_decoder_get_state(p->dec) == FLAC__STREAM_DECODER_END_OF_STREAM)
				p->eof = 1;
			continue;
		}
		k = avail < n - got ? avail : n - got;
		memcpy(lr + 2 * got, p->pending + 2 * p->pending_pos, k * 8);
		p->pending_pos += k;
		got += k;
	}
	return p->error && !got ? -1 : (long)got;
}

static void flac_close(struct audio_reader *r)
{
	struct flac_priv *p = r->priv;

	FLAC__stream_decoder_finish(p->dec);
	FLAC__stream_decoder_delete(p->dec);
	free(p->pending);
	free(p);
}

static int flac_create(struct audio_writer *w, const char *path, unsigned channels, unsigned rate,
		       unsigned bits, const struct audio_meta *meta, uint64_t expected)
{
	struct flac_priv *p = calloc(1, sizeof *p);
	size_t i;

	if (!p)
		return -1;
	p->enc = FLAC__stream_encoder_new();
	if (!p->enc) {
		free(p);
		return -1;
	}
	FLAC__stream_encoder_set_channels(p->enc, channels);
	FLAC__stream_encoder_set_bits_per_sample(p->enc, bits);
	FLAC__stream_encoder_set_sample_rate(p->enc, rate);
	FLAC__stream_encoder_set_compression_level(p->enc, 5);
	FLAC__stream_encoder_set_total_samples_estimate(p->enc, expected);
	if (meta) {
		FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);

		for (i = 0; vc && i < meta->ntags; i++) {
			FLAC__StreamMetadata_VorbisComment_Entry e;

			if (FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&e, meta->tags[i].name,
											    meta->tags[i].value))
				FLAC__metadata_object_vorbiscomment_append_comment(vc, e, false);
		}
		if (vc)
			p->blocks[p->nblocks++] = vc;
		for (i = 0; i < meta->npictures && p->nblocks < 3; i++) {
			const struct audio_picture *pic = &meta->pictures[i];
			FLAC__StreamMetadata *b = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PICTURE);

			if (!b)
				break;
			b->data.picture.type = (FLAC__StreamMetadata_Picture_Type)pic->type;
			FLAC__metadata_object_picture_set_mime_type(b, pic->mime, true);
			FLAC__metadata_object_picture_set_description(b, (FLAC__byte *)pic->description, true);
			b->data.picture.width = pic->width;
			b->data.picture.height = pic->height;
			b->data.picture.depth = pic->depth;
			b->data.picture.colors = pic->colors;
			FLAC__metadata_object_picture_set_data(b, pic->data, (FLAC__uint32)pic->size, true);
			p->blocks[p->nblocks++] = b;
		}
		if (p->nblocks)
			FLAC__stream_encoder_set_metadata(p->enc, p->blocks, p->nblocks);
	}
	if (FLAC__stream_encoder_init_file(p->enc, path, NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
		FLAC__stream_encoder_delete(p->enc);
		free(p);
		return -1;
	}
	w->priv = p;
	w->format = AUDIO_FLAC;
	w->bits = bits;
	return 0;
}

static int flac_write(struct audio_writer *w, const int32_t *lr, size_t n)
{
	struct flac_priv *p = w->priv;
	FLAC__int32 buf[2 * 1024];
	size_t done = 0;

	while (done < n) {
		size_t k = n - done < 1024 ? n - done : 1024, i;

		for (i = 0; i < 2 * k; i++)
			buf[i] = lr[2 * done + i] >> (32 - w->bits);
		if (!FLAC__stream_encoder_process_interleaved(p->enc, buf, (unsigned)k))
			return -1;
		done += k;
	}
	w->frames += n;
	return 0;
}

static int flac_finish(struct audio_writer *w)
{
	struct flac_priv *p = w->priv;
	unsigned i;
	int ok = FLAC__stream_encoder_finish(p->enc);

	FLAC__stream_encoder_delete(p->enc);
	for (i = 0; i < p->nblocks; i++)
		FLAC__metadata_object_delete(p->blocks[i]);
	free(p);
	return ok ? 0 : -1;
}
#endif

/* --- the front ------------------------------------------------------------- */

int audio_open(struct audio_reader *r, const char *path)
{
	enum audio_format fmt = audio_format_of(path);
	FILE *f;
	uint8_t magic[4];

	memset(r, 0, sizeof *r);
	f = fopen(path, "rb");
	if (!f)
		return -1;
	if (fread(magic, 1, 4, f) == 4) {
		if (!memcmp(magic, "fLaC", 4))
			fmt = AUDIO_FLAC;
		else if (!memcmp(magic, "RIFF", 4))
			fmt = AUDIO_WAV;
	}
	rewind(f);
	switch (fmt) {
	case AUDIO_FLAC:
		fclose(f);
#ifdef HAVE_FLAC
		return flac_open(r, path);
#else
		errno = ENOTSUP;
		return -1;
#endif
	case AUDIO_WAV:
		if (wav_open(r, f) < 0) {
			fclose(f);
			return -1;
		}
		return 0;
	case AUDIO_RAW: {
		struct raw_priv *p = calloc(1, sizeof *p);
		long size;

		if (!p) {
			fclose(f);
			return -1;
		}
		p->f = f;
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		rewind(f);
		r->format = AUDIO_RAW;
		r->channels = 2;
		r->rate = 48000;                 /* raw files carry no rate: assume the decoder's */
		r->bits = 24;
		r->frames = size > 0 ? (uint64_t)size / 8 : 0;
		r->priv = p;
		return 0;
	}
	default:
		fclose(f);
		errno = EINVAL;
		return -1;
	}
}

long audio_read(struct audio_reader *r, int32_t *lr, size_t n)
{
	switch (r->format) {
#ifdef HAVE_FLAC
	case AUDIO_FLAC: return flac_read(r, lr, n);
#endif
	case AUDIO_WAV: return wav_read(r, lr, n);
	case AUDIO_RAW: return raw_read(r, lr, n);
	default: return -1;
	}
}

void audio_close(struct audio_reader *r)
{
	switch (r->format) {
#ifdef HAVE_FLAC
	case AUDIO_FLAC: flac_close(r); break;
#endif
	case AUDIO_WAV: {
		struct wav_priv *w = r->priv;

		fclose(w->f);
		free(w->info);
		free(w);
		break;
	}
	case AUDIO_RAW: {
		struct raw_priv *p = r->priv;

		fclose(p->f);
		free(p);
		break;
	}
	default: break;
	}
	audio_meta_free(&r->meta);
	r->priv = NULL;
}

int audio_create(struct audio_writer *w, const char *path, enum audio_format fmt,
		 unsigned channels, unsigned rate, unsigned bits, const struct audio_meta *meta,
		 uint64_t expected_frames)
{
	memset(w, 0, sizeof *w);
	w->channels = channels;
	w->rate = rate;
	w->bits = bits;
	switch (fmt) {
#ifdef HAVE_FLAC
	case AUDIO_FLAC: return flac_create(w, path, channels, rate, bits, meta, expected_frames);
#endif
	case AUDIO_WAV: return wav_create(w, path, channels, rate, bits, meta);
	case AUDIO_RAW: {
		struct raw_priv *p = calloc(1, sizeof *p);

		if (!p)
			return -1;
		p->f = fopen(path, "wb");
		if (!p->f) {
			free(p);
			return -1;
		}
		w->priv = p;
		w->format = AUDIO_RAW;
		return 0;
	}
	default:
		errno = ENOTSUP;
		return -1;
	}
}

int audio_write(struct audio_writer *w, const int32_t *lr, size_t n)
{
	switch (w->format) {
#ifdef HAVE_FLAC
	case AUDIO_FLAC: return flac_write(w, lr, n);
#endif
	case AUDIO_WAV: return wav_write(w, lr, n);
	case AUDIO_RAW: {
		struct raw_priv *p = w->priv;

		w->frames += n;
		return fwrite(lr, 8, n, p->f) == n ? 0 : -1;
	}
	default: return -1;
	}
}

int audio_finish(struct audio_writer *w)
{
	switch (w->format) {
#ifdef HAVE_FLAC
	case AUDIO_FLAC: return flac_finish(w);
#endif
	case AUDIO_WAV: return wav_finish(w);
	case AUDIO_RAW: {
		struct raw_priv *p = w->priv;
		int rc = fclose(p->f);

		free(p);
		return rc;
	}
	default: return -1;
	}
}
