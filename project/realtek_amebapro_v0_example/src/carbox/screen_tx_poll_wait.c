#include "screen_tx_poll_wait.h"

#if CONFIG_SCREEN_TX_WRITE_CHUNK_BYTES < 1
#error CONFIG_SCREEN_TX_WRITE_CHUNK_BYTES must be positive
#endif

#if CONFIG_SCREEN_TX_BATCH_DIAG
#include <stdint.h>
#include <errno.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "lwip/err.h"
#include "rx_handover_diag.h"

struct screen_write_stats {
	uint32_t writes, limited, partial, eagain, errors, zero;
	uint32_t submitted, accepted, submit_max, write_max_us;
};
static struct screen_write_stats screen_writes;
static uint32_t screen_write_total;
#endif

/* The derived normal-frame sender calls this in both quiet and diagnostic
 * builds. No task-name/fd cache: the socket API validates the live PCB and USB
 * route. Repeated opt-in never restarts its timer or resets RTO backoff. */
ssize_t carbox_screen_tcp_write(int socket, const void *buffer, size_t bytes)
{
	size_t batch = bytes > CONFIG_SCREEN_TX_WRITE_CHUNK_BYTES ?
		CONFIG_SCREEN_TX_WRITE_CHUNK_BYTES : bytes;
	ssize_t result;
#if CONFIG_SCREEN_TX_BATCH_DIAG
	uint32_t start_us, elapsed_us;
	int error;
	start_us = hal_read_curtime_us();
#endif
#if LWIP_TCP && LWIP_TCP_RECOVERY
	(void)lwip_usb_screen_rto_enable(socket);
#endif
	result = lwip_write(socket, buffer, batch);
#if CONFIG_SCREEN_TX_BATCH_DIAG
	/* Capture errno before any other work; counting never invokes socket APIs,
	 * logging, waits or allocation. Timing includes the route-policy call. */
	error = result < 0 ? lwip_errno() : 0;
	elapsed_us = hal_read_curtime_us() - start_us;
	taskENTER_CRITICAL();
	screen_write_total++;
	screen_writes.writes++;
	screen_writes.limited += batch < bytes;
	screen_writes.submitted += (uint32_t)batch;
	if (batch > screen_writes.submit_max) screen_writes.submit_max = batch;
	if (elapsed_us > screen_writes.write_max_us)
		screen_writes.write_max_us = elapsed_us;
	if (result > 0) {
		screen_writes.accepted += (uint32_t)result;
		screen_writes.partial += (size_t)result < batch;
	} else if (result < 0) {
		if (error == EAGAIN || error == EWOULDBLOCK) screen_writes.eagain++;
		else screen_writes.errors++;
	} else if (batch != 0) {
		screen_writes.zero++;
	}
	taskEXIT_CRITICAL();
	/* Customer caller reads the shared lwip_errno() after return. Restore the
	 * captured failure after instrumentation, which can be preempted by other
	 * socket tasks. This does not fix lwIP's pre-existing global-errno race. */
	if (result < 0) lwip_err = error;
#endif
	return result;
}

void carbox_screen_write_report(void)
{
#if CONFIG_SCREEN_TX_BATCH_DIAG
	static TickType_t previous_tick;
	struct screen_write_stats s;
	uint32_t total;
	TickType_t now = xTaskGetTickCount();
	TickType_t elapsed = now - previous_tick;
	if (elapsed < pdMS_TO_TICKS(5000)) return;
	previous_tick = now;
	taskENTER_CRITICAL();
	s = screen_writes;
	screen_writes = (struct screen_write_stats){0};
	total = screen_write_total;
	taskEXIT_CRITICAL();
	if (!s.writes) return;
	/* Window byte totals count submissions (including retries), not on-wire
	 * traffic. No producer printing. Shared UART logger may skip on contention;
	 * total_writes remains cumulative modulo 2^32 across skipped reports. */
	carbox_rxhd_log("[SCREEN_WRITE] tick=%lu window_ms=%lu cap=%lu writes=%lu total_writes=%lu limited=%lu submitted=%lu accepted=%lu submit_max=%lu partial=%lu eagain=%lu errors=%lu zero=%lu write_max_us=%lu\n",
		(unsigned long)now,
		(unsigned long)((uint64_t)elapsed * 1000U / configTICK_RATE_HZ),
		(unsigned long)CONFIG_SCREEN_TX_WRITE_CHUNK_BYTES,
		(unsigned long)s.writes, (unsigned long)total,
		(unsigned long)s.limited, (unsigned long)s.submitted,
		(unsigned long)s.accepted, (unsigned long)s.submit_max,
		(unsigned long)s.partial, (unsigned long)s.eagain,
		(unsigned long)s.errors, (unsigned long)s.zero,
		(unsigned long)s.write_max_us);
#endif
}

int carbox_screen_block_quiet(const char *message)
{
	(void)message;
	return 0;
}
