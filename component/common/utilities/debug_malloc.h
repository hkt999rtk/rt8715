#ifndef DEBUG_MALLOC_H
#define DEBUG_MALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- configurable constants ---- */
#ifndef DEBUG_MALLOC_MAX_ENTRIES
#define DEBUG_MALLOC_MAX_ENTRIES  256   /* max tracked allocations */
#endif

/* ---- public API ---- */

/**
 * Tracked malloc.  Records __FUNCTION__ and __LINE__ in the alloc table,
 * then delegates to pvPortMalloc().
 */
void *debug_malloc(size_t size, const char *func, int line);

/**
 * Tracked free.  Removes the matching entry from the alloc table,
 * then delegates to vPortFree().
 */
void debug_free(void *ptr, const char *func, int line);

/**
 * Dump all currently-tracked (unfreed) allocations to stdout.
 */
void debug_malloc_dump(void);

/**
 * Return the number of active tracked allocations.
 */
int debug_malloc_count(void);

/* ---- convenience macros ---- */

/**
 * Use these instead of raw malloc()/free() to get automatic
 * __FUNCTION__ and __LINE__ tracking.
 *
 * Example:
 *   void *buf = MALLOC(128);
 *   if (buf) { ... }
 *   FREE(buf);
 */
#define MALLOC(sz)   debug_malloc((sz), __FUNCTION__, __LINE__)
#define FREE(ptr)    debug_free((ptr),  __FUNCTION__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_MALLOC_H */
