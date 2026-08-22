#ifndef CARBOX_SCREEN_RX_RATE_LIMIT_H
#define CARBOX_SCREEN_RX_RATE_LIMIT_H

#include <stddef.h>
#include <stdint.h>

void carbox_screen_rx_rate_limit_observe(int socket, const void *buffer,
					 size_t requested, int result);
void carbox_screen_rx_rate_limit_close(int socket);
void carbox_screen_rx_rate_limit_frame(void);
void carbox_screen_rx_rate_limit_report(uint32_t sequence);

#endif
