/*
 * mqa/stream_decoder.h -- the whole decoder on a stream of frames: the
 * intake in front of the group decoder, with the carrier rings fed the
 * way the reference decoder's read loop feeds them.
 *
 *   struct mqa_stream_decoder sd;
 *   mqa_stream_decoder_init(&sd);
 *   while (frames left) {
 *           n = mqa_stream_decoder_feed(&sd, lr, count);   // takes what fits
 *           produced = mqa_stream_decoder_run(&sd, out, out_capacity);
 *           ... write `produced` output frames ...
 *   }
 *   mqa_stream_decoder_finish(&sd) ... run/collect until it returns 0
 *
 * Frames are stereo, interleaved, 32-bit words with the 24-bit sample in
 * the top bits; output comes at twice the rate in the same form.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_STREAM_DECODER_H
#define MQA_DECODE_STREAM_DECODER_H

#include "mqa/decoder.h"
#include "mqa/intake.h"
#include "mqa/watermark.h"

/* The most output samples per channel one group can produce. */
#define MQA_STREAM_DECODER_GROUP_MAX (MQA_RING_WORDS + 2 * MQA_GROUP + 64)

struct mqa_stream_decoder {
	struct mqa_decoder dec;
	struct mqa_intake in;
	int finishing;                    /* no more input is coming            */
	unsigned long groups;             /* groups decoded                     */
	unsigned long declined;           /* groups the decoder could not do    */
	/*
	 * One call of the decoder writes at most a group's output pair
	 * per carrier sample, plus the dither and flush the alternative
	 * kernel needs past it, but a group with no packet in it, or the
	 * flush at the end of a file, passes samples straight through, and
	 * that can be as much as the carrier ring holds.
	 */
	int32_t out_l[MQA_STREAM_DECODER_GROUP_MAX], out_r[MQA_STREAM_DECODER_GROUP_MAX];

	/* The renderer signalling the vendor's decoder embeds in its output
	 * (watermark.h): off unless the owner asks for it. With it on the
	 * output matches the vendor's delivered files bit for bit. */
	int signalling;
	struct mqa_watermark watermark;

	/* optional hooks around each group, for tools that look inside:
	 * before it runs, and after it with the output samples (24-bit) */
	void (*before_group)(void *user, struct mqa_stream_decoder *sd);
	void (*after_group)(void *user, struct mqa_stream_decoder *sd, const int32_t *l, const int32_t *r, unsigned n);
	void *user;
};

void mqa_stream_decoder_init(struct mqa_stream_decoder *sd, unsigned rate_hz);

/* Embed the renderer signalling in the output (off by default). */
void mqa_stream_decoder_set_signalling(struct mqa_stream_decoder *sd, int on);

/* Append frames to the carrier rings; returns how many were taken. */
size_t mqa_stream_decoder_feed(struct mqa_stream_decoder *sd, const int32_t *lr, size_t n);

/*
 * Decode as many groups as the rings allow, writing interleaved output
 * frames (at most `capacity`); returns the frames written. `capacity`
 * must be at least MQA_STREAM_DECODER_GROUP_MAX or no group can run.
 */
size_t mqa_stream_decoder_run(struct mqa_stream_decoder *sd, int32_t *out, size_t capacity);

/* No more input: further run() calls decode what the rings hold and
 * pass the rest through (the reference's end-of-file flush), until run()
 * returns 0. */
void mqa_stream_decoder_finish(struct mqa_stream_decoder *sd);

/* Frames the rings can take right now. */
unsigned mqa_stream_decoder_space(const struct mqa_stream_decoder *sd);

/* Bookkeeping between groups (run() does this itself): the position
 * wraps and what the feed staged in the tail returns to the head. */
void mqa_stream_decoder_after_group(struct mqa_stream_decoder *sd);

#endif
