#include "touch_frame_profiler.h"

#ifndef CONFIG_TOUCH_FRAME_PROFILE
#define CONFIG_TOUCH_FRAME_PROFILE 0
#endif

#ifndef CONFIG_TOUCH_PATH_PROFILE
#define CONFIG_TOUCH_PATH_PROFILE 0
#endif

#if CONFIG_TOUCH_FRAME_PROFILE

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "hal_timer.h"
#include "lwip/sockets.h"

#define TOUCH_FRAME_HTTP_SLOTS       8U
#define TOUCH_FRAME_HTTP_STALE_US    30000000U
#define TOUCH_FRAME_WOULD_BLOCK      11

typedef struct touch_frame_stage_s {
	uint64_t sum_us;
	uint32_t max_us;
	uint32_t samples;
} touch_frame_stage_t;

typedef struct touch_frame_stats_s {
	uint32_t hid_calls;
	uint32_t hid_errors;
	touch_frame_stage_t hid_interval;
	uint32_t hid_interval_gt_20ms;
	uint32_t hid_interval_gt_33ms;
	touch_frame_stage_t hid_call;
	uint32_t dispatch_calls;
	uint32_t dispatch_missing;
	touch_frame_stage_t dispatch_wait;
	touch_frame_stage_t dispatch_exec;
	touch_frame_stage_t dispatch_total;
	uint32_t dispatch_wait_ge_1ms;
	uint32_t dispatch_wait_ge_5ms;
	uint32_t dispatch_wait_ge_10ms;
	uint32_t http_enqueued;
	uint32_t http_enqueue_errors;
	uint32_t http_slot_full;
	uint32_t http_stale;
	uint32_t http_write_calls;
	uint32_t http_write_complete;
	uint32_t http_write_deferred;
	uint32_t http_write_errors;
	uint32_t tcp_calls;
	uint32_t tcp_errors;
	uint32_t tcp_partial;
	uint32_t tcp_requested_bytes;
	uint32_t tcp_accepted_bytes;
	touch_frame_stage_t hid_to_http_enqueue;
	touch_frame_stage_t enqueue_to_write;
	touch_frame_stage_t http_write_total;
	touch_frame_stage_t hid_to_tcp_accept;
	touch_frame_stage_t tcp_call;
	uint32_t source_normal;
	uint32_t source_gap;
	uint32_t normal_with_hid;
	uint32_t normal_without_hid;
	uint32_t gap_with_hid;
	uint32_t gap_without_hid;
	uint32_t normal_gcd_ge_5ms;
	uint32_t gap_gcd_ge_1ms;
	uint32_t gap_gcd_ge_5ms;
	uint32_t gap_gcd_ge_10ms;
	uint32_t normal_with_http;
	uint32_t normal_without_http;
	uint32_t gap_with_http;
	uint32_t gap_without_http;
	uint32_t normal_with_tcp;
	uint32_t normal_without_tcp;
	uint32_t gap_with_tcp;
	uint32_t gap_without_tcp;
	uint32_t gap_tcp_ge_5ms;
	uint32_t gap_tcp_ge_10ms;
	uint32_t interval_hid_sum;
	uint32_t interval_hid_max;
	touch_frame_stage_t hid_to_frame;
} touch_frame_stats_t;

typedef struct touch_frame_http_slot_s {
	void *message;
	uint32_t hid_start_us;
	uint32_t enqueue_us;
	uint32_t write_start_us;
	uint32_t first_tcp_accept_us;
	uint8_t active;
	uint8_t write_started;
	uint8_t tcp_accepted;
} touch_frame_http_slot_t;

typedef struct touch_frame_state_s {
	TaskHandle_t hid_task;
	uint32_t hid_start_us;
	uint32_t previous_hid_start_us;
	uint32_t last_hid_complete_us;
	uint32_t dispatch_token_next;
	uint32_t dispatch_token_active;
	uint32_t hid_since_source;
	uint32_t max_gcd_wait_since_source;
	uint32_t http_complete_since_source;
	uint32_t tcp_accept_since_source;
	uint32_t max_hid_to_tcp_since_source;
	TaskHandle_t dispatch_callback_task;
	TaskHandle_t http_write_task;
	touch_frame_http_slot_t *http_write_slot;
	uint8_t hid_active;
	uint8_t dispatch_callback_active;
	uint8_t previous_hid_valid;
	uint8_t last_hid_complete_valid;
} touch_frame_state_t;

static touch_frame_stats_t touch_frame_stats
	__attribute__((section(".lpddr.bss.touch_frame_stats")));
static touch_frame_state_t touch_frame_state
	__attribute__((section(".lpddr.bss.touch_frame_state")));
static touch_frame_http_slot_t touch_frame_http_slots[TOUCH_FRAME_HTTP_SLOTS]
	__attribute__((section(".lpddr.bss.touch_frame_http_slots")));

static void touch_frame_stage_add(touch_frame_stage_t *stage,
				  uint32_t elapsed_us)
{
	stage->sum_us += elapsed_us;
	stage->samples++;
	if (elapsed_us > stage->max_us) {
		stage->max_us = elapsed_us;
	}
}

static touch_frame_http_slot_t *touch_frame_http_find(void *message)
{
	uint32_t i;

	for (i = 0U; i < TOUCH_FRAME_HTTP_SLOTS; i++) {
		if (touch_frame_http_slots[i].active &&
		    (touch_frame_http_slots[i].message == message)) {
			return &touch_frame_http_slots[i];
		}
	}
	return NULL;
}

static touch_frame_http_slot_t *touch_frame_http_allocate(
	void *message, uint32_t hid_start_us, uint32_t enqueue_us)
{
	uint32_t i;
	touch_frame_http_slot_t *slot = NULL;

	for (i = 0U; i < TOUCH_FRAME_HTTP_SLOTS; i++) {
		if (touch_frame_http_slots[i].active &&
		    ((enqueue_us - touch_frame_http_slots[i].enqueue_us) >
		     TOUCH_FRAME_HTTP_STALE_US)) {
			memset(&touch_frame_http_slots[i], 0,
			       sizeof(touch_frame_http_slots[i]));
			touch_frame_stats.http_stale++;
		}
		if (!touch_frame_http_slots[i].active && (slot == NULL)) {
			slot = &touch_frame_http_slots[i];
		}
	}
	if (slot != NULL) {
		memset(slot, 0, sizeof(*slot));
		slot->message = message;
		slot->hid_start_us = hid_start_us;
		slot->enqueue_us = enqueue_us;
		slot->active = 1U;
	}
	return slot;
}

#if !CONFIG_TOUCH_PATH_PROFILE
extern int32_t __real_AirPlayReceiverSessionSendHIDReport(
	void *session, uint32_t device_uid, const void *report, uint32_t length);

int32_t __wrap_AirPlayReceiverSessionSendHIDReport(
	void *session, uint32_t device_uid, const void *report, uint32_t length)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t start_us = hal_read_curtime_us();
	uint32_t end_us;
	int32_t result;

	/* The existing bridge identifies the 5-byte HID report as touch. */
	if (length != 5U) {
		return __real_AirPlayReceiverSessionSendHIDReport(
			session, device_uid, report, length);
	}

	taskENTER_CRITICAL();
	touch_frame_stats.hid_calls++;
	if (touch_frame_state.previous_hid_valid) {
		uint32_t interval_us = start_us -
			touch_frame_state.previous_hid_start_us;

		touch_frame_stage_add(&touch_frame_stats.hid_interval,
			interval_us);
		touch_frame_stats.hid_interval_gt_20ms +=
			(interval_us > 20000U);
		touch_frame_stats.hid_interval_gt_33ms +=
			(interval_us > 33334U);
	}
	touch_frame_state.previous_hid_start_us = start_us;
	touch_frame_state.previous_hid_valid = 1U;
	touch_frame_state.hid_since_source++;
	touch_frame_state.hid_task = current;
	touch_frame_state.hid_start_us = start_us;
	touch_frame_state.hid_active = 1U;
	taskEXIT_CRITICAL();

	result = __real_AirPlayReceiverSessionSendHIDReport(
		session, device_uid, report, length);
	end_us = hal_read_curtime_us();

	taskENTER_CRITICAL();
	touch_frame_stage_add(&touch_frame_stats.hid_call, end_us - start_us);
	touch_frame_stats.hid_errors += (result != 0);
	touch_frame_state.last_hid_complete_us = end_us;
	touch_frame_state.last_hid_complete_valid = 1U;
	if (touch_frame_state.hid_task == current) {
		touch_frame_state.hid_active = 0U;
		touch_frame_state.hid_task = NULL;
	}
	taskEXIT_CRITICAL();
	return result;
}
#endif

uint32_t carbox_touch_frame_dispatch_begin(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t token = 0U;

	taskENTER_CRITICAL();
	if (touch_frame_state.hid_active &&
	    (touch_frame_state.hid_task == current)) {
		token = ++touch_frame_state.dispatch_token_next;
		if (token == 0U) {
			token = ++touch_frame_state.dispatch_token_next;
		}
		touch_frame_state.dispatch_token_active = token;
		touch_frame_stats.dispatch_calls++;
	}
	taskEXIT_CRITICAL();
	return token;
}

void carbox_touch_frame_dispatch_callback_begin(uint32_t token)
{
	taskENTER_CRITICAL();
	if ((token != 0U) &&
	    (touch_frame_state.dispatch_token_active == token)) {
		touch_frame_state.dispatch_callback_task =
			xTaskGetCurrentTaskHandle();
		touch_frame_state.dispatch_callback_active = 1U;
	}
	taskEXIT_CRITICAL();
}

void carbox_touch_frame_dispatch_callback_end(uint32_t token)
{
	taskENTER_CRITICAL();
	if ((token != 0U) &&
	    (touch_frame_state.dispatch_token_active == token)) {
		touch_frame_state.dispatch_callback_active = 0U;
		touch_frame_state.dispatch_callback_task = NULL;
	}
	taskEXIT_CRITICAL();
}

void carbox_touch_frame_dispatch_complete(uint32_t token,
					  uint32_t submit_us,
					  uint32_t callback_start_us,
					  uint32_t callback_end_us,
					  uint32_t return_us,
					  int callback_called)
{
	uint32_t wait_us;

	if (token == 0U) {
		return;
	}
	taskENTER_CRITICAL();
	if (touch_frame_state.dispatch_token_active != token) {
		taskEXIT_CRITICAL();
		return;
	}
	touch_frame_state.dispatch_token_active = 0U;
	if (!callback_called) {
		touch_frame_stats.dispatch_missing++;
		taskEXIT_CRITICAL();
		return;
	}
	wait_us = callback_start_us - submit_us;
	touch_frame_stage_add(&touch_frame_stats.dispatch_wait, wait_us);
	touch_frame_stage_add(&touch_frame_stats.dispatch_exec,
		callback_end_us - callback_start_us);
	touch_frame_stage_add(&touch_frame_stats.dispatch_total,
		return_us - submit_us);
	touch_frame_stats.dispatch_wait_ge_1ms += (wait_us >= 1000U);
	touch_frame_stats.dispatch_wait_ge_5ms += (wait_us >= 5000U);
	touch_frame_stats.dispatch_wait_ge_10ms += (wait_us >= 10000U);
	if (wait_us > touch_frame_state.max_gcd_wait_since_source) {
		touch_frame_state.max_gcd_wait_since_source = wait_us;
	}
	taskEXIT_CRITICAL();
}

extern int32_t __real_HTTPClientSendMessage(void *client, void *message);

int32_t __wrap_HTTPClientSendMessage(void *client, void *message)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t enqueue_us = hal_read_curtime_us();
	int matched;
	int32_t result;

	taskENTER_CRITICAL();
	matched = touch_frame_state.dispatch_callback_active &&
		(touch_frame_state.dispatch_callback_task == current);
	if (matched) {
		if (touch_frame_http_allocate(message,
				touch_frame_state.hid_start_us,
				enqueue_us) != NULL) {
			touch_frame_stats.http_enqueued++;
			touch_frame_stage_add(
				&touch_frame_stats.hid_to_http_enqueue,
				enqueue_us - touch_frame_state.hid_start_us);
		} else {
			touch_frame_stats.http_slot_full++;
			matched = 0;
		}
	}
	taskEXIT_CRITICAL();

	result = __real_HTTPClientSendMessage(client, message);
	if (matched && (result != 0)) {
		taskENTER_CRITICAL();
		{
			touch_frame_http_slot_t *slot =
				touch_frame_http_find(message);

			if (slot != NULL) {
				memset(slot, 0, sizeof(*slot));
			}
			touch_frame_stats.http_enqueue_errors++;
		}
		taskEXIT_CRITICAL();
	}
	return result;
}

extern int32_t __real_HTTPMessageWriteMessage(
	void *message, void *write_f, void *write_context);

int32_t __wrap_HTTPMessageWriteMessage(
	void *message, void *write_f, void *write_context)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	touch_frame_http_slot_t *slot;
	uint32_t start_us = hal_read_curtime_us();
	int matched = 0;
	int32_t result;

	taskENTER_CRITICAL();
	slot = touch_frame_http_find(message);
	if (slot != NULL) {
		matched = 1;
		touch_frame_stats.http_write_calls++;
		if (!slot->write_started) {
			slot->write_started = 1U;
			slot->write_start_us = start_us;
			touch_frame_stage_add(&touch_frame_stats.enqueue_to_write,
				start_us - slot->enqueue_us);
		}
		touch_frame_state.http_write_task = current;
		touch_frame_state.http_write_slot = slot;
	}
	taskEXIT_CRITICAL();

	result = __real_HTTPMessageWriteMessage(message, write_f, write_context);

	taskENTER_CRITICAL();
	if (matched) {
		slot = touch_frame_http_find(message);
		if (slot != NULL) {
			if (result == 0) {
				touch_frame_stats.http_write_complete++;
				touch_frame_state.http_complete_since_source++;
				touch_frame_stage_add(&touch_frame_stats.http_write_total,
					hal_read_curtime_us() - slot->write_start_us);
				memset(slot, 0, sizeof(*slot));
			} else if (result == TOUCH_FRAME_WOULD_BLOCK) {
				touch_frame_stats.http_write_deferred++;
			} else {
				touch_frame_stats.http_write_errors++;
				memset(slot, 0, sizeof(*slot));
			}
		}
	}
	if (touch_frame_state.http_write_task == current) {
		touch_frame_state.http_write_task = NULL;
		touch_frame_state.http_write_slot = NULL;
	}
	taskEXIT_CRITICAL();
	return result;
}

extern ssize_t __real_lwip_writev(int socket, const struct iovec *iov,
				  int iov_count);

ssize_t __wrap_lwip_writev(int socket, const struct iovec *iov, int iov_count)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	touch_frame_http_slot_t *slot;
	uint32_t start_us = hal_read_curtime_us();
	uint32_t requested = 0U;
	ssize_t result;
	int matched;
	int i;

	for (i = 0; i < iov_count; i++) {
		if (iov[i].iov_len > (size_t)(UINT32_MAX - requested)) {
			requested = UINT32_MAX;
			break;
		}
		requested += (uint32_t)iov[i].iov_len;
	}
	taskENTER_CRITICAL();
	matched = (touch_frame_state.http_write_task == current) &&
		(touch_frame_state.http_write_slot != NULL);
	slot = matched ? touch_frame_state.http_write_slot : NULL;
	if (matched) {
		touch_frame_stats.tcp_calls++;
		touch_frame_stats.tcp_requested_bytes += requested;
	}
	taskEXIT_CRITICAL();

	result = __real_lwip_writev(socket, iov, iov_count);

	if (matched) {
		uint32_t end_us = hal_read_curtime_us();

		taskENTER_CRITICAL();
		touch_frame_stage_add(&touch_frame_stats.tcp_call,
			end_us - start_us);
		if (result < 0) {
			touch_frame_stats.tcp_errors++;
		} else {
			uint32_t accepted = (uint32_t)result;

			touch_frame_stats.tcp_accepted_bytes += accepted;
			touch_frame_stats.tcp_partial += (accepted < requested);
			touch_frame_state.tcp_accept_since_source++;
			if ((slot != NULL) && slot->active &&
			    !slot->tcp_accepted) {
				uint32_t hid_to_tcp_us = end_us - slot->hid_start_us;

				slot->tcp_accepted = 1U;
				slot->first_tcp_accept_us = end_us;
				touch_frame_stage_add(
					&touch_frame_stats.hid_to_tcp_accept,
					hid_to_tcp_us);
				if (hid_to_tcp_us >
				    touch_frame_state.max_hid_to_tcp_since_source) {
					touch_frame_state.max_hid_to_tcp_since_source =
						hid_to_tcp_us;
				}
			}
		}
		taskEXIT_CRITICAL();
	}
	return result;
}

void carbox_touch_frame_source_frame(uint32_t source_delta_us,
				      int delta_valid)
{
	uint32_t now_us = hal_read_curtime_us();
	uint32_t hid_count;
	uint32_t max_gcd_wait;
	uint32_t http_complete_count;
	uint32_t tcp_accept_count;
	uint32_t max_hid_to_tcp;

	taskENTER_CRITICAL();
	hid_count = touch_frame_state.hid_since_source;
	max_gcd_wait = touch_frame_state.max_gcd_wait_since_source;
	http_complete_count = touch_frame_state.http_complete_since_source;
	tcp_accept_count = touch_frame_state.tcp_accept_since_source;
	max_hid_to_tcp = touch_frame_state.max_hid_to_tcp_since_source;
	if (delta_valid) {
		int gap = source_delta_us > 38000U;

		if (gap) {
			touch_frame_stats.source_gap++;
			touch_frame_stats.gap_with_hid += (hid_count != 0U);
			touch_frame_stats.gap_without_hid += (hid_count == 0U);
			touch_frame_stats.gap_gcd_ge_1ms +=
				(max_gcd_wait >= 1000U);
			touch_frame_stats.gap_gcd_ge_5ms +=
				(max_gcd_wait >= 5000U);
			touch_frame_stats.gap_gcd_ge_10ms +=
				(max_gcd_wait >= 10000U);
			touch_frame_stats.gap_with_http +=
				(http_complete_count != 0U);
			touch_frame_stats.gap_without_http +=
				(http_complete_count == 0U);
			touch_frame_stats.gap_with_tcp +=
				(tcp_accept_count != 0U);
			touch_frame_stats.gap_without_tcp +=
				(tcp_accept_count == 0U);
			touch_frame_stats.gap_tcp_ge_5ms +=
				(max_hid_to_tcp >= 5000U);
			touch_frame_stats.gap_tcp_ge_10ms +=
				(max_hid_to_tcp >= 10000U);
		} else {
			touch_frame_stats.source_normal++;
			touch_frame_stats.normal_with_hid += (hid_count != 0U);
			touch_frame_stats.normal_without_hid += (hid_count == 0U);
			touch_frame_stats.normal_gcd_ge_5ms +=
				(max_gcd_wait >= 5000U);
			touch_frame_stats.normal_with_http +=
				(http_complete_count != 0U);
			touch_frame_stats.normal_without_http +=
				(http_complete_count == 0U);
			touch_frame_stats.normal_with_tcp +=
				(tcp_accept_count != 0U);
			touch_frame_stats.normal_without_tcp +=
				(tcp_accept_count == 0U);
		}
		touch_frame_stats.interval_hid_sum += hid_count;
		if (hid_count > touch_frame_stats.interval_hid_max) {
			touch_frame_stats.interval_hid_max = hid_count;
		}
		if ((hid_count != 0U) &&
		    touch_frame_state.last_hid_complete_valid) {
			touch_frame_stage_add(&touch_frame_stats.hid_to_frame,
				now_us - touch_frame_state.last_hid_complete_us);
		}
	}
	touch_frame_state.hid_since_source = 0U;
	touch_frame_state.max_gcd_wait_since_source = 0U;
	touch_frame_state.http_complete_since_source = 0U;
	touch_frame_state.tcp_accept_since_source = 0U;
	touch_frame_state.max_hid_to_tcp_since_source = 0U;
	taskEXIT_CRITICAL();
}

static uint32_t touch_frame_stage_avg(const touch_frame_stage_t *stage)
{
	return stage->samples != 0U ?
		(uint32_t)(stage->sum_us / stage->samples) : 0U;
}

void carbox_touch_frame_profiler_report(uint32_t sequence)
{
	touch_frame_stats_t stats;
	uint32_t source_samples;

	taskENTER_CRITICAL();
	stats = touch_frame_stats;
	memset(&touch_frame_stats, 0, sizeof(touch_frame_stats));
	taskEXIT_CRITICAL();
	if ((stats.hid_calls == 0U) && (stats.source_normal == 0U) &&
	    (stats.source_gap == 0U)) {
		return;
	}
	source_samples = stats.source_normal + stats.source_gap;
	rt_printf("[TOUCHFRAME][%lu] hid calls/error=%lu/%lu "
		  "interval_us samples/avg/max=%lu/%lu/%lu >20/33ms=%lu/%lu "
		  "call_us avg/max=%lu/%lu dispatch/missing=%lu/%lu "
		  "gcd wait/exec/total_us avg:max=%lu:%lu/%lu:%lu/%lu:%lu "
		  "wait_ge1/5/10ms=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.hid_calls,
		  (unsigned long)stats.hid_errors,
		  (unsigned long)stats.hid_interval.samples,
		  (unsigned long)touch_frame_stage_avg(&stats.hid_interval),
		  (unsigned long)stats.hid_interval.max_us,
		  (unsigned long)stats.hid_interval_gt_20ms,
		  (unsigned long)stats.hid_interval_gt_33ms,
		  (unsigned long)touch_frame_stage_avg(&stats.hid_call),
		  (unsigned long)stats.hid_call.max_us,
		  (unsigned long)stats.dispatch_calls,
		  (unsigned long)stats.dispatch_missing,
		  (unsigned long)touch_frame_stage_avg(&stats.dispatch_wait),
		  (unsigned long)stats.dispatch_wait.max_us,
		  (unsigned long)touch_frame_stage_avg(&stats.dispatch_exec),
		  (unsigned long)stats.dispatch_exec.max_us,
		  (unsigned long)touch_frame_stage_avg(&stats.dispatch_total),
		  (unsigned long)stats.dispatch_total.max_us,
		  (unsigned long)stats.dispatch_wait_ge_1ms,
		  (unsigned long)stats.dispatch_wait_ge_5ms,
		  (unsigned long)stats.dispatch_wait_ge_10ms);
	rt_printf("[TOUCHFRAME][%lu] source normal/gap=%lu/%lu "
		  "normal hid/no_hid/gcd_ge5=%lu/%lu/%lu "
		  "gap hid/no_hid/gcd_ge1/5/10=%lu/%lu/%lu/%lu/%lu "
		  "hid_per_interval avg/max=%lu/%lu "
		  "last_hid_to_frame_us samples/avg/max=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.source_normal,
		  (unsigned long)stats.source_gap,
		  (unsigned long)stats.normal_with_hid,
		  (unsigned long)stats.normal_without_hid,
		  (unsigned long)stats.normal_gcd_ge_5ms,
		  (unsigned long)stats.gap_with_hid,
		  (unsigned long)stats.gap_without_hid,
		  (unsigned long)stats.gap_gcd_ge_1ms,
		  (unsigned long)stats.gap_gcd_ge_5ms,
		  (unsigned long)stats.gap_gcd_ge_10ms,
		  (unsigned long)(source_samples != 0U ?
			stats.interval_hid_sum / source_samples : 0U),
		  (unsigned long)stats.interval_hid_max,
		  (unsigned long)stats.hid_to_frame.samples,
		  (unsigned long)touch_frame_stage_avg(&stats.hid_to_frame),
		  (unsigned long)stats.hid_to_frame.max_us);
	rt_printf("[TOUCHFRAME][%lu] http enq/error/full/stale=%lu/%lu/%lu/%lu "
		  "write call/done/defer/error=%lu/%lu/%lu/%lu "
		  "tcp_accept calls/error/partial bytes=%lu/%lu/%lu %lu/%luB "
		  "us hid_to_enq=%lu/%lu enq_to_write=%lu/%lu "
		  "write_total=%lu/%lu hid_to_tcp=%lu/%lu tcp_call=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.http_enqueued,
		  (unsigned long)stats.http_enqueue_errors,
		  (unsigned long)stats.http_slot_full,
		  (unsigned long)stats.http_stale,
		  (unsigned long)stats.http_write_calls,
		  (unsigned long)stats.http_write_complete,
		  (unsigned long)stats.http_write_deferred,
		  (unsigned long)stats.http_write_errors,
		  (unsigned long)stats.tcp_calls,
		  (unsigned long)stats.tcp_errors,
		  (unsigned long)stats.tcp_partial,
		  (unsigned long)stats.tcp_requested_bytes,
		  (unsigned long)stats.tcp_accepted_bytes,
		  (unsigned long)touch_frame_stage_avg(
			&stats.hid_to_http_enqueue),
		  (unsigned long)stats.hid_to_http_enqueue.max_us,
		  (unsigned long)touch_frame_stage_avg(&stats.enqueue_to_write),
		  (unsigned long)stats.enqueue_to_write.max_us,
		  (unsigned long)touch_frame_stage_avg(&stats.http_write_total),
		  (unsigned long)stats.http_write_total.max_us,
		  (unsigned long)touch_frame_stage_avg(&stats.hid_to_tcp_accept),
		  (unsigned long)stats.hid_to_tcp_accept.max_us,
		  (unsigned long)touch_frame_stage_avg(&stats.tcp_call),
		  (unsigned long)stats.tcp_call.max_us);
	rt_printf("[TOUCHFRAME][%lu] source_transport normal http/no_http="
		  "%lu/%lu tcp/no_tcp=%lu/%lu gap http/no_http=%lu/%lu "
		  "tcp/no_tcp=%lu/%lu hid_to_tcp_ge5/10ms=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.normal_with_http,
		  (unsigned long)stats.normal_without_http,
		  (unsigned long)stats.normal_with_tcp,
		  (unsigned long)stats.normal_without_tcp,
		  (unsigned long)stats.gap_with_http,
		  (unsigned long)stats.gap_without_http,
		  (unsigned long)stats.gap_with_tcp,
		  (unsigned long)stats.gap_without_tcp,
		  (unsigned long)stats.gap_tcp_ge_5ms,
		  (unsigned long)stats.gap_tcp_ge_10ms);
}

#else

uint32_t carbox_touch_frame_dispatch_begin(void)
{
	return 0U;
}

void carbox_touch_frame_dispatch_callback_begin(uint32_t token)
{
	(void)token;
}

void carbox_touch_frame_dispatch_callback_end(uint32_t token)
{
	(void)token;
}

void carbox_touch_frame_dispatch_complete(uint32_t token,
					  uint32_t submit_us,
					  uint32_t callback_start_us,
					  uint32_t callback_end_us,
					  uint32_t return_us,
					  int callback_called)
{
	(void)token;
	(void)submit_us;
	(void)callback_start_us;
	(void)callback_end_us;
	(void)return_us;
	(void)callback_called;
}

void carbox_touch_frame_source_frame(uint32_t source_delta_us,
				      int delta_valid)
{
	(void)source_delta_us;
	(void)delta_valid;
}

void carbox_touch_frame_profiler_report(uint32_t sequence)
{
	(void)sequence;
}

#endif
