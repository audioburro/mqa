/*
 * spec-tables -- print the constant tables of docs/mqa-stage1-spec.md's
 * appendix A, straight from the library's own definitions, so that the
 * specification and the implementation cannot drift apart.
 *
 *     build/spec-tables > /tmp/appendix.md
 *
 * The output is Markdown, meant to be pasted into the appendix (or
 * diffed against it after a change).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include "mqa/carrier.h"
#include "mqa/entropy.h"
#include "mqa/refine.h"
#include "mqa/reconstruct.h"
#include "mqa/recon2.h"
#include "mqa/residual_stage.h"
#include "mqa/lsb_correction.h"

static void carrier_classes(void)
{
	unsigned i;

	printf("### A.1 Carrier classes\n\n");
	printf("| Class | M (levels) | param[0] | param[1] | param[2] | word | reciprocal |\n");
	printf("| --- | --- | --- | --- | --- | --- | --- |\n");
	for (i = 0; i < MQA_CARRIER_CLASSES; i++) {
		const struct mqa_carrier_class *c = &mqa_carrier_classes[i];

		printf("| %u | %u | %u | %u | %u | 0x%08x | 0x%08x |\n", i, c->levels,
		       c->param[0], c->param[1], c->param[2], c->word, c->recip);
	}
	printf("\n`param[0]` is how many of each channel's two records read the\n"
	       "carrier's digits, `param[1]` how many digits pack into one FIFO\n"
	       "byte, and `param[2] + 1` the radix of the digit coder. `word`'s\n"
	       "low byte is the level-bound base of section 7.4.\n\n");
}

static void entropy_tables(void)
{
	unsigned i;

	printf("### A.6 Entropy coder tables\n\n");
	printf("The 32-bin cumulative distribution:\n\n");
	printf("| Bin | base | threshold | width | reciprocal | weight |\n");
	printf("| --- | --- | --- | --- | --- | --- |\n");
	for (i = 0; i < MQA_ENTROPY_RECORDS; i++) {
		const struct mqa_entropy_record *r = &mqa_entropy_records[i];

		printf("| %u | %d | 0x%08x | %d | 0x%08x | %d |\n", i, r->base,
		       r->threshold, r->width, r->recip, r->weight);
	}
	printf("\nBin selection by the code word's top nibble:\n\n```\n"
	       "    bin_by_nibble[16] = { 8, 10, 12, 13, 14, 14, 15, 15,\n"
	       "                          16, 17, 17, 18, 19, 21, 23, 30 }\n```\n\n");
	printf("Magnitude class by `|x| >> 14`, capped at 31:\n\n```\n"
	       "    class_by_magnitude[32] = {\n"
	       "        0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 9, 9, 10, 10, 11, 11,\n"
	       "        12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14 }\n```\n\n");
	printf("The AR taps' decay weights, and the P/Q predictor's constants:\n\n```\n"
	       "    decay[4] = { 0xf000, 0xe100, 0xd2f0, 0xc5c1 }\n"
	       "    K0 = 0x51147576, K1 = 0x6c1b4748, K2 = 0x1b06d1d2\n```\n\n");

	printf("The level, rate and scale tables of section 7.3:\n\n```\n    level_table[4] = {");
	for (i = 0; i < 4; i++)
		printf("%s %u", i ? "," : "", mqa_residual_level_table[i]);
	printf(" }\n    rate_table[4]  = {");
	for (i = 0; i < 4; i++)
		printf("%s %u", i ? "," : "", mqa_residual_rate_table[i]);
	printf(" }\n    scale_table[64] = {");
	for (i = 0; i < 64; i++)
		printf("%s%s%u", i ? "," : "", i % 10 == 0 ? "\n        " : " ",
		       mqa_residual_scale_table[i]);
	printf(" }\n```\n\n");
	printf("A record's scale is `scale_table[index & 63]`, from which its gain\n"
	       "record follows by the recipe of section 8.1.\n\n");
}

static void refine_tables(void)
{
	unsigned s, i;

	printf("### A.4 Refinement coefficient sets\n\n");
	printf("Four sets; a stream selects one with `(kernel_param & 7) * 3 +\n"
	       "(kernel_param >> 3)`, clamped to 3. `taps` is the FIR's pair\n"
	       "count, so it has `2 * taps + 2` coefficients; `shift` is the\n"
	       "coarse symbol's width in bits.\n\n");
	for (s = 0; s < MQA_REFINE_COEF_SETS; s++) {
		const struct mqa_refine_coef_set *set = &mqa_refine_coef_sets[s];

		printf("Set %u (taps %u, shift %d):\n\n```\n   ", s, set->taps, set->shift);
		for (i = 0; i < 2 * set->taps + 2; i++)
			printf(" %d%s", set->coef[i], i + 1 < 2 * set->taps + 2 ? "," : "");
		printf("\n```\n\n");
	}
}

static void recon_tables(void)
{
	unsigned i;

	printf("### A.7 Reconstruction coefficients\n\n");
	printf("One table of 26 Q31 coefficients. The short filter of section 8.2\n"
	       "uses entries 0..3 as `c0..c3`; the alternative kernel of section\n"
	       "8.3 uses 4..25 as `c4..c25`.\n\n```\n");
	for (i = 0; i < 26; i++)
		printf("%s0x%08x,%s", i % 5 == 0 ? "    " : " ", mqa_recon_coeff_table[i],
		       i % 5 == 4 ? "\n" : "");
	printf("\n```\n\nThe alternative kernel's three shaping integers, one triple per\n"
	       "stream parameter set:\n\n```\n");
	for (i = 0; i < MQA_RECON2_SHAPES; i++)
		printf("    set %u: %d, %d, %d\n", i, mqa_recon2_shape_table[i][0],
		       mqa_recon2_shape_table[i][1], mqa_recon2_shape_table[i][2]);
	printf("```\n\n");
}

static void lsb_tables(void)
{
	unsigned i;

	printf("### A.9 Output-side tables\n\n");
	printf("The LSB correction's 16 (dL, dR) pairs, indexed by\n"
	       "`(scrambler & 0xe) | ((L ^ R ^ priming_bit) & 1)`:\n\n```\n   ");
	for (i = 0; i < 16; i++)
		printf(" {%d,%d}%s", mqa_lsb_correction_table[i][0],
		       mqa_lsb_correction_table[i][1], i < 15 ? "," : "");
	printf("\n```\n\nIts scrambler is a 24-bit register with reflected polynomial\n"
	       "0x8100c9, seeded 0xffffff and clocked with a zero byte per sample.\n"
	       "The renderer signalling of section 11 clocks the same register.\n\n");
	printf("The signalling's nudge table, sixteen (left, right) pairs:\n\n```\n"
	       "     0,  0,   0,  1,   0,  0,   1,  0,\n"
	       "     0,  0,   0, -1,   0,  0,  -1,  0,\n"
	       "     1,  1,   0,  1,   1, -1,   1,  0,\n"
	       "    -1, -1,   0, -1,  -1,  1,  -1,  0\n```\n\n");
}

int main(void)
{
	printf("<!-- generated by tools/spec-tables.c; do not edit by hand -->\n\n");
	carrier_classes();
	refine_tables();
	entropy_tables();
	recon_tables();
	lsb_tables();
	return 0;
}
