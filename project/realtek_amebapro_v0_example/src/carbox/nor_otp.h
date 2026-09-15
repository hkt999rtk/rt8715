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
 * Call from a normal task after system/flash initialization. Do not call from
 * an ISR, with interrupts disabled, or while already holding the flash lock.
 * Shares the SRAM transport in nor_uuid.c; uses about 512 bytes of stack for
 * staging, plus normal call overhead. SPI/dual/quad/QPI STR only, not DTR.
 * A hardware timeout triggers best-effort OTP exit/mode restoration; hardware
 * recovery and continued XIP cannot be guaranteed for a faulty controller.
 * This new API does not redirect the customer's nor_read_otp/spinor_read_otp.
 */
int carplay_nor_read_otp(uint32_t offset, void *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif
