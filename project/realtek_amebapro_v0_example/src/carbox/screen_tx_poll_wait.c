#include "screen_tx_poll_wait.h"

#include "FreeRTOS.h"
#include "task.h"
#include "lwip/sockets.h"
#include "lwip/priv/tcp_wake_trace.h"

#if defined(CONFIG_SCREEN_BLOCK_PROFILE) && CONFIG_SCREEN_BLOCK_PROFILE && \
    !CONFIG_SCREEN_TX_DIRECT_CRYPTO && !CONFIG_SCREEN_QUEUE_PROFILE
#include "hal_timer.h"
#include "screen_tx_probe.h"
#endif

#if CONFIG_SCREEN_WAKE_TRACE && (POLLOUT != 2 || (POLLERR | POLLNVAL | POLLHUP) != 0x20c)
#error "Update tcp_wake_trace poll masks for this socket ABI"
#endif

#ifndef CONFIG_SCREEN_TX_POLL_TIMEOUT_MS
#define CONFIG_SCREEN_TX_POLL_TIMEOUT_MS 250
#endif

#if CONFIG_SCREEN_TX_POLL_TIMEOUT_MS <= 0
#error "screen TX poll timeout must be positive"
#endif

/* The derived normal-frame sender calls this in both quiet and diagnostic
 * builds. No task-name/fd cache: the socket API validates the live PCB and USB
 * route. Repeated opt-in never restarts its timer or resets RTO backoff. */
ssize_t carbox_screen_tcp_write(int socket, const void *buffer, size_t bytes)
{
#if LWIP_TCP && LWIP_TCP_RECOVERY
	(void)lwip_usb_screen_rto_enable(socket);
#endif
	return lwip_write(socket, buffer, bytes);
}

void carbox_screen_tx_poll_wait(int socket)
{
	struct pollfd fd = { .fd = socket, .events = POLLOUT, .revents = 0 };
	int result;
#if defined(CONFIG_SCREEN_BLOCK_PROFILE) && CONFIG_SCREEN_BLOCK_PROFILE && \
    !CONFIG_SCREEN_TX_DIRECT_CRYPTO && !CONFIG_SCREEN_QUEUE_PROFILE
	uint32_t start_us = hal_read_curtime_us();
#endif
#if LWIP_TCP && LWIP_TCP_RECOVERY
	/* Idempotent PCB-scoped opt-in; only the USB en route is eligible.
	 * Apply before waiting so an ACK-silent full sender need not wait 1s. */
	(void)lwip_usb_screen_rto_enable(socket);
#endif
#if CONFIG_SCREEN_WAKE_TRACE
	tcp_wake_poll_begin(socket);
#endif
	result = lwip_poll(&fd, 1, CONFIG_SCREEN_TX_POLL_TIMEOUT_MS);
#if CONFIG_SCREEN_WAKE_TRACE
	tcp_wake_poll_end(socket, result, fd.revents);
#endif

	/* A timeout lets the customer's sender retry its write.  If poll itself
	 * fails, retain a short delay so an EAGAIN write cannot spin forever. */
	if (result < 0) {
		vTaskDelay(pdMS_TO_TICKS(10));
	}
#if defined(CONFIG_SCREEN_BLOCK_PROFILE) && CONFIG_SCREEN_BLOCK_PROFILE && \
    !CONFIG_SCREEN_TX_DIRECT_CRYPTO && !CONFIG_SCREEN_QUEUE_PROFILE
	carbox_screen_tx_poll_observe(result, fd.revents,
				      hal_read_curtime_us() - start_us);
#endif
}

int carbox_screen_block_quiet(const char *message)
{
	(void)message;
	return 0;
}
