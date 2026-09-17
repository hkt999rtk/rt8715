#ifndef CARBOX_NOR_UUID_H
#define CARBOX_NOR_UUID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CARBOX_NOR_UUID_SIZE 12U
#define CARBOX_NOR_UUID_INVALID_ARGUMENT (-1)
#define CARBOX_NOR_UUID_UNSUPPORTED      (-2)
#define CARBOX_NOR_UUID_TIMEOUT          (-3)
#define CARBOX_NOR_UUID_INVALID_DATA     (-4)
#define CARBOX_NOR_UUID_NOT_READY        (-5)

/* Hardware error diagnostics during the early boot read, no OTP payload. */
#ifndef CARBOX_NOR_UUID_DIAG
#define CARBOX_NOR_UUID_DIAG 1
#endif

/* Controls boot summary logging only; cache initialization is unconditional. */
#ifndef CARBOX_NOR_BOOT_DIAG
#define CARBOX_NOR_BOOT_DIAG 1
#endif

/* Call once in early main(), after ROM flash/RAM setup and before starting
 * tasks, WLAN, ISP or other DMA users of flash. No RTOS, heap or flash writes.
 * Loads the 12-byte factory UID and full 512-byte OTP into independent caches.
 * Repeated calls are no-ops, including after failure. Internal command code
 * runs from SRAM and restores the original interrupt/cache/SPIC state.
 */
void carbox_nor_identity_cache_init(void);

/* EN25S64A factory UID, in wire/address order (not a 128-bit RFC UUID).
 * Copies the boot cache to writable RAM of at least 12 bytes. Returns 12 on
 * success, NOT_READY before initialization, or the saved boot-read error.
 * Output is unchanged on failure. No flash access, locks or allocation.
 */
int carbox_nor_read_uuid(uint8_t *uuid, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
