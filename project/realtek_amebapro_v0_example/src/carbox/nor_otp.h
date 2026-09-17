#ifndef CARPLAY_NOR_OTP_H
#define CARPLAY_NOR_OTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CARPLAY_NOR_OTP_SIZE             512U
#define CARPLAY_NOR_OTP_INVALID_ARGUMENT (-1)
#define CARPLAY_NOR_OTP_UNSUPPORTED      (-2)
#define CARPLAY_NOR_OTP_TIMEOUT          (-3)
#define CARPLAY_NOR_OTP_INVALID_DATA     (-4)
#define CARPLAY_NOR_OTP_NOT_READY        (-5)
#define CARPLAY_NOR_OTP_BUSY             (-6)

/* Read the EN25S64A user OTP, NOT its factory UID or the SoC eFuse.
 * offset is relative to the 512-byte OTP region (0..511), not a flash address.
 * buffer must hold length bytes in writable RAM. Returns length on success,
 * or a negative code above; output is unchanged on error. A zero-length read
 * at offset <= 512 succeeds without touching hardware (NULL buffer allowed).
 * All-zero/all-FF OTP contents are valid; this API never writes/erases/locks OTP.
 *
 * Reads only the immutable full-region RAM cache populated by
 * carbox_nor_identity_cache_init() in early main(). No flash commands, locks,
 * allocation or RTOS calls. Before initialization returns NOT_READY; a failed
 * boot read returns the saved error without modifying the caller buffer.
 * OTP programming after boot, if performed externally, is visible next boot.
 * The current customer spinor_read_otp() calls this API; its ABI is unchanged.
 */
int carplay_nor_read_otp(uint32_t offset, void *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif
