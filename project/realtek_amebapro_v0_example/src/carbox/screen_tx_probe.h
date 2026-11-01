#ifndef CARBOX_SCREEN_TX_PROBE_H
#define CARBOX_SCREEN_TX_PROBE_H

#include <stdint.h>

void carbox_screen_tx_probe_report(uint32_t sequence);
void carbox_screen_normal_frame_probe(const void *data, int length);
int carbox_screen_block_note(const char *message);
void carbox_screen_tx_poll_observe(int result, int revents, uint32_t wait_us);

#endif
