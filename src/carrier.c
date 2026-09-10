/*
 * Carrier digit extraction -- see include/mqa/carrier.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mqa/carrier.h"

const struct mqa_carrier_class mqa_carrier_classes[MQA_CARRIER_CLASSES] = {
	{ 1, { 0x00, 0x01, 0xff }, 0x29, 0x00000000u },
	{ 3, { 0x01, 0x05, 0xf2 }, 0x24, 0x55555556u },
	{ 4, { 0x01, 0x04, 0xff }, 0x15, 0x40000000u },
	{ 8, { 0x02, 0x02, 0x3f }, 0x29, 0x20000000u },
};

static uint8_t digit(int32_t sample, unsigned shift, const struct mqa_carrier_class *cls)
{
	uint32_t v = (uint32_t)(sample >> shift) + 0x690000u;
	uint32_t frac = v * cls->recip;              /* fractional part of v / M */

	return (uint8_t)(((uint64_t)frac * cls->levels) >> 32);
}

void mqa_carrier_digits(const int32_t *a, const int32_t *b, unsigned count,
			unsigned shift, const struct mqa_carrier_class *cls,
			uint8_t *digits)
{
	unsigned i;

	shift += 8;
	for (i = 0; i < count; i++) {
		digits[2 * i] = digit(a[i], shift, cls);
		digits[2 * i + 1] = digit(b[i], shift, cls);
	}
}
