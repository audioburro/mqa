/*
 * The residual stage -- see include/mqa/residual_stage.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/residual_stage.h"
#include "mqa/lcg.h"

/* scale by record index: 2^(n/6) shaped */
const uint16_t mqa_residual_scale_table[64] = {
	0, 1, 2, 4, 8, 11, 16, 21, 25, 32, 35, 41, 45, 51, 57, 64, 71, 81, 91, 101,
	115, 128, 143, 161, 181, 203, 229, 256, 287, 323, 363, 407, 457, 512, 575,
	645, 725, 813, 913, 1024, 1149, 1291, 1449, 1625, 1825, 2048, 2299, 2581,
	2897, 3251, 3649, 4096, 4597, 5161, 5793, 6501, 7299, 8192, 9195, 10321,
	11585, 13003, 14597, 16384,
};
/* quarter-octave steps of 256 */
const uint16_t mqa_residual_level_table[4] = { 256, 304, 362, 431 };
const uint16_t mqa_residual_rate_table[4] = { 4176, 4256, 4416, 4704 };

static const struct mqa_gain_params neutral_gain = { 2, 0x40000000, 0 };

/* The two channels' reseed salts. */
static const uint32_t channel_salt[2] = { 0xa1e24bbau, 0xa1e24cbau };

void mqa_gain_from_scale(struct mqa_gain_params *g, int32_t scale)
{
	unsigned k = 0;
	uint32_t m;

	if ((int32_t)g->scale == scale)
		return;
	if (scale <= 0) {
		g->scale = 0;
		return;
	}
	g->scale = (uint32_t)scale;
	for (m = (uint32_t)(scale - 1); m >> (k + 1); k++)
		;
	g->shift = (int)k;
	g->gain = (int32_t)(((uint64_t)1 << (32 + k)) / (uint32_t)(2 * scale));
}

static void set_predictor_gain(struct mqa_predictor *p, const struct mqa_gain_params *g)
{
	p->gain = *g;
	p->noise[0].scale = g->scale;
	p->noise[1].scale = g->scale;
}

void mqa_residual_stage_init(struct mqa_residual_stage *s, struct mqa_byte_ring *ring,
			     const struct mqa_carrier_class *cls)
{
	unsigned c;

	memset(s, 0, sizeof *s);
	s->ring = ring;
	s->cls = cls;
	s->coder.src = ring;
	s->coder.base = 256;
	s->digit_ring.data = s->digit_data;
	s->digit_ring.size = sizeof s->digit_data;
	s->digit_coder.src = &s->digit_ring;
	s->digit_coder.base = 256;
	s->seek = -1;
	s->resync = -1;
	for (c = 0; c < 2; c++)
		mqa_residuals_attach(&s->ch[c], &s->coder);
}

void mqa_residual_stage_configure(struct mqa_residual_stage *s, unsigned index0,
				  unsigned index1, int32_t start,
				  const uint8_t *part_a, unsigned a_len)
{
	s->index[0] = (uint8_t)index0;
	s->index[1] = (uint8_t)index1;
	s->seek = start;
	s->fresh = 1;
	if (s->mode != 0)
		return;
	s->mode = 1;
	if (a_len == 8 && s->counter == 0) {
		/* the record's part A seeds the predictors' symbol memories */
		int32_t k = (int32_t)s->record.scale;
		unsigned c;

		for (c = 0; c < 2; c++) {
			const uint8_t *a = part_a + 4 * c;

			s->ch[c].predictor.sym_prev[0] = k * (int16_t)(a[0] | a[1] << 8);
			s->ch[c].predictor.sym_prev[1] = k * (int16_t)(a[2] | a[3] << 8);
		}
	}
}

void mqa_residual_stage_reset(struct mqa_residual_stage *s)
{
	unsigned c, i;

	s->mode = 0;
	for (c = 0; c < 2; c++) {
		s->ch[c].records = s->cls->param[0];
		for (i = 0; i < MQA_RESIDUAL_RECORDS; i++)
			s->ch[c].record[i].gain.scale = 0;
		set_predictor_gain(&s->ch[c].predictor, &neutral_gain);
	}
}

/* --- packet start --------------------------------------------------------- */

/* One entropy decoder's initial adaptation state. */
static void decoder_initial(struct mqa_entropy_decoder *d, uint32_t scale2)
{
	static const uint16_t decay[MQA_ENTROPY_TAPS] = { 0xf000, 0xe100, 0xd2f0, 0xc5c1 };

	d->level = 0xe6;
	d->level_max = 0xe6;
	d->level_min = 0x39;
	d->level_rate = 0x108f;
	d->variance = 0x10000000;
	d->gain.scale = 0;
	d->scale2 = scale2;
	d->head = 4;
	memset(d->history, 0, sizeof d->history);
	memset(d->coef, 0, sizeof d->coef);
	memset(d->taps, 0, sizeof d->taps);
	memset(d->pred_a, 0, sizeof d->pred_a);
	memset(d->pred_b, 0, sizeof d->pred_b);
	memcpy(d->decay, decay, sizeof d->decay);
}

static void channel_initial(struct mqa_residual_decoder *ch, uint32_t scale2)
{
	unsigned i;

	for (i = 0; i < MQA_RESIDUAL_RECORDS; i++)
		decoder_initial(&ch->record[i], scale2);
	ch->predictor.sym_prev[0] = 0;
	ch->predictor.sym_prev[1] = 0;
	ch->predictor.y1_prev = 0;
	ch->predictor.y2h_prev = 0;
}

/* --- the carrier-digit FIFO --------------------------------------------- */

static uint32_t window(const struct mqa_residual_stage *s);

/*
 * One byte into the FIFO. It holds at most `size - 1` bytes, so a write
 * into a full one drops the oldest: the read position moves up with the
 * write position, which is what keeps the window a fixed depth behind
 * the carrier once the stream is running.
 */
static void fifo_put(struct mqa_residual_stage *s, uint32_t byte)
{
	struct mqa_byte_ring *f = &s->digit_ring;

	if (window(s) + 1 >= f->size)
		f->cursor = f->cursor + 1 >= f->size ? 0 : f->cursor + 1;
	f->data[f->wpos] = (uint8_t)byte;
	f->wpos = f->wpos + 1 >= f->size ? 0 : f->wpos + 1;
}

/*
 * Pack carrier digits into the FIFO: `param[1]` of them make one byte,
 * most significant digit first, so a byte is a digit of radix
 * `param[2] + 1`. A group rarely ends on a byte boundary, so the part
 * that is left over stays in `digit_carry` with `digit_phase` saying how
 * many more digits it wants.
 */
static void push_digits(struct mqa_residual_stage *s, const uint8_t *digits, unsigned count)
{
	unsigned levels = s->cls->levels, per = s->cls->param[1];
	uint32_t carry = s->digit_carry;
	unsigned i, want = s->digit_phase;

	if (per < 2 || count == 0)
		return;
	if (want > 0 && want <= count) {
		for (i = 0; i < want; i++)
			carry = carry * levels + digits[i];
		fifo_put(s, carry);
		digits += want;
		count -= want;
	}
	while (count > per - 1) {
		carry = 0;
		for (i = 0; i < per; i++)
			carry = carry * levels + digits[i];
		fifo_put(s, carry);
		digits += per;
		count -= per;
	}
	carry = 0;
	for (i = 0; i < count; i++)
		carry = carry * levels + digits[i];
	s->digit_carry = carry;
	s->digit_phase = per - count;
}

/* Reset everything the decoders adapt, keeping the configuration. */
static void restart(struct mqa_residual_stage *s, int with_digits, const uint8_t *digits)
{
	unsigned c;

	mqa_residual_stage_reset(s);
	if (with_digits) {
		uint32_t period = s->cls->param[1];
		int32_t phase = (int32_t)period - 2 * (int32_t)(period ? s->counter % period : 0);

		s->digit_carry = 0;
		s->digit_phase = (uint32_t)(phase <= 0 ? phase + (int32_t)period : phase);
		if (s->cls->param[0] && digits)
			push_digits(s, digits, MQA_RESIDUAL_GROUP);
	}
	for (c = 0; c < 2; c++)
		channel_initial(&s->ch[c], s->record.scale);
}

int mqa_residual_stage_setup(struct mqa_residual_stage *s, const struct mqa_carrier_class *cls,
			     unsigned scale_index, uint32_t kernel_param, uint32_t shift)
{
	unsigned c, i;

	s->cls = cls;
	s->resync_enable = (int)(cls - mqa_carrier_classes);   /* the class index, as the reference keeps it */
	s->scale_index = scale_index;
	s->record_index = scale_index;
	s->kernel_param = kernel_param;
	s->shift = shift;
	mqa_gain_from_scale(&s->record, mqa_residual_scale_table[scale_index & 63]);
	/* a byte of the digit FIFO is one symbol of this radix */
	s->digit_coder.base = (uint32_t)cls->param[2] + 1;
	for (c = 0; c < 2; c++) {
		s->ch[c].records = cls->param[0];
		for (i = 0; i < MQA_RESIDUAL_RECORDS; i++) {
			/* the leading records of each channel read the carrier's
			 * own digits; every record's residuals come from the
			 * data channel */
			s->ch[c].record[i].coder = i < cls->param[0] ? &s->digit_coder : &s->coder;
			s->ch[c].record[i].residual = &s->coder;
		}
	}
	s->fresh = 0;
	s->resync = -1;
	s->unsupported = 0;
	return 0;
}

void mqa_residual_stage_start(struct mqa_residual_stage *s, int with_digits, const uint8_t *digits)
{
	s->resync = -1;
	s->active = 1;
	s->digit_ring.cursor = 0;
	s->counter = 0;
	s->digit_ring.wpos = 0;
	restart(s, with_digits, digits);
}

void mqa_residual_stage_position(struct mqa_residual_stage *s, uint32_t counter, uint32_t limit,
				 unsigned resync_mode, int with_digits, const uint8_t *digits)
{
	s->counter = counter;
	s->limit = limit;
	s->resync = (int32_t)(resync_mode & 0xff);
	s->active = 0;
	s->index[0] = 0;
	s->index[1] = 0;
	restart(s, with_digits, digits);
}

void mqa_residual_stage_sync(struct mqa_residual_stage *s, uint32_t position, unsigned mode)
{
	if (s->resync >= 0 || !s->resync_enable)
		return;
	s->resync = (int32_t)mode;
	s->limit = position;
}

/* --- fresh record distribution ---------------------------------------- */

static struct mqa_gain_params record_for(const struct mqa_residual_stage *s, unsigned index)
{
	struct mqa_gain_params g = s->record;

	if (index == s->record_index)
		return g;
	if (index == 0)
		g.scale = 0;
	else
		mqa_gain_from_scale(&g, mqa_residual_scale_table[index & 63]);
	return g;
}

static void distribute(struct mqa_residual_stage *s)
{
	struct mqa_gain_params g0 = record_for(s, s->index[0]), g1 = record_for(s, s->index[1]);
	int both = s->index[0] == s->record_index && s->index[1] == s->record_index;
	unsigned c;

	for (c = 0; c < 2; c++) {
		s->ch[c].record[0].gain = g0;
		s->ch[c].record[1].gain = g1;
		set_predictor_gain(&s->ch[c].predictor, both ? &s->record : &neutral_gain);
	}
	if (s->seek >= 0)
		s->ring->cursor = (unsigned)s->seek;
	s->fresh = 0;
}

/* --- block start -------------------------------------------------------- */

/* One entropy decoder's block header: level and variance. */
int mqa_residual_decoder_block_init(struct mqa_entropy_decoder *d, uint32_t block, int32_t rate)
{
	struct mqa_range_decoder *rc = d->coder;
	unsigned bits = block ? 11 : 7, low;
	uint32_t v;
	int32_t var;
	int status = 0;

	d->level = 0x200;
	d->level_rate = rate;
	mqa_range_normalize(rc);
	v = rc->value & ((1u << bits) - 1);
	rc->range >>= bits;
	rc->value >>= bits;
	low = v & 0x7f;
	if (block) {
		unsigned hi = v >> 7;

		d->level = mqa_residual_level_table[(0u - hi) & 3] >> ((hi + 3) >> 2);
	}
	var = (int32_t)((uint32_t)mqa_residual_level_table[low & 3] << (low >> 2)) >> 8;
	if ((int)low > 0x5b || var >= 2 * d->variance)
		status = rc == d->residual ? MQA_RESIDUAL_RESET : MQA_RESIDUAL_ABORT;
	d->variance = var;
	return status;
}

static int channel_block_init(struct mqa_residual_decoder *ch, int32_t rate,
			      uint32_t salt, uint32_t block)
{
	uint32_t s = salt + block;
	unsigned i;
	int status = 0;

	ch->predictor.noise[0].state = mqa_nr_lcg_step(s * s);
	ch->predictor.noise[1].state = mqa_nr_lcg_step(ch->predictor.noise[0].state);
	for (i = 0; i < ch->records; i++) {
		uint32_t r = s + 32 * (i + 1);

		ch->record[i].rng = r * r;
		status |= mqa_residual_decoder_block_init(&ch->record[i], block, rate);
	}
	return status;
}

/*
 * The stage reads its own fields, the block header, through the
 * first record of the left channel, so for a carrier class whose
 * leading records read the carrier's own digits that is the digit
 * FIFO's coder, and otherwise the data channel's.
 */
static struct mqa_range_decoder *stage_coder(struct mqa_residual_stage *s)
{
	return s->ch[0].record[0].coder;
}

/* The 3-bit block header, then every decoder's. */
static int block_header(struct mqa_residual_stage *s)
{
	struct mqa_range_decoder *rc = stage_coder(s);
	uint32_t v, block = s->counter >> 12;
	int32_t rate;

	if (rc->range == 0) {
		/* a coder that was never started: nothing can be read from
		 * it, and renormalising would not end */
		s->unsupported = 1;
		return 0;
	}
	mqa_range_normalize(rc);
	v = rc->value;
	rc->range >>= 3;
	rc->value >>= 3;
	s->spread = (v & 4) ? (int32_t)(s->cls->word & 0xff) >> 1 : 0;
	s->header_bits = v & 3;
	rate = mqa_residual_rate_table[v & 3];
	return channel_block_init(&s->ch[0], rate, channel_salt[0], block) |
	       channel_block_init(&s->ch[1], rate, channel_salt[1], block);
}

static void block_start(struct mqa_residual_stage *s)
{
	int status;

	s->block_seen = 1;
	s->abort_request = 0;
	/* every block restarts the carrier-digit stream: its coder begins
	 * empty and the first renormalisation fills it from the FIFO */
	s->digit_coder.value = 0;
	s->digit_coder.range = 1;
	if (s->mode == 0) {
		if (s->ch[0].records == 0)
			return;
	} else {
		s->coder.src = s->ring;
		s->coder.value = 0;
		s->coder.range = 1;
		s->reset_request = 0;
		s->mode = 2;
		s->ch[0].records = 2;
		s->ch[1].records = 2;
	}
	status = block_header(s);
	if (status & 2)
		s->active = 0;
	else if (status)
		mqa_residual_stage_reset(s);
}

/* --- the group ---------------------------------------------------------- */

/* Level bound from the carrier class, ramped by the window position. */
int32_t mqa_residual_class_level(const struct mqa_carrier_class *cls, int32_t at)
{
	int32_t w = (int32_t)(cls->word & 0xff);

	if (cls->param[0])
		return 8 * w - ((4 * w * (int32_t)at) >> 8);
	return 4 * w;
}

/* Bytes the digit FIFO holds: its write position minus its read one. */
static uint32_t window(const struct mqa_residual_stage *s)
{
	uint32_t w = s->digit_ring.wpos - s->digit_ring.cursor;

	if (s->digit_ring.wpos < s->digit_ring.cursor)
		w += s->digit_ring.size;
	return w;
}

static void set_bounds(struct mqa_residual_stage *s, uint32_t at)
{
	int32_t base = mqa_residual_class_level(s->cls, (int32_t)at);
	int32_t lo = base - s->spread, hi = base + s->spread;
	unsigned c;

	for (c = 0; c < 2; c++) {
		struct mqa_entropy_decoder *d = s->ch[c].record;

		d[0].level_max = lo;
		d[0].level_min = (int32_t)((uint32_t)lo >> 2);
		d[1].level_max = hi;
		d[1].level_min = (int32_t)((uint32_t)hi >> 2);
		if (s->counter == 0) {
			d[0].level = lo;
			d[1].level = hi;
		}
	}
}

int mqa_residual_stage_group(struct mqa_residual_stage *s, unsigned count, const uint8_t *digits,
			     int32_t p[MQA_RESIDUAL_GROUP], int32_t q[MQA_RESIDUAL_GROUP])
{
	int status = 0;
	uint32_t at;

	if (s->cls->param[0] && digits)
		push_digits(s, digits, 2 * count);

	/* resynchronisation and record distribution happen only at a block
	 * boundary; mid-block, an announced record waits */
	if ((s->counter & (MQA_RESIDUAL_BLOCK - 1)) == 0) {
		if (s->resync >= 0 && s->counter > s->limit) {
			/* resynchronisation: the window's read position is
			 * re-based `resync` behind its fill; a running stage
			 * whose window disagrees is restarted */
			uint32_t back = (uint32_t)s->resync;

			if (s->counter != 0) {
				if (s->active && window(s) != back) {
					s->active = 0;
					restart(s, 0, NULL);
					status = MQA_RESIDUAL_RESYNC;
				}
				s->digit_ring.cursor = s->digit_ring.wpos >= back
					    ? s->digit_ring.wpos - back
					    : s->digit_ring.wpos + s->digit_ring.size - back;
			}
			s->active = 1;
			s->resync = -1;
		}
		if (s->fresh)
			distribute(s);
	}

	if (s->active) {
		at = window(s);
		if ((s->counter & (MQA_RESIDUAL_BLOCK - 1)) == 0)
			block_start(s);
		if (s->unsupported)
			return MQA_RESIDUAL_UNSUPPORTED;
		set_bounds(s, at);
		mqa_residuals_decode(&s->ch[0], p, count / 2, s->mode == 2);
		mqa_residuals_decode(&s->ch[1], q, count / 2, s->mode == 2);
		if (at < window(s)) {
			s->active = 0;
			status = MQA_RESIDUAL_WINDOW;
		}
	}
	if (s->unsupported)
		return MQA_RESIDUAL_UNSUPPORTED;
	if (s->reset_request) {
		mqa_residual_stage_reset(s);
		status = MQA_RESIDUAL_RESET;
	}
	if (s->abort_request) {
		s->active = 0;
		status = MQA_RESIDUAL_ABORT;
	}
	if (s->abort_request || !s->active) {
		memset(p, 0, MQA_RESIDUAL_GROUP * sizeof *p);
		memset(q, 0, MQA_RESIDUAL_GROUP * sizeof *q);
	}
	(void)count;
	s->counter += MQA_RESIDUAL_GROUP;
	return status;
}
