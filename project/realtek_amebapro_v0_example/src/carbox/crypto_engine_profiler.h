#ifndef CARBOX_CRYPTO_ENGINE_PROFILER_H
#define CARBOX_CRYPTO_ENGINE_PROFILER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void crypto_engine_profiler_report(uint32_t sequence);

/*
 * Explicit scope for the non-aligned ChaCha combined ROM bypass.
 *
 * Normal HAL calls are observed by link-time --wrap hooks.  The partial TX
 * path deliberately calls the ROM function table directly, so it must mark
 * the same API interval explicitly or the surrounding device-lock interval
 * is incorrectly reported as time outside the crypto API.
 */
uint32_t crypto_engine_profiler_chacha_combined_begin(uint32_t message_len);
void crypto_engine_profiler_chacha_combined_end(uint32_t start_us,
	uint32_t message_len, uint32_t aad_len, int result);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_CRYPTO_ENGINE_PROFILER_H */
