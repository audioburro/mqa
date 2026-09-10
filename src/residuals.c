/*
 * Per-channel residual production -- see include/mqa/residuals.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mqa/residuals.h"

void mqa_residuals_attach(struct mqa_residual_decoder *r, struct mqa_range_decoder *coder)
{
	int i;

	for (i = 0; i < MQA_RESIDUAL_RECORDS; i++) {
		r->record[i].coder = coder;
		r->record[i].residual = coder;
	}
}

void mqa_residuals_decode(struct mqa_residual_decoder *r,
			  int32_t out[MQA_PREDICTOR_OUTPUTS], unsigned count, int with_residual)
{
	int32_t rec[MQA_RESIDUAL_RECORDS][MQA_PREDICTOR_SYMBOLS];
	unsigned i;

	for (i = 0; i < MQA_RESIDUAL_RECORDS; i++) {
		if (i < r->records)
			mqa_entropy_decode(&r->record[i], rec[i], count, with_residual);
		else
			memset(rec[i], 0, sizeof rec[i]);
	}

	mqa_predictor_run(&r->predictor, rec[0], rec[1], out, count);
}
