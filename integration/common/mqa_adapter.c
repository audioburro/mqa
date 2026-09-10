/*
 * mqa_adapter -- see mqa_adapter.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include "mqa_adapter.h"

#define DEFAULT_SNIFF_SECONDS 5

/* --- the output queue ---------------------------------------------------- */

static int queue_room(struct mqa_adapter *a, size_t frames)
{
	size_t need = a->tail + frames;
	int32_t *grown;

	if (need <= a->cap)
		return 0;
	if (a->head) {                        /* reclaim what has been read */
		memmove(a->queue, a->queue + 2 * a->head, 2 * (a->tail - a->head) * sizeof *a->queue);
		a->tail -= a->head;
		a->head = 0;
		if (a->tail + frames <= a->cap)
			return 0;
		need = a->tail + frames;
	}
	while (a->cap < need)
		a->cap = a->cap ? a->cap * 2 : 8192;
	grown = realloc(a->queue, 2 * a->cap * sizeof *grown);
	if (!grown) {
		a->failed = 1;
		return -1;
	}
	a->queue = grown;
	return 0;
}

static void queue_write(struct mqa_adapter *a, const int32_t *frames, size_t n)
{
	if (queue_room(a, n) < 0)
		return;
	memcpy(a->queue + 2 * a->tail, frames, 2 * n * sizeof *frames);
	a->tail += n;
}

/* Each frame twice: what keeps a passed-through run in time with a
 * doubled output rate. */
static void queue_write_held(struct mqa_adapter *a, const int32_t *frames, size_t n)
{
	size_t i;

	if (queue_room(a, 2 * n) < 0)
		return;
	for (i = 0; i < n; i++) {
		int32_t *at = a->queue + 2 * (a->tail + 2 * i);

		at[0] = at[2] = frames[2 * i];
		at[1] = at[3] = frames[2 * i + 1];
	}
	a->tail += 2 * n;
}

/*
 * While the adapter was deciding, the decoder may have passed frames
 * through *and* gone on to decode some: a stream that opens at the
 * first frame is found within the sniffing window, not before it. Only
 * the frames it passed through are the run-in the policy acts on; the
 * decoded ones behind them are already at the doubled rate and must be
 * left exactly as they are.
 */
static void queue_apply_policy(struct mqa_adapter *a)
{
	size_t held = a->tail - a->head;
	size_t run_in = a->run_in < held ? a->run_in : held;
	size_t rest = held - run_in;
	int32_t *from;
	size_t i;

	a->run_in = 0;
	if (a->policy == MQA_ADAPTER_RAW || run_in == 0)
		return;
	from = a->queue + 2 * a->head;
	if (a->policy == MQA_ADAPTER_DROP) {
		memmove(from, from + 2 * run_in, 2 * rest * sizeof *from);
		a->tail = a->head + rest;
		return;
	}
	if (queue_room(a, run_in) < 0)        /* room for the copies */
		return;
	from = a->queue + 2 * a->head;        /* the queue may have moved */
	memmove(from + 4 * run_in, from + 2 * run_in, 2 * rest * sizeof *from);
	for (i = run_in; i-- > 0; ) {
		int32_t l = from[2 * i], r = from[2 * i + 1];

		from[4 * i] = from[4 * i + 2] = l;
		from[4 * i + 1] = from[4 * i + 3] = r;
	}
	a->tail = a->head + 2 * run_in + rest;
}

/* --- the decoder --------------------------------------------------------- */

/*
 * One group per call: with exactly this capacity the stream decoder runs
 * a single group, and a group either decodes or passes through, never
 * both -- which is what lets the policy above be applied to the right
 * frames.
 */
static void drain_decoder(struct mqa_adapter *a)
{
	static const size_t one_group = MQA_STREAM_DECODER_GROUP_MAX;
	int32_t staging[2 * MQA_STREAM_DECODER_GROUP_MAX];

	for (;;) {
		unsigned long before = a->sd.dec.passed;
		size_t produced = mqa_stream_decoder_run(&a->sd, staging, one_group);

		if (produced == 0)
			return;
		if (a->sd.dec.passed == before || a->state != MQA_ADAPTER_DECODING) {
			if (a->sd.dec.passed != before && a->state == MQA_ADAPTER_SNIFFING)
				a->run_in += produced;
			queue_write(a, staging, produced);
		}
		else if (a->policy == MQA_ADAPTER_HOLD)
			queue_write_held(a, staging, produced);
		else if (a->policy == MQA_ADAPTER_RAW)
			queue_write(a, staging, produced);
		/* else MQA_ADAPTER_DROP: the frames go nowhere */
	}
}

/* The control channel is found the moment its magic turns up. */
static int stream_found(const struct mqa_adapter *a)
{
	return a->sd.in.bs.xbit >= 0;
}

static void decide(struct mqa_adapter *a)
{
	if (a->state != MQA_ADAPTER_SNIFFING)
		return;
	if (stream_found(a)) {
		a->state = MQA_ADAPTER_DECODING;
		queue_apply_policy(a);
	} else if (a->fed >= a->sniff_frames) {
		a->state = MQA_ADAPTER_PLAIN;
	}
}

/* --- the interface ------------------------------------------------------- */

int mqa_adapter_init(struct mqa_adapter *a, unsigned rate_hz, unsigned sniff_frames)
{
	memset(a, 0, sizeof *a);
	a->rate_hz = rate_hz;
	a->sniff_frames = sniff_frames ? sniff_frames : rate_hz * DEFAULT_SNIFF_SECONDS;
	a->policy = MQA_ADAPTER_HOLD;
	mqa_stream_decoder_init(&a->sd, rate_hz);
	return 0;
}

void mqa_adapter_clear(struct mqa_adapter *a)
{
	free(a->queue);
	a->queue = NULL;
	a->cap = a->head = a->tail = 0;
}

void mqa_adapter_reset(struct mqa_adapter *a)
{
	unsigned rate = a->rate_hz, sniff = a->sniff_frames;
	enum mqa_adapter_passthrough policy = a->policy;
	int signalling = a->sd.signalling;
	int32_t *queue = a->queue;
	size_t cap = a->cap;

	memset(a, 0, sizeof *a);
	a->rate_hz = rate;
	a->sniff_frames = sniff;
	a->policy = policy;
	a->queue = queue;
	a->cap = cap;
	mqa_stream_decoder_init(&a->sd, rate);
	if (signalling)
		mqa_stream_decoder_set_signalling(&a->sd, 1);
}

void mqa_adapter_set_signalling(struct mqa_adapter *a, int on)
{
	mqa_stream_decoder_set_signalling(&a->sd, on);
}

void mqa_adapter_set_passthrough(struct mqa_adapter *a, enum mqa_adapter_passthrough p)
{
	a->policy = p;
}

int mqa_adapter_push(struct mqa_adapter *a, const int32_t *frames, size_t n)
{
	while (n) {
		unsigned space = mqa_stream_decoder_space(&a->sd);
		size_t took;

		if (space == 0) {
			drain_decoder(a);
			space = mqa_stream_decoder_space(&a->sd);
			if (space == 0)
				return -1;    /* the decoder is stuck: it declined a group */
		}
		took = mqa_stream_decoder_feed(&a->sd, frames, n < space ? n : space);
		if (took == 0)
			return -1;
		a->fed += took;
		frames += 2 * took;
		n -= took;
		drain_decoder(a);
		decide(a);
	}
	decide(a);
	return a->failed ? -1 : 0;
}

void mqa_adapter_drain(struct mqa_adapter *a)
{
	mqa_stream_decoder_finish(&a->sd);
	/* the run-in never became a stream: what was held is all there is */
	if (a->state == MQA_ADAPTER_SNIFFING)
		a->state = stream_found(a) ? MQA_ADAPTER_DECODING : MQA_ADAPTER_PLAIN;
	drain_decoder(a);
}

size_t mqa_adapter_available(const struct mqa_adapter *a)
{
	return a->state == MQA_ADAPTER_SNIFFING ? 0 : a->tail - a->head;
}

size_t mqa_adapter_pull(struct mqa_adapter *a, int32_t *out, size_t capacity)
{
	size_t have = mqa_adapter_available(a);

	if (have > capacity)
		have = capacity;
	if (have == 0)
		return 0;
	memcpy(out, a->queue + 2 * a->head, 2 * have * sizeof *out);
	a->head += have;
	if (a->head == a->tail)
		a->head = a->tail = 0;
	return have;
}

void mqa_adapter_force(struct mqa_adapter *a, enum mqa_adapter_state state)
{
	if (a->state == state)
		return;
	a->state = state;
	if (state == MQA_ADAPTER_DECODING)
		queue_apply_policy(a);
}

enum mqa_adapter_state mqa_adapter_state(const struct mqa_adapter *a)
{
	return a->state;
}

unsigned mqa_adapter_output_rate(const struct mqa_adapter *a)
{
	return a->state == MQA_ADAPTER_DECODING ? a->rate_hz * 2 : a->rate_hz;
}

unsigned mqa_adapter_latency_frames(const struct mqa_adapter *a)
{
	(void)a;
	/*
	 * A group is decoded only once the rings hold it, the intake's 480
	 * frames of lookahead and one more group (mqa_stream_decoder_run's
	 * own condition), and the decoder primes on the group before it.
	 */
	return MQA_GROUP + MQA_INTAKE_LOOKAHEAD + 2 * MQA_GROUP;
}
