#ifndef CARBOX_SCREEN_RX_RECORD_PROFILER_H
#define CARBOX_SCREEN_RX_RECORD_PROFILER_H

#include <stddef.h>
#include <stdint.h>

void carbox_screen_rx_record_recv(const void *buffer, size_t requested,
				  int result);
void carbox_screen_rx_record_alloc(void *pointer, size_t length);
void carbox_screen_rx_record_free(void *pointer);
void carbox_screen_rx_record_send_video(const void *pointer, int length);

void carbox_screen_rx_crypto_init(void);
void carbox_screen_rx_crypto_aad(size_t length);
void carbox_screen_rx_crypto_decrypt(size_t input, size_t output);
void carbox_screen_rx_crypto_verify(size_t output, const int32_t *error);
void carbox_screen_rx_crypto_final(size_t output);

void carbox_screen_rx_record_profiler_report(uint32_t sequence);

#endif /* CARBOX_SCREEN_RX_RECORD_PROFILER_H */
