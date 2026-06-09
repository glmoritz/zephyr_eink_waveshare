/*
 * Deterministic frame pattern self-test — see pattern_check.h.
 * Keep pattern_pixel()/pattern_expected_byte() byte-identical to
 * eink_llss/app/frame_converter.py pattern_framebuffer_1bpp().
 */
#include "pattern_check.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pattern, LOG_LEVEL_INF);

/* 1 = white, 0 = black. Integer-only so C and Python agree exactly. */
static int pattern_pixel(int x, int y)
{
	if (x == 0 || x == PATTERN_W - 1 || y == 0 || y == PATTERN_H - 1) {
		return 1; /* border */
	}
	if (x < 64 && y < 64) {
		return 1; /* solid top-left square */
	}
	if (y < 24) {
		return 1; /* thick top stripe */
	}
	if (y == (x * (PATTERN_H - 1)) / (PATTERN_W - 1)) {
		return 1; /* TL->BR diagonal */
	}
	if ((y % 40) == 0) {
		return 1; /* horizontal ruler ticks */
	}
	return 0;
}

uint8_t pattern_expected_byte(size_t i)
{
	int y = (int)(i / PATTERN_STRIDE);
	int b = (int)(i % PATTERN_STRIDE);
	uint8_t v = 0;

	for (int k = 0; k < 8; k++) {
		int x = b * 8 + k;

		if (pattern_pixel(x, y)) {
			v |= (uint8_t)(1u << (7 - k)); /* MSB = leftmost pixel */
		}
	}
	return v;
}

int pattern_verify(const uint8_t *buf, size_t len, const char *stage)
{
	if (len != PATTERN_BYTES) {
		LOG_ERR("%s: length %zu != expected %d", stage, len, PATTERN_BYTES);
		/* still scan the overlap so we learn whether content is shifted */
	}

	size_t scan = (len < PATTERN_BYTES) ? len : PATTERN_BYTES;
	size_t first = scan;
	size_t diffs = 0;

	for (size_t i = 0; i < scan; i++) {
		if (buf[i] != pattern_expected_byte(i)) {
			if (first == scan) {
				first = i;
			}
			diffs++;
		}
	}

	if (diffs == 0 && len == PATTERN_BYTES) {
		LOG_INF("%s: OK (%d bytes match)", stage, PATTERN_BYTES);
		return 0;
	}

	if (first < scan) {
		int y = (int)(first / PATTERN_STRIDE);
		int b = (int)(first % PATTERN_STRIDE);

		LOG_ERR("%s: MISMATCH first@%zu (row y=%d, byte b=%d, x=%d) "
			"exp=0x%02x got=0x%02x; total_diffs=%zu/%zu",
			stage, first, y, b, b * 8,
			pattern_expected_byte(first), buf[first], diffs, scan);
	}
	return -1;
}

void pattern_canary_fill(uint8_t *region, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		region[i] = PATTERN_CANARY;
	}
}

int pattern_canary_verify(const uint8_t *region, size_t len, const char *stage)
{
	for (size_t i = 0; i < len; i++) {
		if (region[i] != PATTERN_CANARY) {
			LOG_ERR("%s: CANARY corrupted @%zu (got 0x%02x, want 0x%02x) "
				"=> a write spilled past the frame body",
				stage, i, region[i], PATTERN_CANARY);
			return -1;
		}
	}
	return 0;
}
