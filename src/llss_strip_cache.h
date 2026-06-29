#ifndef LLSS_STRIP_CACHE_H_
#define LLSS_STRIP_CACHE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Small LRU cache of pressed-state button strips, keyed by the
 * content-hash strip_id advertised by LLSS. Bodies live in PSRAM.
 *
 * Concurrency model: a single producer (background prefetch in the LLSS
 * polling thread) calls _put(); a single consumer (the press handler on
 * the input/UI thread) calls _get(). The pointer returned by _get() is
 * valid as long as no _put() that would evict that slot runs in the
 * meantime — fine in practice because presses are short and prefetches
 * only happen between frames.
 */

void llss_strip_cache_init(void);

/* Returns a pointer to the cached strip buffer on hit (and writes its
 * size to *len_out), or NULL on miss / empty id. */
const uint8_t *llss_strip_cache_get(const char *strip_id, size_t *len_out);

/* Inserts (or refreshes) a strip in the cache. No-op on invalid input or
 * len > max strip size. */
void llss_strip_cache_put(const char *strip_id,
			  const uint8_t *src, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LLSS_STRIP_CACHE_H_ */
