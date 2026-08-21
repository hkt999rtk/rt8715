#ifndef CARBOX_SCREEN_RX_STAGE_PROFILER_H
#define CARBOX_SCREEN_RX_STAGE_PROFILER_H

#include <stddef.h>
#include <stdint.h>

/* Lightweight timestamps for one screen RX record.  The hooks intentionally
 * retain only the current record; any unexpected overlap is counted instead
 * of being paired with the wrong frame. */
void carbox_screen_rx_stage_recv(const void *buffer, size_t requested,
				 int result);
void carbox_screen_rx_stage_crypto_begin(void);
void carbox_screen_rx_stage_crypto_end(int failed);
void carbox_screen_rx_stage_handover_begin(void);
void carbox_screen_rx_stage_handover_end(void);
void carbox_screen_rx_stage_report(uint32_t sequence);

#endif /* CARBOX_SCREEN_RX_STAGE_PROFILER_H */
