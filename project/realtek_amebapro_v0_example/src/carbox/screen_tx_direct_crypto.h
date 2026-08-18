#ifndef CARBOX_SCREEN_TX_DIRECT_CRYPTO_H
#define CARBOX_SCREEN_TX_DIRECT_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

/* AirPlayScreen.o-only hooks installed in the derived vendor archive. */
void carbox_screen_tx_allocation(void *pointer, size_t length);
void carbox_screen_tx_release(void *pointer);
int carbox_screen_tx_owned_begin(const void *pointer, size_t length);
int carbox_screen_tx_owned_consumer_release(void *pointer);
void carbox_screen_tx_owned_complete(void *pointer);
void *carbox_airplay_screen_memcpy(void *destination, const void *source,
				   size_t length);

/* Resolve the closed sender's nominal in-place crypto arguments to the
 * deferred plaintext source and final wire destination. */
int carbox_screen_tx_crypto_begin(void *alias_source, size_t length,
				  void *destination, uint32_t kind,
				  const void **direct_source);
int carbox_screen_tx_crypto_active(void *destination, size_t length,
				   const void **direct_source);
void carbox_screen_tx_crypto_materialized(void *destination, size_t length);
void carbox_screen_tx_crypto_complete(void *destination, size_t length,
				      uint32_t kind, int status);
void carbox_screen_tx_before_write(const void *buffer, size_t length);
void carbox_screen_block_profile_write(int socket, size_t requested,
				       int result);
size_t carbox_screen_tx_pacer_allowance(size_t requested);
void carbox_screen_tx_pacer_complete(size_t allowed, int result);
void carbox_screen_tx_direct_crypto_report(uint32_t sequence);
void carbox_screen_block_profile_report(uint32_t sequence);
void carbox_screen_tx_pacer_report(uint32_t sequence);
void carbox_screen_usb_probe_start(void);

#define CARBOX_SCREEN_TX_CRYPTO_AES     1U
#define CARBOX_SCREEN_TX_CRYPTO_CHACHA  2U

#endif /* CARBOX_SCREEN_TX_DIRECT_CRYPTO_H */
