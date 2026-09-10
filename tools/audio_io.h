/*
 * audio_io.h -- reading and writing stereo audio files for mqad: FLAC
 * (through libFLAC, when built with it) and WAV. Samples are handled as
 * the decoder wants them: 32-bit words holding the 24-bit sample in the
 * top 24 bits.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQAD_AUDIO_IO_H
#define MQAD_AUDIO_IO_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

enum audio_format { AUDIO_UNKNOWN, AUDIO_FLAC, AUDIO_WAV, AUDIO_RAW };

/* A tag: name=value pairs, as Vorbis comments carry them. */
struct audio_tag { char *name, *value; };

struct audio_picture {                 /* a FLAC PICTURE block, verbatim */
	uint32_t type;
	char *mime, *description;
	uint32_t width, height, depth, colors;
	uint8_t *data;
	size_t size;
};

struct audio_meta {
	struct audio_tag *tags;
	size_t ntags;
	struct audio_picture *pictures;
	size_t npictures;
};

struct audio_reader {
	enum audio_format format;
	unsigned channels, rate, bits;
	uint64_t frames;                   /* total, 0 if unknown             */
	struct audio_meta meta;
	void *priv;
};

/* Open a file; format from its content / extension. Returns 0 or -1. */
int audio_open(struct audio_reader *r, const char *path);
/* Read up to n frames of interleaved 32-bit samples; returns frames read (0 at end, -1 error). */
long audio_read(struct audio_reader *r, int32_t *lr, size_t n);
void audio_close(struct audio_reader *r);

struct audio_writer {
	enum audio_format format;
	unsigned channels, rate, bits;
	uint64_t frames;
	void *priv;
};

/* Create a file of the given format with the metadata copied over. */
int audio_create(struct audio_writer *w, const char *path, enum audio_format fmt,
		 unsigned channels, unsigned rate, unsigned bits, const struct audio_meta *meta,
		 uint64_t expected_frames);
int audio_write(struct audio_writer *w, const int32_t *lr, size_t n);
int audio_finish(struct audio_writer *w);

void audio_meta_free(struct audio_meta *m);
const char *audio_tag_get(const struct audio_meta *m, const char *name);
enum audio_format audio_format_of(const char *path);
int audio_have_flac(void);

#endif
