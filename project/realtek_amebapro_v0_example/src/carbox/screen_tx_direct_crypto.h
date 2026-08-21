#ifndef CARBOX_SCREEN_TX_DIRECT_CRYPTO_H
#define CARBOX_SCREEN_TX_DIRECT_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

typedef struct carbox_screen_tx_stage_snapshot_s {
	uint32_t frames;
	uint32_t full_writes;
	uint32_t partial_writes;
	uint32_t unmatched_writes;
	uint64_t prepare_sum_us;
	uint32_t prepare_max_us;
	uint64_t alloc_to_header_sum_us;
	uint32_t alloc_to_header_max_us;
	uint64_t header_to_payload_sum_us;
	uint32_t header_to_payload_max_us;
	uint64_t payload_to_crypto_sum_us;
	uint32_t payload_to_crypto_max_us;
	uint64_t crypto_sum_us;
	uint32_t crypto_max_us;
	uint64_t post_crypto_sum_us;
	uint32_t post_crypto_max_us;
	uint64_t crypto_to_write_sum_us;
	uint32_t crypto_to_write_max_us;
	uint64_t write_call_sum_us;
	uint32_t write_call_max_us;
	uint64_t service_sum_us;
	uint32_t service_max_us;
	uint32_t service_over_16ms;
	uint32_t service_over_33ms;
} carbox_screen_tx_stage_snapshot_t;

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
void carbox_screen_tx_write_begin(const void *buffer);
void carbox_screen_tx_after_write(const void *buffer, size_t requested,
				  int result);
void carbox_screen_block_profile_write(int socket, size_t requested,
				       int result);
size_t carbox_screen_tx_pacer_allowance(size_t requested);
void carbox_screen_tx_pacer_complete(size_t allowed, int result);
void carbox_screen_tx_direct_crypto_report(uint32_t sequence);
void carbox_screen_crypto_latency_report(uint32_t sequence);
int carbox_screen_tx_stage_snapshot(carbox_screen_tx_stage_snapshot_t *snapshot);
void carbox_screen_tx_stage_report(uint32_t sequence);
void carbox_screen_block_profile_report(uint32_t sequence);
void carbox_screen_tx_pacer_report(uint32_t sequence);
void carbox_screen_usb_probe_start(void);

#define CARBOX_SCREEN_TX_CRYPTO_AES     1U
#define CARBOX_SCREEN_TX_CRYPTO_CHACHA  2U

#endif /* CARBOX_SCREEN_TX_DIRECT_CRYPTO_H */
