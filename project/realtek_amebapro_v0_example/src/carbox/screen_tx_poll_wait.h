#ifndef CARBOX_SCREEN_TX_POLL_WAIT_H
#define CARBOX_SCREEN_TX_POLL_WAIT_H

#include "lwip/sockets.h"

/* Normal-frame writes opt in before sending; return partial/error unchanged. */
ssize_t carbox_screen_tcp_write(int socket, const void *buffer, size_t bytes);

/* Called from the customer's normal-frame EAGAIN retry path. */
void carbox_screen_tx_poll_wait(int socket);

/* Replacement for the vendor EAGAIN puts call when tracing is disabled. */
int carbox_screen_block_quiet(const char *message);

#endif
