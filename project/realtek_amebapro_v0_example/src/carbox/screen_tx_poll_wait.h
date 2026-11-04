#ifndef CARBOX_SCREEN_TX_POLL_WAIT_H
#define CARBOX_SCREEN_TX_POLL_WAIT_H

#include "lwip/sockets.h"

#ifndef CONFIG_SCREEN_TX_WRITE_CHUNK_BYTES
#define CONFIG_SCREEN_TX_WRITE_CHUNK_BYTES 4096
#endif
#ifndef CONFIG_SCREEN_TX_BATCH_DIAG
#define CONFIG_SCREEN_TX_BATCH_DIAG 0
#endif

/* Normal-frame writes opt in before sending. Limit one submission; the vendor
 * loop advances by the actual return value and polls on EAGAIN. */
ssize_t carbox_screen_tcp_write(int socket, const void *buffer, size_t bytes);

/* Only the existing low-priority diagnostic task calls this. */
void carbox_screen_write_report(void);

/* Replacement for the vendor EAGAIN puts call when tracing is disabled. */
int carbox_screen_block_quiet(const char *message);

#endif
