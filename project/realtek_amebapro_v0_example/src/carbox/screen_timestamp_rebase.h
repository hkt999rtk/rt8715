#ifndef CARBOX_SCREEN_TIMESTAMP_REBASE_H
#define CARBOX_SCREEN_TIMESTAMP_REBASE_H

#include <stddef.h>
#include <stdint.h>

/* Lightweight RX-header to outgoing-normal-frame timestamp transport. */
void carbox_screen_timestamp_rx_header(int socket, const void *buffer,
				       size_t requested, int result);
void carbox_screen_timestamp_send_begin(const void *data, int bytes);
void carbox_screen_timestamp_send_end(void);
void carbox_screen_timestamp_queue_push(void *vector, const void *pointer,
					int bytes, int pushed);
void carbox_screen_timestamp_queue_erase(void *vector, int index,
					 const void *pointer, int bytes);
void carbox_screen_timestamp_queue_delete(void *vector);
void carbox_screen_timestamp_close(int socket);

/* Called only after the closed sender copied its validated 128-byte normal
 * frame header and before its payload encryption/authentication begins. */
int carbox_screen_timestamp_patch_normal_header(void *header, size_t length);

/* Called by the existing 10-second PC-profiler reporter. */
void carbox_screen_timestamp_report(uint32_t sequence);

#endif /* CARBOX_SCREEN_TIMESTAMP_REBASE_H */
