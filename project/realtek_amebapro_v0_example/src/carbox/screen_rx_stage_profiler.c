#include "screen_rx_stage_profiler.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "hal_timer.h"
#include "lwip/sockets.h"

#ifndef CONFIG_SCREEN_FPS_PROFILE
#define CONFIG_SCREEN_FPS_PROFILE 0
#endif
#ifndef CONFIG_SCREEN_QUEUE_PROFILE
#define CONFIG_SCREEN_QUEUE_PROFILE 0
#endif
#ifndef CONFIG_SCREEN_RX_RATE_LIMIT
#define CONFIG_SCREEN_RX_RATE_LIMIT 0
#endif

#if CONFIG_SCREEN_FPS_PROFILE
typedef struct screen_rx_stage_stats_s {
	uint32_t frames;
	uint32_t crypto_errors;
	uint32_t unmatched_crypto;
	uint32_t unmatched_handover;
	uint32_t overwritten;
	uint64_t recv_to_crypto_sum_us;
	uint32_t recv_to_crypto_max_us;
	uint64_t crypto_sum_us;
	uint32_t crypto_max_us;
	uint64_t crypto_to_handover_sum_us;
	uint32_t crypto_to_handover_max_us;
	uint64_t handover_sum_us;
	uint32_t handover_max_us;
	uint64_t service_sum_us;
	uint32_t service_max_us;
	uint32_t service_over_16ms;
	uint32_t service_over_33ms;
} screen_rx_stage_stats_t;

typedef struct screen_rx_stage_state_s {
	TaskHandle_t receiver_task;
	uint32_t last_payload_recv_us;
	uint32_t frame_recv_us;
	uint32_t crypto_start_us;
	uint32_t crypto_end_us;
	uint32_t handover_start_us;
	uint8_t crypto_active;
	uint8_t crypto_ready;
	uint8_t handover_active;
} screen_rx_stage_state_t;

static screen_rx_stage_stats_t screen_rx_stage_stats
	__attribute__((section(".lpddr.bss.screen_rx_stage_stats")));
static screen_rx_stage_state_t screen_rx_stage_state
	__attribute__((section(".lpddr.bss.screen_rx_stage_state")));

static int screen_rx_stage_is_receiver(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();

	if (screen_rx_stage_state.receiver_task != NULL) {
		return current == screen_rx_stage_state.receiver_task;
	}
	if (strcmp(pcTaskGetName(current), "AirPlayScreenReceiver") == 0) {
		screen_rx_stage_state.receiver_task = current;
		return 1;
	}
	return 0;
}

void carbox_screen_rx_stage_recv(const void *buffer, size_t requested,
				 int result)
{
	(void)buffer;
	/* The closed receiver uses >128-byte requests for the frame body.  Keep
	 * the latest successful return so partial body reads naturally select the
	 * final payload recv immediately preceding crypto. */
	if ((requested <= 128U) || (result <= 0) ||
	    !screen_rx_stage_is_receiver()) {
		return;
	}
	taskENTER_CRITICAL();
	screen_rx_stage_state.last_payload_recv_us = hal_read_curtime_us();
	taskEXIT_CRITICAL();
}

void carbox_screen_rx_stage_crypto_begin(void)
{
	uint32_t now_us = hal_read_curtime_us();

	taskENTER_CRITICAL();
	if (screen_rx_stage_state.crypto_active ||
	    screen_rx_stage_state.crypto_ready ||
	    screen_rx_stage_state.handover_active) {
		screen_rx_stage_stats.overwritten++;
	}
	if (screen_rx_stage_state.last_payload_recv_us == 0U) {
		screen_rx_stage_stats.unmatched_crypto++;
		screen_rx_stage_state.crypto_active = 0U;
		taskEXIT_CRITICAL();
		return;
	}
	screen_rx_stage_state.frame_recv_us =
		screen_rx_stage_state.last_payload_recv_us;
	screen_rx_stage_state.last_payload_recv_us = 0U;
	screen_rx_stage_state.crypto_start_us = now_us;
	screen_rx_stage_state.crypto_active = 1U;
	screen_rx_stage_state.crypto_ready = 0U;
	screen_rx_stage_state.handover_active = 0U;
	taskEXIT_CRITICAL();
}

void carbox_screen_rx_stage_crypto_end(int failed)
{
	taskENTER_CRITICAL();
	if (!screen_rx_stage_state.crypto_active) {
		taskEXIT_CRITICAL();
		return;
	}
	screen_rx_stage_state.crypto_end_us = hal_read_curtime_us();
	screen_rx_stage_state.crypto_active = 0U;
	if (failed) {
		screen_rx_stage_stats.crypto_errors++;
		screen_rx_stage_state.crypto_ready = 0U;
	} else {
		screen_rx_stage_state.crypto_ready = 1U;
	}
	taskEXIT_CRITICAL();
}

void carbox_screen_rx_stage_handover_begin(void)
{
	taskENTER_CRITICAL();
	if (!screen_rx_stage_state.crypto_ready) {
		screen_rx_stage_stats.unmatched_handover++;
		screen_rx_stage_state.handover_active = 0U;
		taskEXIT_CRITICAL();
		return;
	}
	screen_rx_stage_state.handover_start_us = hal_read_curtime_us();
	screen_rx_stage_state.crypto_ready = 0U;
	screen_rx_stage_state.handover_active = 1U;
	taskEXIT_CRITICAL();
}

void carbox_screen_rx_stage_handover_end(void)
{
	uint32_t now_us;
	uint32_t recv_to_crypto_us;
	uint32_t crypto_us;
	uint32_t crypto_to_handover_us;
	uint32_t handover_us;
	uint32_t service_us;

	taskENTER_CRITICAL();
	if (!screen_rx_stage_state.handover_active) {
		taskEXIT_CRITICAL();
		return;
	}
	now_us = hal_read_curtime_us();
	recv_to_crypto_us = screen_rx_stage_state.crypto_start_us -
		screen_rx_stage_state.frame_recv_us;
	crypto_us = screen_rx_stage_state.crypto_end_us -
		screen_rx_stage_state.crypto_start_us;
	crypto_to_handover_us = screen_rx_stage_state.handover_start_us -
		screen_rx_stage_state.crypto_end_us;
	handover_us = now_us - screen_rx_stage_state.handover_start_us;
	service_us = now_us - screen_rx_stage_state.frame_recv_us;

#define SCREEN_RX_STAGE_ADD(name, value) do { \
	screen_rx_stage_stats.name##_sum_us += (value); \
	if ((value) > screen_rx_stage_stats.name##_max_us) \
		screen_rx_stage_stats.name##_max_us = (value); \
} while (0)
	SCREEN_RX_STAGE_ADD(recv_to_crypto, recv_to_crypto_us);
	SCREEN_RX_STAGE_ADD(crypto, crypto_us);
	SCREEN_RX_STAGE_ADD(crypto_to_handover, crypto_to_handover_us);
	SCREEN_RX_STAGE_ADD(handover, handover_us);
	SCREEN_RX_STAGE_ADD(service, service_us);
#undef SCREEN_RX_STAGE_ADD
	screen_rx_stage_stats.frames++;
	screen_rx_stage_stats.service_over_16ms += service_us > 16667U;
	screen_rx_stage_stats.service_over_33ms += service_us > 33333U;
	screen_rx_stage_state.handover_active = 0U;
	taskEXIT_CRITICAL();
}

void carbox_screen_rx_stage_report(uint32_t sequence)
{
	screen_rx_stage_stats_t stats;

	taskENTER_CRITICAL();
	stats = screen_rx_stage_stats;
	screen_rx_stage_stats = (screen_rx_stage_stats_t){ 0 };
	taskEXIT_CRITICAL();
	if ((stats.frames == 0U) && (stats.unmatched_crypto == 0U) &&
	    (stats.unmatched_handover == 0U) && (stats.overwritten == 0U)) {
		return;
	}
#define SCREEN_RX_STAGE_AVG(name) \
	(unsigned long long)(stats.frames != 0U ? \
		stats.name##_sum_us / stats.frames : 0U), \
	(unsigned long)stats.name##_max_us
	rt_printf("[SCREENRXSTAGE][%lu] frames/error/unmatched_crypto/handover/overwrite="
		  "%lu/%lu/%lu/%lu/%lu recv_to_crypto_us avg/max=%llu/%lu "
		  "crypto_us avg/max=%llu/%lu crypto_to_handover_us avg/max=%llu/%lu "
		  "handover_us avg/max=%llu/%lu service_us avg/max=%llu/%lu "
		  "over16/33ms=%lu/%lu boundary=last-payload-recv-to-queue-commit\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.frames,
		  (unsigned long)stats.crypto_errors,
		  (unsigned long)stats.unmatched_crypto,
		  (unsigned long)stats.unmatched_handover,
		  (unsigned long)stats.overwritten,
		  SCREEN_RX_STAGE_AVG(recv_to_crypto),
		  SCREEN_RX_STAGE_AVG(crypto),
		  SCREEN_RX_STAGE_AVG(crypto_to_handover),
		  SCREEN_RX_STAGE_AVG(handover),
		  SCREEN_RX_STAGE_AVG(service),
		  (unsigned long)stats.service_over_16ms,
		  (unsigned long)stats.service_over_33ms);
#undef SCREEN_RX_STAGE_AVG
}

/* In the compact-profiler build no other feature owns lwip_recv.  Full queue
 * profiling and RX limiting call carbox_screen_rx_stage_recv() from their
 * existing wrappers instead, preventing duplicate linker wrappers. */
#if !CONFIG_SCREEN_QUEUE_PROFILE && !CONFIG_SCREEN_RX_RATE_LIMIT
extern ssize_t __real_lwip_recv(int socket, void *buffer, size_t bytes,
				int flags);

ssize_t __wrap_lwip_recv(int socket, void *buffer, size_t bytes, int flags)
{
	ssize_t result = __real_lwip_recv(socket, buffer, bytes, flags);

	carbox_screen_rx_stage_recv(buffer, bytes, (int)result);
	return result;
}
#endif

#else
void carbox_screen_rx_stage_recv(const void *buffer, size_t requested,
				 int result)
{
	(void)buffer; (void)requested; (void)result;
}
void carbox_screen_rx_stage_crypto_begin(void) { }
void carbox_screen_rx_stage_crypto_end(int failed) { (void)failed; }
void carbox_screen_rx_stage_handover_begin(void) { }
void carbox_screen_rx_stage_handover_end(void) { }
void carbox_screen_rx_stage_report(uint32_t sequence) { (void)sequence; }
#endif
