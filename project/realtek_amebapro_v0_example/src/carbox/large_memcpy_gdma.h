#ifndef CARBOX_LARGE_MEMCPY_GDMA_H
#define CARBOX_LARGE_MEMCPY_GDMA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reserve two linked-list-capable GDMA contexts during application startup.
 * The channels remain owned for the lifetime of the firmware. */
void carbox_large_memcpy_gdma_init(void);

/* Return non-zero only when the entire memcpy has completed through GDMA.
 * A zero return means the caller must perform the complete CPU copy. */
int carbox_large_memcpy_gdma_try(void *dst, const void *src, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_LARGE_MEMCPY_GDMA_H */
