/* Small helpers shared by the test programs. */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_fails;

#define CHECK_EQ(what, got, want) do {                                        \
	long long g_ = (long long)(got), w_ = (long long)(want);              \
	if (g_ != w_) {                                                       \
		printf("  MISMATCH %s: got=%lld want=%lld\n", what, g_, w_);  \
		test_fails++;                                                 \
	}                                                                     \
} while (0)

/* A small deterministic PRNG for differential tests. */
static inline uint32_t xorshift(uint32_t *s)
{
	uint32_t x = *s;

	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	return *s = x;
}

static inline int test_result(const char *name, const char *summary)
{
	if (test_fails == 0)
		printf("%s: %s\n", name, summary);
	else
		printf("%s: %d MISMATCHES\n", name, test_fails);
	return test_fails != 0;
}

#endif
