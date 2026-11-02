/* Screen TCP backpressure observer for builds without direct crypto. */
#include "screen_tx_probe.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "hal_timer.h"
#include "lwip/err.h"
#include "lwip/priv/tcp_wake_trace.h"
#include "lwip/sockets.h"
#include "lwip_intf.h"

#define SCREEN_TX_SAMPLE_INTERVAL_US 250000U
#define SCREEN_TX_TRACK_INTERVAL_US 10000000U

typedef struct screen_tx_probe_stats_s {
	uint32_t writes;
	uint32_t bytes;
	uint32_t partial;
	uint32_t eagain;
	uint32_t other_error;
	uint32_t episodes;
	uint32_t recoveries;
	uint32_t blocked_sum_us;
	uint32_t blocked_max_us;
	uint32_t write_max_us;
	uint32_t write_over_10ms;
	uint32_t tcp_samples;
	uint32_t tcp_sndbuf_min;
	uint32_t tcp_qlen_max;
	uint32_t tcp_unsent_max;
	uint32_t tcp_unacked_max;
	uint32_t tcp_wnd_avail_min;
	uint32_t ncm_samples;
	uint32_t ncm_input_max;
	uint32_t ncm_inflight_max;
	uint32_t ncm_inflight_age_max_us;
} screen_tx_probe_stats_t;

static screen_tx_probe_stats_t screen_tx_stats = {
	.tcp_sndbuf_min = UINT32_MAX,
	.tcp_wnd_avail_min = UINT32_MAX,
};
static TaskHandle_t screen_tx_task;
static int screen_tx_socket = -1;
static uint32_t screen_tx_last_track_us;
static uint32_t screen_tx_last_sample_us;
static uint32_t screen_tx_block_start_us;
static uint8_t screen_tx_blocked;

/* Only ScreenThread enters the normal-frame sender.  A single active record
 * links the customer's EAGAIN print, poll wait and write calls to a frame. */
typedef struct screen_frame_probe_s {
	uint32_t id;
	uint32_t started_us;
	uint32_t length;
	uint32_t writes;
	uint32_t accepted_bytes;
	uint32_t partial;
	uint32_t eagain;
	uint32_t poll_ready;
	uint32_t poll_timeout;
	uint32_t poll_error;
	uint32_t poll_other;
	uint32_t poll_sum_us;
	uint32_t poll_max_us;
} screen_frame_probe_t;

static screen_frame_probe_t screen_frame;
static uint32_t screen_frame_next_id;

extern ssize_t __real_lwip_write(int socket, const void *buffer, size_t bytes);
extern void AirPlayScreen_SendScreenNormalFrame(const void *data, int length);

void carbox_screen_normal_frame_probe(const void *data, int length)
{
	screen_frame_probe_t finished;
	uint32_t elapsed_us;

	screen_frame = (screen_frame_probe_t){
		.id = ++screen_frame_next_id,
		.started_us = hal_read_curtime_us(),
		.length = length > 0 ? (uint32_t)length : 0U,
	};
	AirPlayScreen_SendScreenNormalFrame(data, length);
#if CONFIG_SCREEN_WAKE_TRACE
	tcp_wake_frame_end(screen_tx_socket);
#endif
	finished = screen_frame;
	elapsed_us = hal_read_curtime_us() - finished.started_us;
	screen_frame.id = 0U;
	if (finished.eagain == 0U) return;
	rt_printf("[SCREENFRAME] id=%lu len=%lu writes=%lu accepted=%lu "
		  "partial=%lu eagain=%lu elapsed_ms=%lu "
		  "poll writable/timeout/error/other=%lu/%lu/%lu/%lu "
		  "wait_ms sum/max=%lu/%lu\r\n",
		  (unsigned long)finished.id, (unsigned long)finished.length,
		  (unsigned long)finished.writes,
		  (unsigned long)finished.accepted_bytes,
		  (unsigned long)finished.partial,
		  (unsigned long)finished.eagain,
		  (unsigned long)(elapsed_us / 1000U),
		  (unsigned long)finished.poll_ready,
		  (unsigned long)finished.poll_timeout,
		  (unsigned long)finished.poll_error,
		  (unsigned long)finished.poll_other,
		  (unsigned long)(finished.poll_sum_us / 1000U),
		  (unsigned long)(finished.poll_max_us / 1000U));
}

int carbox_screen_block_note(const char *message)
{
	if (screen_frame.id == 0U) return puts(message);
	return rt_printf("screen block frame_id=%lu retry=%lu age_ms=%lu\r\n",
			 (unsigned long)screen_frame.id,
			 (unsigned long)screen_frame.eagain,
			 (unsigned long)((hal_read_curtime_us() -
					 screen_frame.started_us) / 1000U));
}

void carbox_screen_tx_poll_observe(int result, int revents, uint32_t wait_us)
{
	if (screen_frame.id == 0U) return;
	if (result < 0 || (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
		screen_frame.poll_error++;
	else if (result == 0)
		screen_frame.poll_timeout++;
	else if ((revents & POLLOUT) != 0)
		screen_frame.poll_ready++;
	else
		screen_frame.poll_other++;
	screen_frame.poll_sum_us += wait_us;
	if (wait_us > screen_frame.poll_max_us)
		screen_frame.poll_max_us = wait_us;
}

static int screen_tx_is_sender(void)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();

	if (task == screen_tx_task) return 1;
	if (strcmp(pcTaskGetTaskName(task), "ScreenThread") != 0) return 0;
	screen_tx_task = task;
	return 1;
}

static void screen_tx_max(uint32_t *value, uint32_t sample)
{
	if (sample > *value) *value = sample;
}

ssize_t __wrap_lwip_write(int socket, const void *buffer, size_t bytes)
{
	ssize_t result;
	uint32_t start_us;
	uint32_t now_us;
	uint32_t write_us;
	int error;
	int sample = 0;
	uint32_t first_stall_id = 0U;
	struct lwip_tcp_buffer_diag tcp;
	rltk_ncm_tx_diag_t ncm;
	int tcp_ok = -1;
	int ncm_ok = -1;

	if (!screen_tx_is_sender()) return __real_lwip_write(socket, buffer, bytes);
#if defined(CONFIG_SCREEN_TCP_ACK_PROFILE) && CONFIG_SCREEN_TCP_ACK_PROFILE
	/* Track once per connection, then periodically to handle socket reuse. */
	if (socket != screen_tx_socket ||
	    (screen_frame.id != 0U && screen_frame.writes == 0U) ||
	    (uint32_t)(hal_read_curtime_us() - screen_tx_last_track_us) >=
	    SCREEN_TX_TRACK_INTERVAL_US) {
		(void)lwip_diag_screen_tcp_ack_track(socket);
		screen_tx_socket = socket;
		screen_tx_last_track_us = hal_read_curtime_us();
	}
#else
	(void)screen_tx_socket;
	(void)screen_tx_last_track_us;
#endif
#if CONFIG_SCREEN_WAKE_TRACE
	tcp_wake_write_begin(socket, screen_frame.id);
#endif
	start_us = hal_read_curtime_us();
	result = __real_lwip_write(socket, buffer, bytes);
	error = result < 0 ? lwip_err : 0;
#if CONFIG_SCREEN_WAKE_TRACE
	tcp_wake_write_end(socket, (int)result, error);
#endif
	now_us = hal_read_curtime_us();
	write_us = now_us - start_us;
	if (screen_frame.id != 0U) {
		screen_frame.writes++;
		if (result >= 0) {
			screen_frame.accepted_bytes += (uint32_t)result;
			screen_frame.partial += (size_t)result < bytes;
		} else if (error == EAGAIN || error == EWOULDBLOCK) {
			screen_frame.eagain++;
			if (screen_frame.eagain == 1U)
				first_stall_id = screen_frame.id;
		}
	}


	if (result < 0 && (error == EAGAIN || error == EWOULDBLOCK) &&
	    (first_stall_id != 0U || screen_tx_last_sample_us == 0U ||
	     (uint32_t)(now_us - screen_tx_last_sample_us) >=
	     SCREEN_TX_SAMPLE_INTERVAL_US)) {
		screen_tx_last_sample_us = now_us;
		sample = 1;
		tcp_ok = lwip_diag_tcp_buffer_state(socket, &tcp);
		ncm_ok = rltk_ncm_tx_diag_snapshot(&ncm);
	}

	taskENTER_CRITICAL();
	screen_tx_stats.writes++;
	screen_tx_max(&screen_tx_stats.write_max_us, write_us);
	screen_tx_stats.write_over_10ms += write_us > 10000U;
	if (result >= 0) {
		screen_tx_stats.bytes += (uint32_t)result;
		screen_tx_stats.partial += (size_t)result < bytes;
		if (screen_tx_blocked) {
			uint32_t blocked_us = now_us - screen_tx_block_start_us;
			screen_tx_stats.recoveries++;
			screen_tx_stats.blocked_sum_us += blocked_us;
			screen_tx_max(&screen_tx_stats.blocked_max_us, blocked_us);
			screen_tx_blocked = 0U;
		}
	} else if (error == EAGAIN || error == EWOULDBLOCK) {
		screen_tx_stats.eagain++;
		if (!screen_tx_blocked) {
			screen_tx_blocked = 1U;
			screen_tx_block_start_us = now_us;
			screen_tx_stats.episodes++;
		}
	} else {
		screen_tx_stats.other_error++;
		screen_tx_blocked = 0U;
	}
	if (sample && tcp_ok == 0) {
		uint32_t wnd_avail = tcp.tx_send_window > tcp.tx_unacked_bytes ?
			tcp.tx_send_window - tcp.tx_unacked_bytes : 0U;
		screen_tx_stats.tcp_samples++;
		if (tcp.tx_buffer_available < screen_tx_stats.tcp_sndbuf_min)
			screen_tx_stats.tcp_sndbuf_min = tcp.tx_buffer_available;
		if (wnd_avail < screen_tx_stats.tcp_wnd_avail_min)
			screen_tx_stats.tcp_wnd_avail_min = wnd_avail;
		screen_tx_max(&screen_tx_stats.tcp_qlen_max, tcp.tx_queue_len);
		screen_tx_max(&screen_tx_stats.tcp_unsent_max,
			      tcp.tx_unsent_bytes);
		screen_tx_max(&screen_tx_stats.tcp_unacked_max,
			      tcp.tx_unacked_bytes);
	}
	if (sample && ncm_ok == 0) {
		screen_tx_stats.ncm_samples++;
		screen_tx_max(&screen_tx_stats.ncm_input_max, ncm.queue_depth);
		screen_tx_max(&screen_tx_stats.ncm_inflight_max,
			      ncm.inflight_packets);
		screen_tx_max(&screen_tx_stats.ncm_inflight_age_max_us,
			      ncm.inflight_age_us);
	}
	taskEXIT_CRITICAL();
	if (first_stall_id != 0U && tcp_ok == 0) {
		rt_printf("[SCREENFRAME][STALL] id=%lu tcp sndbuf/qlen/unsent/"
			  "unacked/wnd/cwnd/mss=%lu/%lu/%lu/%lu/%lu/%lu/%lu "
			  "ncm input/inflight/age_us=%lu/%lu/%lu\r\n",
			  (unsigned long)first_stall_id,
			  (unsigned long)tcp.tx_buffer_available,
			  (unsigned long)tcp.tx_queue_len,
			  (unsigned long)tcp.tx_unsent_bytes,
			  (unsigned long)tcp.tx_unacked_bytes,
			  (unsigned long)tcp.tx_send_window,
			  (unsigned long)tcp.tx_congestion_window,
			  (unsigned long)tcp.tx_mss,
			  (unsigned long)(ncm_ok == 0 ? ncm.queue_depth : UINT32_MAX),
			  (unsigned long)(ncm_ok == 0 ? ncm.inflight_packets : UINT32_MAX),
			  (unsigned long)(ncm_ok == 0 ? ncm.inflight_age_us : UINT32_MAX));
	}
	if (result < 0) lwip_err = error;
	return result;
}

void carbox_screen_tx_probe_report(uint32_t sequence)
{
	screen_tx_probe_stats_t stats;
	uint32_t pending_age_us;

	taskENTER_CRITICAL();
	stats = screen_tx_stats;
	pending_age_us = screen_tx_blocked ?
		hal_read_curtime_us() - screen_tx_block_start_us : 0U;
	screen_tx_stats = (screen_tx_probe_stats_t){
		.tcp_sndbuf_min = UINT32_MAX,
		.tcp_wnd_avail_min = UINT32_MAX,
	};
	taskEXIT_CRITICAL();
	if (stats.writes == 0U && pending_age_us == 0U) return;
	rt_printf("[SCREENTX][%lu] write=%lu bytes=%lu partial=%lu "
		  "eagain/episode/recover/error=%lu/%lu/%lu/%lu "
		  "block_us avg/max/pending=%lu/%lu/%lu "
		  "write_us max/over10ms=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.writes,
		  (unsigned long)stats.bytes,
		  (unsigned long)stats.partial,
		  (unsigned long)stats.eagain,
		  (unsigned long)stats.episodes,
		  (unsigned long)stats.recoveries,
		  (unsigned long)stats.other_error,
		  (unsigned long)(stats.recoveries ?
			stats.blocked_sum_us / stats.recoveries : 0U),
		  (unsigned long)stats.blocked_max_us,
		  (unsigned long)pending_age_us,
		  (unsigned long)stats.write_max_us,
		  (unsigned long)stats.write_over_10ms);
	if (stats.tcp_samples == 0U) return;
	rt_printf("[SCREENTX][%lu] samples=%lu tcp sndbuf_min/qlen_max/"
		  "unsent_max/unacked_max/wnd_avail_min=%lu/%lu/%lu/%lu/%lu "
		  "ncm samples/input_max/inflight_max/inflight_age_max_us="
		  "%lu/%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.tcp_samples,
		  (unsigned long)stats.tcp_sndbuf_min,
		  (unsigned long)stats.tcp_qlen_max,
		  (unsigned long)stats.tcp_unsent_max,
		  (unsigned long)stats.tcp_unacked_max,
		  (unsigned long)stats.tcp_wnd_avail_min,
		  (unsigned long)stats.ncm_samples,
		  (unsigned long)stats.ncm_input_max,
		  (unsigned long)stats.ncm_inflight_max,
		  (unsigned long)stats.ncm_inflight_age_max_us);
}
