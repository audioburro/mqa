/*
 * mqa_adapter -- what a media framework needs around mqa/stream_decoder.h.
 *
 * The library decodes; a filter in a media pipeline has three further
 * problems, and they are the same three in GStreamer, VLC and FFmpeg, so
 * they are solved once here:
 *
 *   1. *Is this MQA at all?* Nothing in a file says so: the stream is
 *      recognised only when its magic turns up in the carrier's low
 *      bits, which on real material can be a second or more in. The
 *      adapter therefore starts in SNIFFING, holds what the decoder
 *      gives it, and settles into DECODING or PLAIN once it knows --
 *      after at most `sniff_frames`, five seconds of audio by default.
 *
 *   2. *The output rate doubles.* A host has to negotiate a format
 *      before it can push anything, so it must ask (mqa_adapter_state)
 *      before it answers. In DECODING the adapter guarantees two output
 *      frames per input frame, which keeps a filter's timestamps
 *      trivial; in PLAIN it is one for one. With the second unfold on
 *      (mqa_adapter_set_render) it is four or eight per frame.
 *
 *   3. *The decoder passes some frames through.* The run-in before a
 *      stream opens, and anything after one ends, comes back unfolded --
 *      one frame per frame, which would run fast in a doubled stream.
 *      With `hold` set (the default) the adapter repeats each of those
 *      frames instead, so the two-for-one rule holds through them; it
 *      can drop them instead, or emit them once as the reference decoder
 *      does, which is bit-exact but runs that stretch at double speed.
 *
 * Samples are interleaved stereo int32, 24-bit values left-justified
 * (that is, the 24-bit sample shifted up by 8), the same convention
 * the library uses.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_ADAPTER_H
#define MQA_ADAPTER_H

#include <stddef.h>
#include <stdint.h>
#include "mqa/stream_decoder.h"

enum mqa_adapter_state {
	MQA_ADAPTER_SNIFFING,   /* still deciding: pull returns nothing      */
	MQA_ADAPTER_DECODING,   /* MQA: two output frames per input frame    */
	MQA_ADAPTER_PLAIN       /* not MQA: frames pass through unchanged    */
};

enum mqa_adapter_passthrough {
	MQA_ADAPTER_HOLD = 0,   /* repeat passed-through frames (keeps time) */
	MQA_ADAPTER_DROP,       /* discard them (loses the run-in)           */
	MQA_ADAPTER_RAW         /* emit them once, as the reference does     */
};

/* mqa_adapter_set_render's ratio: to the original rate, once known */
#define MQA_ADAPTER_RENDER_ORIGINAL 1

struct mqa_adapter {
	struct mqa_stream_decoder sd;
	enum mqa_adapter_state state;
	enum mqa_adapter_passthrough policy;
	unsigned rate_hz;
	unsigned render;               /* asked for: 0 off, 1 original, 2, 4 */
	unsigned render_ratio;         /* in force: 1, 2 or 4                */
	int requantise;
	unsigned sniff_frames;         /* input frames to wait for a stream  */
	unsigned long fed;             /* input frames given to the decoder  */
	unsigned long passed_seen;     /* passed-through frames accounted for */
	size_t run_in;                 /* queued frames the decoder passed
					* through before it found the stream:
					* only these are the run-in the policy
					* acts on, and they are always at the
					* queue's head                        */
	int32_t *queue;                /* interleaved output frames          */
	size_t cap, head, tail;        /* in frames                          */
	int failed;                    /* out of memory                      */
};

/*
 * `rate_hz` is the carrier's rate (44100 or 48000). `sniff_frames` of 0
 * asks for the default, five seconds. Returns 0, or -1 if the adapter
 * could not allocate.
 */
int mqa_adapter_init(struct mqa_adapter *a, unsigned rate_hz, unsigned sniff_frames);

/* Free what init allocated. */
void mqa_adapter_clear(struct mqa_adapter *a);

/* Back to the state a fresh adapter is in: for a seek, or a flush. */
void mqa_adapter_reset(struct mqa_adapter *a);

/* Embed the renderer signalling in the output (off by default). */
void mqa_adapter_set_signalling(struct mqa_adapter *a, int on);

/* What to do with frames the decoder passes through. */
void mqa_adapter_set_passthrough(struct mqa_adapter *a, enum mqa_adapter_passthrough p);

/*
 * The second unfold as well: `ratio` is 0 for off, 2 or 4 for a fixed
 * ratio on top of the doubling, or MQA_ADAPTER_RENDER_ORIGINAL for
 * whatever ratio reaches the stream's original rate (2 or 4; 1 when the
 * first unfold already reaches it). With `requantise` the rendered
 * output is requantised as a renderer's would be. Call before pushing.
 */
void mqa_adapter_set_render(struct mqa_adapter *a, unsigned ratio, int requantise);

/* Give it `n` interleaved stereo frames. Returns 0, or -1 on failure. */
int mqa_adapter_push(struct mqa_adapter *a, const int32_t *frames, size_t n);

/*
 * Take up to `capacity` output frames. Returns how many were written;
 * zero while the adapter is still sniffing, or when it has nothing
 * ready. Call it until it returns 0.
 */
size_t mqa_adapter_pull(struct mqa_adapter *a, int32_t *out, size_t capacity);

/* No more input: decode what is held and let pull() finish the stream. */
void mqa_adapter_drain(struct mqa_adapter *a);

/* Frames ready to be pulled. */
size_t mqa_adapter_available(const struct mqa_adapter *a);

/*
 * The adapter's decision. A host that has to negotiate a format should
 * push until this is no longer MQA_ADAPTER_SNIFFING; from then on the
 * ratio does not change, two output frames per input frame while
 * decoding, one for one otherwise, give or take the last group of a
 * file, which cannot be completed and is dropped.
 */
enum mqa_adapter_state mqa_adapter_state(const struct mqa_adapter *a);

/*
 * Settle the adapter's decision without sniffing for it. A host that
 * seeks within a file it has already identified resets the adapter and
 * then forces the state back, so that its negotiated output format
 * still holds.
 */
void mqa_adapter_force(struct mqa_adapter *a, enum mqa_adapter_state state);

/*
 * The output rate the state implies, in Hz: twice the carrier's while
 * decoding (times the render ratio in force), the carrier's own
 * otherwise. Meaningless while sniffing.
 */
unsigned mqa_adapter_output_rate(const struct mqa_adapter *a);

/*
 * Input frames the decoder holds before its first output: the filter's
 * latency, in input frames. Constant for the life of a stream.
 */
unsigned mqa_adapter_latency_frames(const struct mqa_adapter *a);

#endif
