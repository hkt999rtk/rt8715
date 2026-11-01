#include "screen_tx_poll_wait.h"

#include "FreeRTOS.h"
#include "task.h"
#include "lwip/sockets.h"

#if defined(CONFIG_SCREEN_BLOCK_PROFILE) && CONFIG_SCREEN_BLOCK_PROFILE && \
    !CONFIG_SCREEN_TX_DIRECT_CRYPTO && !CONFIG_SCREEN_QUEUE_PROFILE
#include "hal_timer.h"
#include "screen_tx_probe.h"
#endif

#ifndef CONFIG_SCREEN_TX_POLL_TIMEOUT_MS
#define CONFIG_SCREEN_TX_POLL_TIMEOUT_MS 250
#endif

#if CONFIG_SCREEN_TX_POLL_TIMEOUT_MS <= 0
#error "screen TX poll timeout must be positive"
#endif

void carbox_screen_tx_poll_wait(int socket)
{
	struct pollfd fd = { .fd = socket, .events = POLLOUT, .revents = 0 };
	int result;
#if defined(CONFIG_SCREEN_BLOCK_PROFILE) && CONFIG_SCREEN_BLOCK_PROFILE && \
    !CONFIG_SCREEN_TX_DIRECT_CRYPTO && !CONFIG_SCREEN_QUEUE_PROFILE
	uint32_t start_us = hal_read_curtime_us();
#endif
	result = lwip_poll(&fd, 1, CONFIG_SCREEN_TX_POLL_TIMEOUT_MS);

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
