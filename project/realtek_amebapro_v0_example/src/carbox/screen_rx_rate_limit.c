#include "screen_rx_rate_limit.h"
#include "screen_rx_stage_profiler.h"
#include "screen_timestamp_rebase.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "lwip/sockets.h"

#ifndef CONFIG_SCREEN_RX_RATE_LIMIT
#define CONFIG_SCREEN_RX_RATE_LIMIT 0
#endif

#ifndef CONFIG_SCREEN_RX_RATE_LIMIT_BPS
#define CONFIG_SCREEN_RX_RATE_LIMIT_BPS 5000000U
#endif

#ifndef CONFIG_SCREEN_RX_RATE_LIMIT_INTERVAL_MS
#define CONFIG_SCREEN_RX_RATE_LIMIT_INTERVAL_MS 10U
#endif

#ifndef CONFIG_SCREEN_RX_RATE_LIMIT_TASK_PRIORITY
#define CONFIG_SCREEN_RX_RATE_LIMIT_TASK_PRIORITY 5U
#endif

#ifndef CONFIG_SCREEN_RX_RATE_LIMIT_TASK_STACK
#define CONFIG_SCREEN_RX_RATE_LIMIT_TASK_STACK 512U
#endif

#ifndef CONFIG_SCREEN_QUEUE_PROFILE
#define CONFIG_SCREEN_QUEUE_PROFILE 0
#endif
#ifndef CONFIG_SCREEN_TIMESTAMP_REBASE
#define CONFIG_SCREEN_TIMESTAMP_REBASE 0
#endif

#if CONFIG_SCREEN_RX_RATE_LIMIT

static TaskHandle_t screen_rx_task;
static TaskHandle_t screen_rx_limiter_task;
static int screen_rx_socket = -1;

static void screen_rx_limiter_thread(void *argument)
{
	(void)argument;
	for (;;) {
		TickType_t last_wake;

		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		last_wake = xTaskGetTickCount();
		do {
			vTaskDelayUntil(&last_wake,
				pdMS_TO_TICKS(CONFIG_SCREEN_RX_RATE_LIMIT_INTERVAL_MS));
		} while (lwip_screen_rx_rate_limit_tick());
	}
}

static int screen_rx_limiter_ensure(void)
{
	if (screen_rx_limiter_task == NULL) {
		if (xTaskCreate(screen_rx_limiter_thread, "vidrxlimit",
				CONFIG_SCREEN_RX_RATE_LIMIT_TASK_STACK, NULL,
				CONFIG_SCREEN_RX_RATE_LIMIT_TASK_PRIORITY,
				&screen_rx_limiter_task) != pdPASS) {
			return -1;
		}
	}
	return 0;
}

static uint32_t screen_rx_load_le32(const void *pointer)
{
	const uint8_t *p = (const uint8_t *)pointer;

	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int screen_rx_is_receiver_task(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();

	if (screen_rx_task != NULL) {
		return current == screen_rx_task;
	}
	if (strcmp(pcTaskGetName(current), "AirPlayScreenReceiver") == 0) {
		screen_rx_task = current;
		return 1;
	}
	return 0;
}

void carbox_screen_rx_rate_limit_observe(int socket, const void *buffer,
					 size_t requested, int result)
{
	const uint8_t *header;
	uint32_t body_length;
	uint8_t frame_type;

	if ((screen_rx_socket >= 0) || (requested != 128U) ||
	    (result != 128) || (buffer == NULL) ||
	    !screen_rx_is_receiver_task()) {
		return;
	}

	header = (const uint8_t *)buffer;
	body_length = screen_rx_load_le32(header);
	frame_type = header[4];
	if ((body_length < 16U) || (body_length > (4U * 1024U * 1024U)) ||
	    !((frame_type <= 2U) || (frame_type == 4U) ||
	      (frame_type == 5U))) {
		return;
	}

	if ((screen_rx_limiter_ensure() == 0) &&
	    (lwip_screen_rx_rate_limit_enable(socket) == 0)) {
		screen_rx_socket = socket;
		xTaskNotifyGive(screen_rx_limiter_task);
#if defined(CONFIG_SCREEN_DATAPATH_PROFILE) && CONFIG_SCREEN_DATAPATH_PROFILE
		rt_printf("[VIDRXLIMIT] enabled socket=%d target=%lu kbps\r\n",
			  socket,
			  (unsigned long)(CONFIG_SCREEN_RX_RATE_LIMIT_BPS / 1000U));
#endif
	}
}

void carbox_screen_rx_rate_limit_close(int socket)
{
	if (socket == screen_rx_socket) {
		screen_rx_socket = -1;
		screen_rx_task = NULL;
	}
}

void carbox_screen_rx_rate_limit_report(uint32_t sequence)
{
	struct lwip_screen_rx_rate_limit_diag diag;
	uint32_t rate_kbps;

	if (lwip_diag_screen_rx_rate_limit(&diag, 1) != 0) {
		return;
	}
	if (!diag.active && (diag.requested_bytes == 0U) &&
	    (diag.granted_bytes == 0U)) {
		return;
	}
	rate_kbps = diag.elapsed_ms != 0U ?
		(uint32_t)(((uint64_t)diag.granted_bytes * 8U) /
			   diag.elapsed_ms) : 0U;
	rt_printf("[VIDRXLIMIT][%lu] target/rate=%lu/%lu kbps "
		  "requested/granted=%lu/%luB pending/max=%lu/%luB "
		  "tokens=%lu/%luB valve/filtered=%lu/%lu kbps "
		  "adjust open/close/pressure=%lu/%lu/%lu "
		  "ticks=%lu active=%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)(CONFIG_SCREEN_RX_RATE_LIMIT_BPS / 1000U),
		  (unsigned long)rate_kbps,
		  (unsigned long)diag.requested_bytes,
		  (unsigned long)diag.granted_bytes,
		  (unsigned long)diag.pending_bytes,
		  (unsigned long)diag.pending_max_bytes,
		  (unsigned long)diag.tokens_bytes,
		  (unsigned long)diag.bucket_bytes,
		  (unsigned long)diag.valve_kbps,
		  (unsigned long)diag.filtered_kbps,
		  (unsigned long)diag.open_adjusts,
		  (unsigned long)diag.close_adjusts,
		  (unsigned long)diag.pressure_events,
		  (unsigned long)diag.grant_ticks,
		  (unsigned long)diag.active);
}

#if !CONFIG_SCREEN_QUEUE_PROFILE
extern ssize_t __real_lwip_recv(int socket, void *buffer, size_t bytes,
				int flags);

ssize_t __wrap_lwip_recv(int socket, void *buffer, size_t bytes, int flags)
{
	ssize_t result = __real_lwip_recv(socket, buffer, bytes, flags);
	carbox_screen_timestamp_rx_header(socket, buffer, bytes, (int)result);
	carbox_screen_rx_stage_recv(buffer, bytes, (int)result);

	if (screen_rx_socket < 0) {
		carbox_screen_rx_rate_limit_observe(socket, buffer, bytes,
						(int)result);
	}
	return result;
}
#endif

extern int __real_lwip_close(int socket);

int __wrap_lwip_close(int socket)
{
	int result = __real_lwip_close(socket);

	if (result == 0) {
		carbox_screen_timestamp_close(socket);
		carbox_screen_rx_rate_limit_close(socket);
	}
	return result;
}

#else

void carbox_screen_rx_rate_limit_observe(int socket, const void *buffer,
					 size_t requested, int result)
{
	(void)socket;
	(void)buffer;
	(void)requested;
	(void)result;
}

void carbox_screen_rx_rate_limit_close(int socket)
{
	(void)socket;
}

void carbox_screen_rx_rate_limit_report(uint32_t sequence)
{
	(void)sequence;
}

#endif

#if !CONFIG_SCREEN_QUEUE_PROFILE && !CONFIG_SCREEN_RX_RATE_LIMIT && \
	CONFIG_SCREEN_TIMESTAMP_REBASE
extern ssize_t __real_lwip_recv(int socket, void *buffer, size_t bytes,
				int flags);

ssize_t __wrap_lwip_recv(int socket, void *buffer, size_t bytes, int flags)
{
	ssize_t result = __real_lwip_recv(socket, buffer, bytes, flags);

	carbox_screen_timestamp_rx_header(socket, buffer, bytes, (int)result);
	return result;
}

extern int __real_lwip_close(int socket);

int __wrap_lwip_close(int socket)
{
	int result = __real_lwip_close(socket);

	if (result == 0) {
		carbox_screen_timestamp_close(socket);
	}
	return result;
}
#endif
