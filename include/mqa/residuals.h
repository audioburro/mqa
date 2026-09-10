/*
 * mqa/residuals.h -- per-channel residual production: two entropy
 * decoders feeding one predictor.
 *
 * For each channel and each group of 16 carrier samples the decoder runs
 * two entropy decoder instances (entropy.h), each producing a 16-symbol
 * record, then feeds both records to that channel's predictor
 * (predictor.h) to obtain the 32 residuals the reconstruction filter
 * consumes as P (left) or Q (right). All entropy decoders in a stream
 * share one range coder and one byte ring.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MQA_DECODE_RESIDUALS_H
#define MQA_DECODE_RESIDUALS_H

#include <stdint.h>
#include "mqa/entropy.h"
#include "mqa/predictor.h"

#define MQA_RESIDUAL_RECORDS 2

struct mqa_residual_decoder {
	unsigned records;             /* how many decoders run (0..2); the rest  */
	                              /* contribute all-zero records            */
	struct mqa_entropy_decoder record[MQA_RESIDUAL_RECORDS];
	struct mqa_predictor predictor;
};

/*
 * Point both entropy decoders at the shared coder. Their other fields
 * (adaptation parameters, tables of state) are the caller's to set up,
 * as is the predictor (mqa_predictor_init).
 */
void mqa_residuals_attach(struct mqa_residual_decoder *r, struct mqa_range_decoder *coder);

/* Produce the next 2 * count residuals for this channel (count symbols
 * per record; 16 for a full group). */
void mqa_residuals_decode(struct mqa_residual_decoder *r,
			  int32_t out[MQA_PREDICTOR_OUTPUTS], unsigned count, int with_residual);

#endif
