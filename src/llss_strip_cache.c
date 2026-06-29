#include "llss_strip_cache.h"
#include "llss_client.h"
#include "section_attrs.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define DISPLAY_W       800
#define STRIP_H_MAX     MAX(CONFIG_LLSS_TOP_STRIP_HEIGHT, \
				 CONFIG_LLSS_BOTTOM_STRIP_HEIGHT)
#define MAX_STRIP_BYTES (DISPLAY_W * STRIP_H_MAX / 8)
#define CACHE_SLOTS     4

struct strip_slot {
	char     id[LLSS_STRIP_ID_MAX];
	size_t   len;
	uint32_t lru_tick;
};

static struct strip_slot slots[CACHE_SLOTS];
static uint32_t          lru_counter;
static struct k_mutex    cache_lock;

static uint8_t cache_buf[CACHE_SLOTS][MAX_STRIP_BYTES]
	LLSS_EXT_RAM_NOINIT("strip_cache");

void llss_strip_cache_init(void)
{
	k_mutex_init(&cache_lock);
	for (int i = 0; i < CACHE_SLOTS; i++) {
		slots[i].id[0]    = '\0';
		slots[i].len      = 0;
		slots[i].lru_tick = 0;
	}
	lru_counter = 0;
}

const uint8_t *llss_strip_cache_get(const char *strip_id, size_t *len_out)
{
	if (!strip_id || !strip_id[0]) {
		return NULL;
	}

	const uint8_t *hit = NULL;

	k_mutex_lock(&cache_lock, K_FOREVER);
	for (int i = 0; i < CACHE_SLOTS; i++) {
		if (slots[i].id[0] && strcmp(slots[i].id, strip_id) == 0) {
			slots[i].lru_tick = ++lru_counter;
			if (len_out) {
				*len_out = slots[i].len;
			}
			hit = cache_buf[i];
			break;
		}
	}
	k_mutex_unlock(&cache_lock);

	return hit;
}

void llss_strip_cache_put(const char *strip_id,
			  const uint8_t *src, size_t len)
{
	if (!strip_id || !strip_id[0] || !src || len == 0 ||
	    len > MAX_STRIP_BYTES) {
		return;
	}

	k_mutex_lock(&cache_lock, K_FOREVER);

	int target = -1;
	uint32_t oldest = UINT32_MAX;

	for (int i = 0; i < CACHE_SLOTS; i++) {
		if (slots[i].id[0] && strcmp(slots[i].id, strip_id) == 0) {
			/* Already present — touch LRU and exit. */
			slots[i].lru_tick = ++lru_counter;
			k_mutex_unlock(&cache_lock);
			return;
		}
		if (!slots[i].id[0]) {
			target = i;
			oldest = 0;
			continue;
		}
		if (slots[i].lru_tick < oldest) {
			oldest = slots[i].lru_tick;
			target = i;
		}
	}

	strncpy(slots[target].id, strip_id,
		sizeof(slots[target].id) - 1);
	slots[target].id[sizeof(slots[target].id) - 1] = '\0';
	slots[target].len      = len;
	slots[target].lru_tick = ++lru_counter;
	memcpy(cache_buf[target], src, len);

	k_mutex_unlock(&cache_lock);
}
