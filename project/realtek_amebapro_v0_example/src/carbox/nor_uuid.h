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

/* EN25S64A factory UID, in wire/address order (not a 128-bit RFC UUID).
 * Call from a normal task AFTER flash/system initialization, with a writable
 * RAM buffer of at least 12 bytes. Returns 12 on success, a negative code above
 * otherwise; the output buffer is unchanged on failure. No allocation or log.
 * Uses the SDK flash resource lock; do not call with that lock already held.
 * Only supported STR SPI/dual/quad/QPI configurations are accepted.
 * Code must be linked into SRAM: flash is temporarily unavailable for XIP.
 */
int carbox_nor_read_uuid(uint8_t *uuid, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
