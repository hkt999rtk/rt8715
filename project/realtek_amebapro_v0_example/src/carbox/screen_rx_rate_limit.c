#include "screen_rx_rate_limit.h"
#include "screen_rx_stage_profiler.h"
#include "touch_frame_profiler.h"

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
#define CONFIG_SCREEN_RX_RATE_LIMIT_TASK_STACK 1024U
#endif

#ifndef CONFIG_SCREEN_RX_RATE_LIMIT_PROFILE
#define CONFIG_SCREEN_RX_RATE_LIMIT_PROFILE 0
#endif

#ifndef CONFIG_SCREEN_QUEUE_PROFILE
#define CONFIG_SCREEN_QUEUE_PROFILE 0
#endif

#ifndef CONFIG_VIDEO_INGRESS_PROFILE
#define CONFIG_VIDEO_INGRESS_PROFILE 0
#endif

#if CONFIG_SCREEN_RX_RATE_LIMIT || CONFIG_SCREEN_RX_RATE_LIMIT_PROFILE || \
	CONFIG_VIDEO_INGRESS_PROFILE

static TaskHandle_t screen_rx_task;
#if CONFIG_SCREEN_RX_RATE_LIMIT
static TaskHandle_t screen_rx_limiter_task;
static int screen_rx_socket = -1;
#endif

static uint32_t screen_rx_load_le32(const void *pointer);
static int screen_rx_is_receiver_task(void);
#if CONFIG_SCREEN_RX_RATE_LIMIT_PROFILE
static uint64_t screen_rx_load_le64(const void *pointer);
static uint32_t screen_rx_ntp_delta_us(uint64_t newer, uint64_t older);
static uint64_t screen_source_previous_ntp;
static uint8_t screen_source_previous_valid;
#endif

#if CONFIG_VIDEO_INGRESS_PROFILE
typedef struct video_ingress_stats_s {
	uint32_t recv_calls;
	uint64_t recv_bytes;
	uint64_t recv_wait_sum_us;
	uint32_t recv_wait_max_us;
	uint32_t recv_wait_gt_1ms;
	uint32_t headers;
	uint32_t frames;
	uint64_t header_wait_sum_us;
	uint32_t header_wait_max_us;
	uint64_t assemble_sum_us;
	uint32_t assemble_max_us;
	uint64_t frame_total_sum_us;
	uint32_t frame_total_max_us;
	uint64_t body_bytes;
	uint32_t body_calls;
	uint32_t body_calls_max;
	uint32_t body_mismatch;
	uint32_t interval_samples;
	uint64_t wire_interval_sum_us;
	uint32_t wire_interval_max_us;
	uint32_t source_gap;
	uint32_t wire_gap;
	uint32_t both_gap;
	uint32_t source_only_gap;
	uint32_t wire_only_gap;
	uint32_t transport_extra_max_us;
	uint32_t rx_samples;
	uint32_t rx_errors;
	uint64_t rx_pending_sum;
	uint32_t rx_pending_max;
	uint32_t rx_capacity;
	uint32_t rx_available_min;
	uint32_t rx_advertised_min;
	uint32_t rx_advertised_zero;
} video_ingress_stats_t;

typedef struct video_ingress_state_s {
	int socket;
	uint32_t header_start_us;
	uint32_t header_done_us;
	uint32_t previous_header_done_us;
	uint32_t body_first_start_us;
	uint32_t body_last_done_us;
	uint32_t expected_body_bytes;
	uint32_t received_body_bytes;
	uint32_t body_calls;
	uint8_t previous_header_valid;
	uint8_t frame_active;
} video_ingress_state_t;

static video_ingress_stats_t video_ingress_stats
	__attribute__((section(".lpddr.bss.video_ingress_stats")));
static video_ingress_state_t video_ingress_state = { .socket = -1 };

#define VIDEO_INGRESS_GAP_US 50000U

static uint32_t video_ingress_avg(uint64_t total, uint32_t samples)
{
	return samples != 0U ? (uint32_t)(total / samples) : 0U;
}

static void video_ingress_reset_connection(int socket)
{
	video_ingress_state = (video_ingress_state_t){ .socket = socket };
}

static void video_ingress_snapshot_rx(int socket)
{
	struct lwip_tcp_buffer_diag diag;

	if (lwip_diag_tcp_buffer_state(socket, &diag) != 0) {
		video_ingress_stats.rx_errors++;
		return;
	}
	video_ingress_stats.rx_samples++;
	video_ingress_stats.rx_pending_sum += diag.rx_pending_bytes;
	if (diag.rx_pending_bytes > video_ingress_stats.rx_pending_max) {
		video_ingress_stats.rx_pending_max = diag.rx_pending_bytes;
	}
	video_ingress_stats.rx_capacity = diag.rx_window_capacity;
	if ((video_ingress_stats.rx_available_min == 0U) ||
	    (diag.rx_window_available < video_ingress_stats.rx_available_min)) {
		video_ingress_stats.rx_available_min = diag.rx_window_available;
	}
	if ((video_ingress_stats.rx_advertised_min == 0U) ||
	    (diag.rx_window_advertised < video_ingress_stats.rx_advertised_min)) {
		video_ingress_stats.rx_advertised_min = diag.rx_window_advertised;
	}
	if (diag.rx_window_advertised == 0U) {
		video_ingress_stats.rx_advertised_zero++;
	}
}

static void video_ingress_before_recv(int socket, size_t requested,
				      uint32_t start_us)
{
	if (!screen_rx_is_receiver_task()) {
		return;
	}
	if (socket != video_ingress_state.socket) {
		return;
	}
	if (requested == 128U) {
		video_ingress_state.header_start_us = start_us;
	} else if (video_ingress_state.frame_active &&
		   (video_ingress_state.body_calls == 0U)) {
		video_ingress_state.body_first_start_us = start_us;
	}
}

static void video_ingress_after_recv(int socket, const void *buffer,
				     size_t requested, int result,
				     uint32_t start_us, uint32_t done_us)
{
	uint32_t elapsed_us;

	if (!screen_rx_is_receiver_task()) {
		return;
	}
	elapsed_us = done_us - start_us;

	if ((requested == 128U) && (result == 128) && (buffer != NULL)) {
		const uint8_t *header = (const uint8_t *)buffer;
		uint32_t body_length = screen_rx_load_le32(header);
		uint8_t frame_type = header[4];

		if ((body_length >= 16U) &&
		    (body_length <= (4U * 1024U * 1024U)) &&
		    ((frame_type <= 2U) || (frame_type == 4U) ||
		     (frame_type == 5U))) {
			uint32_t wire_delta_us = 0U;
			uint32_t source_delta_us = 0U;
			uint64_t source_ntp = screen_rx_load_le64(header + 8U);
			int source_valid = 0;

			if (video_ingress_state.socket != socket) {
				video_ingress_reset_connection(socket);
			}
			video_ingress_state.header_start_us = start_us;
			video_ingress_snapshot_rx(socket);

			video_ingress_stats.headers++;
			video_ingress_stats.header_wait_sum_us += elapsed_us;
			if (elapsed_us > video_ingress_stats.header_wait_max_us) {
				video_ingress_stats.header_wait_max_us = elapsed_us;
			}
			if (video_ingress_state.previous_header_valid) {
				wire_delta_us = done_us -
					video_ingress_state.previous_header_done_us;
				video_ingress_stats.interval_samples++;
				video_ingress_stats.wire_interval_sum_us +=
					wire_delta_us;
				if (wire_delta_us >
				    video_ingress_stats.wire_interval_max_us) {
					video_ingress_stats.wire_interval_max_us =
						wire_delta_us;
				}
			}
#if CONFIG_SCREEN_RX_RATE_LIMIT_PROFILE
			if (source_ntp != 0U && screen_source_previous_valid &&
			    source_ntp >= screen_source_previous_ntp) {
				source_delta_us = screen_rx_ntp_delta_us(
					source_ntp, screen_source_previous_ntp);
				source_valid = 1;
			}
#else
			(void)source_ntp;
#endif
			if (video_ingress_state.previous_header_valid && source_valid) {
				int source_gap = source_delta_us > VIDEO_INGRESS_GAP_US;
				int wire_gap = wire_delta_us > VIDEO_INGRESS_GAP_US;
				if (source_gap) video_ingress_stats.source_gap++;
				if (wire_gap) video_ingress_stats.wire_gap++;
				if (source_gap && wire_gap) {
					video_ingress_stats.both_gap++;
				} else if (source_gap) {
					video_ingress_stats.source_only_gap++;
				} else if (wire_gap) {
					video_ingress_stats.wire_only_gap++;
				}
				if ((wire_delta_us > source_delta_us) &&
				    ((wire_delta_us - source_delta_us) >
				     video_ingress_stats.transport_extra_max_us)) {
					video_ingress_stats.transport_extra_max_us =
						wire_delta_us - source_delta_us;
				}
			}
			video_ingress_state.header_done_us = done_us;
			video_ingress_state.previous_header_done_us = done_us;
			video_ingress_state.previous_header_valid = 1U;
			video_ingress_state.expected_body_bytes = body_length;
			video_ingress_state.received_body_bytes = 0U;
			video_ingress_state.body_calls = 0U;
			video_ingress_state.body_first_start_us = 0U;
			video_ingress_state.body_last_done_us = done_us;
			video_ingress_state.frame_active = 1U;
		}
	} else if ((socket == video_ingress_state.socket) &&
		   video_ingress_state.frame_active && (result > 0)) {
		video_ingress_state.received_body_bytes += (uint32_t)result;
		video_ingress_state.body_calls++;
		video_ingress_state.body_last_done_us = done_us;
	}

	if (socket != video_ingress_state.socket) {
		return;
	}
	video_ingress_stats.recv_calls++;
	video_ingress_stats.recv_wait_sum_us += elapsed_us;
	if (elapsed_us > video_ingress_stats.recv_wait_max_us) {
		video_ingress_stats.recv_wait_max_us = elapsed_us;
	}
	if (elapsed_us > 1000U) {
		video_ingress_stats.recv_wait_gt_1ms++;
	}
	if (result > 0) {
		video_ingress_stats.recv_bytes += (uint32_t)result;
	}
}
#endif /* CONFIG_VIDEO_INGRESS_PROFILE */

#if CONFIG_SCREEN_RX_RATE_LIMIT_PROFILE
typedef struct screen_source_cadence_stats_s {
	uint32_t headers;
	uint32_t frames;
	uint32_t paired;
	uint32_t missing;
	uint32_t overwritten;
	uint32_t interval_samples;
	uint64_t interval_sum_us;
	uint32_t interval_min_us;
	uint32_t interval_max_us;
	uint32_t interval_under_12ms;
	uint32_t interval_60fps;
	uint32_t interval_mid;
	uint32_t interval_30fps;
	uint32_t interval_over_38ms;
	uint32_t regressions;
} screen_source_cadence_stats_t;

static screen_source_cadence_stats_t screen_source_stats
	__attribute__((section(".lpddr.bss.screen_source_stats")));
static uint64_t screen_source_pending_ntp;
static uint64_t screen_source_previous_ntp;
static uint8_t screen_source_pending_valid;
static uint8_t screen_source_previous_valid;
#endif

#if CONFIG_SCREEN_RX_RATE_LIMIT
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
#endif

#if CONFIG_SCREEN_RX_RATE_LIMIT
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
#endif

static uint32_t screen_rx_load_le32(const void *pointer)
{
	const uint8_t *p = (const uint8_t *)pointer;

	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#if CONFIG_SCREEN_RX_RATE_LIMIT_PROFILE
static uint64_t screen_rx_load_le64(const void *pointer)
{
	const uint8_t *p = (const uint8_t *)pointer;

	return (uint64_t)screen_rx_load_le32(p) |
		((uint64_t)screen_rx_load_le32(p + 4U) << 32);
}

static uint32_t screen_rx_ntp_delta_us(uint64_t newer, uint64_t older)
{
	uint64_t delta = newer - older;
	uint64_t us = (delta >> 32) * 1000000ULL +
		(((uint64_t)(uint32_t)delta * 1000000ULL) >> 32);

	return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}
#endif

static int screen_rx_is_receiver_task(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();

	if (current == screen_rx_task) return 1;
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

	if ((requested != 128U) || (result != 128) || (buffer == NULL) ||
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

#if CONFIG_SCREEN_RX_RATE_LIMIT_PROFILE
	{
		uint64_t source_ntp = screen_rx_load_le64(header + 8U);

		if (source_ntp != 0U) {
			taskENTER_CRITICAL();
			screen_source_stats.headers++;
			if (screen_source_pending_valid) {
				screen_source_stats.overwritten++;
			}
			screen_source_pending_ntp = source_ntp;
			screen_source_pending_valid = 1U;
			taskEXIT_CRITICAL();
		}
	}
#endif

#if CONFIG_SCREEN_RX_RATE_LIMIT
	if (screen_rx_socket >= 0) {
		return;
	}
	if ((screen_rx_limiter_ensure() == 0) &&
	    (lwip_screen_rx_rate_limit_enable(socket) == 0)) {
		screen_rx_socket = socket;
		xTaskNotifyGive(screen_rx_limiter_task);
#if CONFIG_SCREEN_RX_RATE_LIMIT_PROFILE
		rt_printf("[VIDRXLIMIT] enabled socket=%d target=%lu kbps\r\n",
			  socket,
			  (unsigned long)(CONFIG_SCREEN_RX_RATE_LIMIT_BPS / 1000U));
#endif
	}
#endif
}

void carbox_screen_rx_rate_limit_frame(void)
{
#if CONFIG_VIDEO_INGRESS_PROFILE
	if (video_ingress_state.frame_active) {
		uint32_t now_us = hal_read_curtime_us();
		uint32_t assemble_us = video_ingress_state.body_first_start_us != 0U ?
			video_ingress_state.body_last_done_us -
			video_ingress_state.body_first_start_us : 0U;
		uint32_t total_us = now_us - video_ingress_state.header_done_us;

		video_ingress_stats.frames++;
		video_ingress_stats.assemble_sum_us += assemble_us;
		if (assemble_us > video_ingress_stats.assemble_max_us) {
			video_ingress_stats.assemble_max_us = assemble_us;
		}
		video_ingress_stats.frame_total_sum_us += total_us;
		if (total_us > video_ingress_stats.frame_total_max_us) {
			video_ingress_stats.frame_total_max_us = total_us;
		}
		video_ingress_stats.body_bytes +=
			video_ingress_state.received_body_bytes;
		video_ingress_stats.body_calls += video_ingress_state.body_calls;
		if (video_ingress_state.body_calls >
		    video_ingress_stats.body_calls_max) {
			video_ingress_stats.body_calls_max =
				video_ingress_state.body_calls;
		}
		if (video_ingress_state.received_body_bytes !=
		    video_ingress_state.expected_body_bytes) {
			video_ingress_stats.body_mismatch++;
		}
		video_ingress_state.frame_active = 0U;
	}
#endif
#if CONFIG_SCREEN_RX_RATE_LIMIT_PROFILE
	uint64_t source_ntp;
	uint32_t source_delta_us = 0U;
	int source_delta_valid = 0;

	taskENTER_CRITICAL();
	screen_source_stats.frames++;
	if (!screen_source_pending_valid) {
		screen_source_stats.missing++;
		taskEXIT_CRITICAL();
		return;
	}
	source_ntp = screen_source_pending_ntp;
	screen_source_pending_valid = 0U;
	screen_source_stats.paired++;
	if (screen_source_previous_valid) {
		if (source_ntp < screen_source_previous_ntp) {
			screen_source_stats.regressions++;
		} else {
			uint32_t delta_us = screen_rx_ntp_delta_us(
				source_ntp, screen_source_previous_ntp);
			source_delta_us = delta_us;
			source_delta_valid = 1;

			screen_source_stats.interval_samples++;
			screen_source_stats.interval_sum_us += delta_us;
			if ((screen_source_stats.interval_min_us == 0U) ||
			    (delta_us < screen_source_stats.interval_min_us)) {
				screen_source_stats.interval_min_us = delta_us;
			}
			if (delta_us > screen_source_stats.interval_max_us) {
				screen_source_stats.interval_max_us = delta_us;
			}
			if (delta_us < 12000U) {
				screen_source_stats.interval_under_12ms++;
			} else if (delta_us <= 20000U) {
				screen_source_stats.interval_60fps++;
			} else if (delta_us < 28000U) {
				screen_source_stats.interval_mid++;
			} else if (delta_us <= 38000U) {
				screen_source_stats.interval_30fps++;
			} else {
				screen_source_stats.interval_over_38ms++;
			}
		}
	}
	screen_source_previous_ntp = source_ntp;
	screen_source_previous_valid = 1U;
	taskEXIT_CRITICAL();
	carbox_touch_frame_source_frame(source_delta_us, source_delta_valid);
#endif
}

void carbox_screen_rx_rate_limit_close(int socket)
{
#if CONFIG_VIDEO_INGRESS_PROFILE
	if (socket == video_ingress_state.socket) {
		video_ingress_reset_connection(-1);
		screen_rx_task = NULL;
	}
#endif
#if CONFIG_SCREEN_RX_RATE_LIMIT
	if (socket == screen_rx_socket) {
		screen_rx_socket = -1;
		screen_rx_task = NULL;
#if CONFIG_SCREEN_RX_RATE_LIMIT_PROFILE
		screen_source_pending_valid = 0U;
		screen_source_previous_valid = 0U;
#endif
	}
#else
	(void)socket;
#endif
}

void carbox_screen_rx_rate_limit_report(uint32_t sequence)
{
#if CONFIG_SCREEN_RX_RATE_LIMIT
	struct lwip_screen_rx_rate_limit_diag diag;
	uint32_t rate_kbps;
	uint32_t pending_avg;
#endif
#if CONFIG_SCREEN_RX_RATE_LIMIT_PROFILE
	screen_source_cadence_stats_t source_stats;
#endif
#if CONFIG_VIDEO_INGRESS_PROFILE
	video_ingress_stats_t ingress;
#endif

#if CONFIG_SCREEN_RX_RATE_LIMIT
	if (lwip_diag_screen_rx_rate_limit(&diag, 1) != 0) {
		memset(&diag, 0, sizeof(diag));
	}
	rate_kbps = diag.elapsed_ms != 0U ?
		(uint32_t)(((uint64_t)diag.granted_bytes * 8U) /
			   diag.elapsed_ms) : 0U;
	pending_avg = diag.pending_ticks != 0U ?
		(uint32_t)(diag.pending_sum_bytes / diag.pending_ticks) : 0U;
	rt_printf("[VIDRXLIMIT][%lu] target/rate=%lu/%lu kbps "
		  "requested/granted=%lu/%luB pending/max=%lu/%luB "
		  "tokens=%lu/%luB valve/filtered=%lu/%lu kbps "
		  "adjust open/close/pressure=%lu/%lu/%lu "
		  "ticks total/pending/grant=%lu/%lu/%lu "
		  "limited token/valve/zero=%lu/%lu/%lu "
		  "pending_avg=%luB active=%lu\r\n",
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
		  (unsigned long)diag.total_ticks,
		  (unsigned long)diag.pending_ticks,
		  (unsigned long)diag.grant_ticks,
		  (unsigned long)diag.token_limited_ticks,
		  (unsigned long)diag.valve_limited_ticks,
		  (unsigned long)diag.zero_grant_ticks,
		  (unsigned long)pending_avg,
		  (unsigned long)diag.active);
#endif

#if CONFIG_VIDEO_INGRESS_PROFILE
	taskENTER_CRITICAL();
	ingress = video_ingress_stats;
	video_ingress_stats = (video_ingress_stats_t){ 0 };
	taskEXIT_CRITICAL();
	if ((ingress.headers != 0U) || (ingress.recv_calls != 0U)) {
		rt_printf("[VIDEOINGRESS][%lu] recv calls/bytes=%lu/%llu "
			  "wait_us avg/max/>1ms=%lu/%lu/%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)ingress.recv_calls,
			  (unsigned long long)ingress.recv_bytes,
			  (unsigned long)video_ingress_avg(
				ingress.recv_wait_sum_us, ingress.recv_calls),
			  (unsigned long)ingress.recv_wait_max_us,
			  (unsigned long)ingress.recv_wait_gt_1ms);
		rt_printf("[VIDEOINGRESS][%lu] headers/frames=%lu/%lu "
			  "header_wait_us avg/max=%lu/%lu assembly_us avg/max=%lu/%lu "
			  "header_to_callback_us avg/max=%lu/%lu body calls/avg/max="
			  "%lu/%lu/%lu bytes=%llu mismatch=%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)ingress.headers,
			  (unsigned long)ingress.frames,
			  (unsigned long)video_ingress_avg(
				ingress.header_wait_sum_us, ingress.headers),
			  (unsigned long)ingress.header_wait_max_us,
			  (unsigned long)video_ingress_avg(
				ingress.assemble_sum_us, ingress.frames),
			  (unsigned long)ingress.assemble_max_us,
			  (unsigned long)video_ingress_avg(
				ingress.frame_total_sum_us, ingress.frames),
			  (unsigned long)ingress.frame_total_max_us,
			  (unsigned long)ingress.body_calls,
			  (unsigned long)video_ingress_avg(
				ingress.body_calls, ingress.frames),
			  (unsigned long)ingress.body_calls_max,
			  (unsigned long long)ingress.body_bytes,
			  (unsigned long)ingress.body_mismatch);
		rt_printf("[VIDEOINGRESS][%lu] wire_interval_us samples/avg/max="
			  "%lu/%lu/%lu gap source/wire/both/source_only/wire_only="
			  "%lu/%lu/%lu/%lu/%lu transport_extra_max_us=%lu "
			  "rx samples/error="
			  "%lu/%lu pending_avg/max=%lu/%luB wnd avail_min/adv_min/"
			  "cap/adv_zero=%lu/%lu/%lu/%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)ingress.interval_samples,
			  (unsigned long)video_ingress_avg(
				ingress.wire_interval_sum_us,
				ingress.interval_samples),
			  (unsigned long)ingress.wire_interval_max_us,
			  (unsigned long)ingress.source_gap,
			  (unsigned long)ingress.wire_gap,
			  (unsigned long)ingress.both_gap,
			  (unsigned long)ingress.source_only_gap,
			  (unsigned long)ingress.wire_only_gap,
			  (unsigned long)ingress.transport_extra_max_us,
			  (unsigned long)ingress.rx_samples,
			  (unsigned long)ingress.rx_errors,
			  (unsigned long)video_ingress_avg(
				ingress.rx_pending_sum, ingress.rx_samples),
			  (unsigned long)ingress.rx_pending_max,
			  (unsigned long)ingress.rx_available_min,
			  (unsigned long)ingress.rx_advertised_min,
			  (unsigned long)ingress.rx_capacity,
			  (unsigned long)ingress.rx_advertised_zero);
	}
#endif

#if CONFIG_SCREEN_RX_RATE_LIMIT_PROFILE
	taskENTER_CRITICAL();
	source_stats = screen_source_stats;
	screen_source_stats = (screen_source_cadence_stats_t){ 0 };
	taskEXIT_CRITICAL();
	if (source_stats.frames != 0U) {
		const char *source_max_fps =
			source_stats.interval_60fps >= 3U ? "60" :
			(source_stats.interval_30fps >= 3U ? "30" : "unknown");

		rt_printf("[IPHONEFPS][%lu] source_ntp headers/frames/paired/missing/overwrite="
			  "%lu/%lu/%lu/%lu/%lu interval_us samples/avg/min/max="
			  "%lu/%llu/%lu/%lu bins <12/12-20/20-28/28-38/>38ms="
			  "%lu/%lu/%lu/%lu/%lu regress=%lu source_max_fps=%s\r\n",
			  (unsigned long)sequence,
			  (unsigned long)source_stats.headers,
			  (unsigned long)source_stats.frames,
			  (unsigned long)source_stats.paired,
			  (unsigned long)source_stats.missing,
			  (unsigned long)source_stats.overwritten,
			  (unsigned long)source_stats.interval_samples,
			  (unsigned long long)(source_stats.interval_samples != 0U ?
				source_stats.interval_sum_us /
				source_stats.interval_samples : 0U),
			  (unsigned long)source_stats.interval_min_us,
			  (unsigned long)source_stats.interval_max_us,
			  (unsigned long)source_stats.interval_under_12ms,
			  (unsigned long)source_stats.interval_60fps,
			  (unsigned long)source_stats.interval_mid,
			  (unsigned long)source_stats.interval_30fps,
			  (unsigned long)source_stats.interval_over_38ms,
			  (unsigned long)source_stats.regressions,
			  source_max_fps);
	}
#endif
}

#if !CONFIG_SCREEN_QUEUE_PROFILE
extern ssize_t __real_lwip_recv(int socket, void *buffer, size_t bytes,
				int flags);

ssize_t __wrap_lwip_recv(int socket, void *buffer, size_t bytes, int flags)
{
	uint32_t start_us = hal_read_curtime_us();
#if CONFIG_VIDEO_INGRESS_PROFILE
	video_ingress_before_recv(socket, bytes, start_us);
#endif
	ssize_t result = __real_lwip_recv(socket, buffer, bytes, flags);
	uint32_t done_us = hal_read_curtime_us();
	carbox_screen_rx_stage_recv(buffer, bytes, (int)result);

	carbox_screen_rx_rate_limit_observe(socket, buffer, bytes, (int)result);
#if CONFIG_VIDEO_INGRESS_PROFILE
	video_ingress_after_recv(socket, buffer, bytes, (int)result,
		start_us, done_us);
#else
	(void)start_us;
	(void)done_us;
#endif
	return result;
}
#endif

#if CONFIG_SCREEN_RX_RATE_LIMIT
extern int __real_lwip_close(int socket);

int __wrap_lwip_close(int socket)
{
	int result = __real_lwip_close(socket);

	if (result == 0) {
		carbox_screen_rx_rate_limit_close(socket);
	}
	return result;
}
#endif

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

void carbox_screen_rx_rate_limit_frame(void)
{
}

void carbox_screen_rx_rate_limit_report(uint32_t sequence)
{
	(void)sequence;
}

#endif
