#include "screen_queue_profiler.h"

#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "hal_timer.h"
#include "lwip/sockets.h"

#ifndef CONFIG_SCREEN_QUEUE_PROFILE
#define CONFIG_SCREEN_QUEUE_PROFILE 0
#endif

#ifndef CONFIG_SCREEN_FRAME_FORMAT_PROFILE
#define CONFIG_SCREEN_FRAME_FORMAT_PROFILE 0
#endif

#ifndef CONFIG_SCREEN_TCP_BUFFER_PROFILE
#define CONFIG_SCREEN_TCP_BUFFER_PROFILE 0
#endif

#ifndef CONFIG_SCREEN_TIMESTAMP_PROFILE
#define CONFIG_SCREEN_TIMESTAMP_PROFILE 0
#endif

#if CONFIG_SCREEN_QUEUE_PROFILE

#define SCREENPROF_ACTIVE_TASKS       4U
#define SCREENPROF_FRAME_TIMES      256U
#define SCREENPROF_METRIC_SAMPLES  1024U
#define SCREENPROF_ANOMALIES          8U
#define SCREENPROF_VARIANCE_CLAMP_US 10000000U
#define SCREENPROF_START_CODE_SCAN_BYTES 256U
#define SCREENPROF_VCL_SCAN_BYTES       1024U
#define SCREENPROF_AVCC_MAX_NALS       64U
/*
 * A ready select()/recv() that completes within this interval means data was
 * already queued locally; it did not wait for another Wi-Fi/TCP arrival.  A
 * long consecutive streak is therefore evidence that ScreenReceiver is
 * consuming an existing TCP backlog.  This does not claim to measure bytes
 * buffered inside the iPhone, but sustained local backlog implies TCP flow
 * control can push the queue back toward the sender.
 */
#define SCREENPROF_IMMEDIATE_US          100U
#define SCREENPROF_SHORT_WAIT_US        1000U

typedef enum screenprof_frame_class_e {
	SCREENPROF_FRAME_UNKNOWN = 0,
	SCREENPROF_FRAME_NON_IDR,
	SCREENPROF_FRAME_IDR,
	SCREENPROF_FRAME_CLASS_COUNT
} screenprof_frame_class_t;

typedef struct screenprof_class_stats_s {
	uint32_t frames;
	uint32_t delta_samples;
	uint32_t completed;
	uint64_t bytes;
	uint64_t delta_sum_us;
	uint64_t queue_sum_us;
	uint64_t prepare_sum_us;
	uint64_t socket_sum_us;
	uint64_t handoff_sum_us;
	uint32_t delta_max_us;
	uint32_t queue_max_us;
	uint32_t prepare_max_us;
	uint32_t socket_max_us;
	uint32_t handoff_max_us;
} screenprof_class_stats_t;

typedef struct screenprof_probe_result_s {
	uint32_t frame_class;
	uint32_t first_nal_type;
} screenprof_probe_result_t;

typedef enum screenprof_metric_id_e {
	SCREENPROF_METRIC_ARRIVAL_DELTA = 0,
	SCREENPROF_METRIC_QUEUE,
	SCREENPROF_METRIC_PREPARE,
	SCREENPROF_METRIC_SOCKET,
	SCREENPROF_METRIC_HANDOFF,
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
	SCREENPROF_METRIC_STAMP_DELTA,
	SCREENPROF_METRIC_STAMP_ERROR,
	SCREENPROF_METRIC_STAMP_AFTER_ARRIVAL,
	SCREENPROF_METRIC_SOURCE_NTP_DELTA,
	SCREENPROF_METRIC_WIRE_NTP_DELTA,
	SCREENPROF_METRIC_WIRE_SOURCE_ERROR,
#endif
	SCREENPROF_METRIC_COUNT
} screenprof_metric_id_t;

typedef struct screenprof_metric_s {
	uint32_t count;
	uint32_t sample_count;
	uint32_t sample_overflow;
	uint32_t variance_clipped;
	uint64_t sum_us;
	uint64_t variance_sum_us;
	uint64_t sum_square_us;
	uint32_t min_us;
	uint32_t max_us;
} screenprof_metric_t;

typedef struct screenprof_anomaly_s {
	uint32_t sequence;
	uint32_t bytes;
	uint32_t arrival_delta_us;
	uint32_t queue_us;
	uint32_t prepare_us;
	uint32_t socket_us;
	uint32_t handoff_us;
	uint32_t score_us;
	uint32_t frame_class;
	uint32_t first_nal_type;
} screenprof_anomaly_t;

typedef struct screenprof_frame_s {
	uint32_t arrival_us;
	uint32_t sequence;
	uint32_t arrival_delta_us;
	uint32_t bytes;
	uint32_t frame_class;
	uint32_t first_nal_type;
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
	uint32_t source_ntp_valid;
	uint64_t source_ntp;
#endif
} screenprof_frame_t;

typedef struct screenprof_active_input_s {
	uint32_t arrival_us;
	uint32_t sequence;
	uint32_t arrival_delta_us;
	uint32_t frame_class;
	uint32_t first_nal_type;
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
	uint32_t source_ntp_valid;
	uint64_t source_ntp;
#endif
} screenprof_active_input_t;

typedef struct screenprof_active_frame_s {
	uint32_t valid;
	uint32_t timestamp_captured;
	uint32_t sequence;
	uint32_t bytes;
	uint32_t arrival_us;
	uint32_t arrival_delta_us;
	uint32_t dequeue_us;
	uint32_t first_write_us;
	uint32_t expected_write_bytes;
	uint32_t accepted_write_bytes;
	uint32_t queue_us;
	uint32_t prepare_us;
	uint32_t frame_class;
	uint32_t first_nal_type;
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
	uint32_t source_ntp_valid;
	uint32_t wire_ntp_captured;
	uint64_t source_ntp;
	uint64_t wire_ntp;
#endif
} screenprof_active_frame_t;

typedef struct screenprof_stats_s {
	uint32_t video_calls;
	uint64_t video_bytes;
	uint32_t enqueue_frames;
	uint64_t enqueue_bytes;
	uint32_t dequeue_frames;
	uint64_t dequeue_bytes;
	uint64_t age_sum_us;
	uint32_t age_samples;
	uint32_t age_max_us;
	uint32_t queue_max_depth;
	uint32_t queue_ring_overflow;
	uint32_t queue_desync;

	uint32_t recv_calls;
	uint64_t recv_bytes;
	uint64_t recv_time_sum_us;
	uint32_t recv_time_max_us;
	uint32_t recv_zero;
	uint32_t recv_error;
	uint32_t recv_immediate;
	uint64_t recv_immediate_bytes;
	uint32_t recv_short_wait;
	uint32_t recv_long_wait;
	uint32_t recv_control_calls;
	uint32_t recv_header_calls;
	uint32_t recv_payload_calls;
	uint32_t recv_tail_calls;
	uint32_t recv_payload_immediate;
	uint64_t recv_payload_immediate_bytes;
	uint32_t rx_buffer_samples;
	uint32_t rx_buffer_errors;
	uint64_t rx_pending_sum;
	uint32_t rx_pending_last;
	uint32_t rx_pending_max;
	uint64_t rx_window_used_sum;
	uint32_t rx_window_used_last;
	uint32_t rx_window_used_max;
	uint32_t rx_window_capacity;
	uint32_t rx_window_ge75;
	uint32_t rx_window_ge90;
	uint32_t rx_window_full;

	uint32_t select_calls;
	uint32_t select_ready;
	uint32_t select_timeout;
	uint32_t select_error;
	uint64_t select_time_sum_us;
	uint32_t select_time_max_us;
	uint32_t select_immediate;
	uint32_t select_short_wait;
	uint32_t select_long_wait;
	uint32_t select_streak_max;
	uint32_t select_streak_max_us;

	uint32_t write_calls;
	uint64_t write_request_bytes;
	uint64_t write_return_bytes;
	uint64_t write_time_sum_us;
	uint32_t write_time_max_us;
	uint32_t write_partial;
	uint32_t write_error;
	uint32_t tx_buffer_samples;
	uint32_t tx_buffer_errors;
	uint64_t tx_buffer_used_sum;
	uint32_t tx_buffer_used_last;
	uint32_t tx_buffer_used_max;
	uint32_t tx_buffer_capacity;
	uint32_t tx_buffer_ge75;
	uint32_t tx_buffer_ge90;
	uint32_t tx_buffer_full;
	uint32_t tx_queue_last;
	uint32_t tx_queue_max;
	uint32_t tx_queue_capacity;
	uint64_t buffer_probe_time_sum_us;
	uint32_t buffer_probe_time_max_us;
	uint32_t completed_frames;
	uint32_t frames_without_write;
	uint32_t gap_over_40ms;
	uint32_t gap_over_50ms;
	uint32_t gap_over_100ms;

#if CONFIG_SCREEN_TIMESTAMP_PROFILE
	uint32_t timestamp_calls;
	uint32_t timestamp_paired;
	uint32_t timestamp_unpaired;
	uint32_t timestamp_duplicate;
	uint32_t timestamp_delta_samples;
	uint32_t timestamp_tick_regressions;
	uint32_t timestamp_error_le_1ms;
	uint32_t timestamp_error_le_5ms;
	uint32_t timestamp_error_le_20ms;
	uint32_t timestamp_tick_last_ms;
	uint64_t timestamp_ntp_last;

	uint32_t source_header_ntp;
	uint32_t source_callback_paired;
	uint32_t source_callback_missing;
	uint32_t wire_source_paired;
	uint32_t wire_source_missing;
	uint32_t timeline_baseline_resets;
	uint32_t timeline_regressions;
	uint32_t timeline_samples;
	uint32_t timeline_interval_samples;
	uint64_t timeline_abs_error_sum_us;
	uint64_t timeline_abs_error_max_us;
	int64_t timeline_error_first_us;
	int64_t timeline_error_last_us;
	int64_t timeline_error_min_us;
	int64_t timeline_error_max_us;
	uint64_t timeline_source_first_elapsed_us;
	uint64_t timeline_window_source_elapsed_us;
#endif

	uint32_t format_samples;
	uint32_t annexb_frames;
	uint32_t annexb_start_at_0;
	uint32_t annexb_start_at_4;
	uint32_t annexb_start_1_15;
	uint32_t annexb_start_16_63;
	uint32_t annexb_start_64_255;
	uint32_t annexb_3byte_prefix;
	uint32_t annexb_4byte_prefix;
	uint32_t annexb_offset_sum;
	uint32_t annexb_offset_min;
	uint32_t annexb_offset_max;
	uint32_t avcc_frames;
	uint32_t unknown_frames;
	uint32_t first_nal_type[32];
	uint32_t secondary_vcl_scans;
	uint32_t secondary_vcl_misses;
	screenprof_class_stats_t class_stats[SCREENPROF_FRAME_CLASS_COUNT];
} screenprof_stats_t;

/* CVector's four-word layout is confirmed from the linked implementation. */
typedef struct screenprof_vector_view_s {
	int capacity;
	int size;
	void *data;
	int element_size;
} screenprof_vector_view_t;

typedef struct screenprof_frame_item_s {
	void *data;
	int bytes;
} screenprof_frame_item_t;

static screenprof_stats_t screenprof_live;
static screenprof_stats_t screenprof_copy;
static screenprof_frame_t screenprof_frames[SCREENPROF_FRAME_TIMES]
	__attribute__((section(".lpddr.bss.screenprof")));
static uint32_t screenprof_metric_samples[SCREENPROF_METRIC_COUNT]
	[SCREENPROF_METRIC_SAMPLES]
	__attribute__((section(".lpddr.bss.screenprof")));
static uint32_t screenprof_metric_samples_copy[SCREENPROF_METRIC_COUNT]
	[SCREENPROF_METRIC_SAMPLES]
	__attribute__((section(".lpddr.bss.screenprof")));
static uint32_t screenprof_sort_scratch[SCREENPROF_METRIC_SAMPLES]
	__attribute__((section(".lpddr.bss.screenprof")));
static screenprof_metric_t screenprof_metrics[SCREENPROF_METRIC_COUNT];
static screenprof_metric_t screenprof_metrics_copy[SCREENPROF_METRIC_COUNT];
static screenprof_anomaly_t screenprof_anomalies[SCREENPROF_ANOMALIES];
static screenprof_anomaly_t screenprof_anomalies_copy[SCREENPROF_ANOMALIES];
static uint32_t screenprof_anomaly_count;
static uint32_t screenprof_anomaly_count_copy;
static uint32_t screenprof_frame_head;
static uint32_t screenprof_frame_count;
static int screenprof_frame_tracking_invalid;
static void *screenprof_vector;
static TaskHandle_t screenprof_active_tasks[SCREENPROF_ACTIVE_TASKS];
static screenprof_active_input_t
	screenprof_active_inputs[SCREENPROF_ACTIVE_TASKS];
static screenprof_active_frame_t screenprof_active_frame;
static uint32_t screenprof_next_sequence;
static uint32_t screenprof_last_arrival_us;
static uint32_t screenprof_have_last_arrival;
static TaskHandle_t screenprof_receiver_task;
static TaskHandle_t screenprof_sender_task;
static uint32_t screenprof_select_streak;
static uint32_t screenprof_select_streak_start_us;
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
static uint32_t screenprof_timestamp_prev_ms;
static uint32_t screenprof_timestamp_have_prev;
static uint32_t screenprof_pending_source_ntp_valid;
static uint64_t screenprof_pending_source_ntp;
static uint32_t screenprof_timeline_have_baseline;
static uint64_t screenprof_timeline_source_base;
static uint64_t screenprof_timeline_wire_base;
static uint64_t screenprof_timeline_source_prev;
static uint64_t screenprof_timeline_wire_prev;
#endif

extern void __real_AirPlayScreen_SendVideo(const void *data, int bytes);
extern void __real_CVector_push_back(void *vector, const void *element);
extern void __real_CVector_erase(void *vector, int index);
extern void __real_CVector_delete(void *vector);
extern ssize_t __real_lwip_recv(int socket, void *buffer, size_t bytes,
				int flags);
extern ssize_t __real_lwip_write(int socket, const void *buffer, size_t bytes);
extern int __real_lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset,
			      fd_set *exceptset, struct timeval *timeout);
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
extern uint64_t __real_UpTicksToNTP(uint64_t ticks);
static void screenprof_record_metric(screenprof_metric_id_t metric_id,
				     uint32_t value_us);

static uint64_t screenprof_load_le64(const void *ptr)
{
	const uint8_t *p = (const uint8_t *)ptr;
	uint64_t value = 0U;
	uint32_t i;

	for (i = 0U; i < 8U; i++) {
		value |= (uint64_t)p[i] << (i * 8U);
	}
	return value;
}

static uint32_t screenprof_load_le32(const void *ptr)
{
	const uint8_t *p = (const uint8_t *)ptr;

	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t screenprof_ntp_delta_us(uint64_t newer, uint64_t older)
{
	uint64_t delta = newer - older;
	uint64_t seconds = delta >> 32;
	uint64_t fraction = (uint32_t)delta;

	return seconds * 1000000ULL +
		((fraction * 1000000ULL) >> 32);
}

static uint32_t screenprof_u64_to_u32_sat(uint64_t value)
{
	return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/* Caller holds the task critical section. */
static void screenprof_record_wire_source_timeline(uint64_t source_ntp,
						   uint64_t wire_ntp)
{
	uint64_t source_elapsed_us;
	uint64_t wire_elapsed_us;
	int64_t error_us;
	uint64_t abs_error_us;

	if (!screenprof_timeline_have_baseline ||
	    source_ntp < screenprof_timeline_source_prev ||
	    wire_ntp < screenprof_timeline_wire_prev) {
		if (screenprof_timeline_have_baseline) {
			screenprof_live.timeline_baseline_resets++;
			screenprof_live.timeline_regressions++;
		}
		screenprof_timeline_have_baseline = 1U;
		screenprof_timeline_source_base = source_ntp;
		screenprof_timeline_wire_base = wire_ntp;
		screenprof_timeline_source_prev = source_ntp;
		screenprof_timeline_wire_prev = wire_ntp;
		source_elapsed_us = 0U;
		wire_elapsed_us = 0U;
	} else {
		uint64_t source_delta_us = screenprof_ntp_delta_us(
			source_ntp, screenprof_timeline_source_prev);
		uint64_t wire_delta_us = screenprof_ntp_delta_us(
			wire_ntp, screenprof_timeline_wire_prev);
		uint64_t interval_error_us =
			(source_delta_us >= wire_delta_us) ?
			source_delta_us - wire_delta_us :
			wire_delta_us - source_delta_us;

		screenprof_record_metric(SCREENPROF_METRIC_SOURCE_NTP_DELTA,
			screenprof_u64_to_u32_sat(source_delta_us));
		screenprof_record_metric(SCREENPROF_METRIC_WIRE_NTP_DELTA,
			screenprof_u64_to_u32_sat(wire_delta_us));
		screenprof_record_metric(SCREENPROF_METRIC_WIRE_SOURCE_ERROR,
			screenprof_u64_to_u32_sat(interval_error_us));
		screenprof_live.timeline_interval_samples++;
		screenprof_timeline_source_prev = source_ntp;
		screenprof_timeline_wire_prev = wire_ntp;
		source_elapsed_us = screenprof_ntp_delta_us(
			source_ntp, screenprof_timeline_source_base);
		wire_elapsed_us = screenprof_ntp_delta_us(
			wire_ntp, screenprof_timeline_wire_base);
	}

	error_us = (wire_elapsed_us >= source_elapsed_us) ?
		(int64_t)(wire_elapsed_us - source_elapsed_us) :
		-(int64_t)(source_elapsed_us - wire_elapsed_us);
	abs_error_us = error_us >= 0 ? (uint64_t)error_us :
		(uint64_t)(-error_us);
	if (screenprof_live.timeline_samples == 0U) {
		screenprof_live.timeline_error_first_us = error_us;
		screenprof_live.timeline_error_min_us = error_us;
		screenprof_live.timeline_error_max_us = error_us;
		screenprof_live.timeline_source_first_elapsed_us =
			source_elapsed_us;
	}
	screenprof_live.timeline_samples++;
	screenprof_live.timeline_error_last_us = error_us;
	if (error_us < screenprof_live.timeline_error_min_us) {
		screenprof_live.timeline_error_min_us = error_us;
	}
	if (error_us > screenprof_live.timeline_error_max_us) {
		screenprof_live.timeline_error_max_us = error_us;
	}
	screenprof_live.timeline_abs_error_sum_us += abs_error_us;
	if (abs_error_us > screenprof_live.timeline_abs_error_max_us) {
		screenprof_live.timeline_abs_error_max_us = abs_error_us;
	}
	screenprof_live.timeline_window_source_elapsed_us =
		source_elapsed_us -
		screenprof_live.timeline_source_first_elapsed_us;
}
#endif

#if CONFIG_SCREEN_TCP_BUFFER_PROFILE
static void screenprof_record_probe_time(uint32_t elapsed_us)
{
	screenprof_live.buffer_probe_time_sum_us += elapsed_us;
	if (elapsed_us > screenprof_live.buffer_probe_time_max_us) {
		screenprof_live.buffer_probe_time_max_us = elapsed_us;
	}
}

static void screenprof_record_rx_buffer(
	const struct lwip_tcp_buffer_diag *diag, uint32_t probe_us)
{
	uint32_t capacity = diag->rx_window_capacity;
	uint32_t available = diag->rx_window_available;
	uint32_t used = (available < capacity) ? capacity - available : 0U;

	taskENTER_CRITICAL();
	screenprof_live.rx_buffer_samples++;
	screenprof_live.rx_pending_sum += diag->rx_pending_bytes;
	screenprof_live.rx_pending_last = diag->rx_pending_bytes;
	if (diag->rx_pending_bytes > screenprof_live.rx_pending_max) {
		screenprof_live.rx_pending_max = diag->rx_pending_bytes;
	}
	screenprof_live.rx_window_used_sum += used;
	screenprof_live.rx_window_used_last = used;
	if (used > screenprof_live.rx_window_used_max) {
		screenprof_live.rx_window_used_max = used;
	}
	screenprof_live.rx_window_capacity = capacity;
	if ((capacity != 0U) && (used * 100U >= capacity * 75U)) {
		screenprof_live.rx_window_ge75++;
	}
	if ((capacity != 0U) && (used * 100U >= capacity * 90U)) {
		screenprof_live.rx_window_ge90++;
	}
	if ((capacity != 0U) && (used >= capacity)) {
		screenprof_live.rx_window_full++;
	}
	screenprof_record_probe_time(probe_us);
	taskEXIT_CRITICAL();
}

static void screenprof_record_tx_buffer(
	const struct lwip_tcp_buffer_diag *diag, uint32_t probe_us)
{
	uint32_t capacity = diag->tx_buffer_capacity;
	uint32_t available = diag->tx_buffer_available;
	uint32_t used = (available < capacity) ? capacity - available : 0U;

	taskENTER_CRITICAL();
	screenprof_live.tx_buffer_samples++;
	screenprof_live.tx_buffer_used_sum += used;
	screenprof_live.tx_buffer_used_last = used;
	if (used > screenprof_live.tx_buffer_used_max) {
		screenprof_live.tx_buffer_used_max = used;
	}
	screenprof_live.tx_buffer_capacity = capacity;
	if ((capacity != 0U) && (used * 100U >= capacity * 75U)) {
		screenprof_live.tx_buffer_ge75++;
	}
	if ((capacity != 0U) && (used * 100U >= capacity * 90U)) {
		screenprof_live.tx_buffer_ge90++;
	}
	if ((capacity != 0U) && (used >= capacity)) {
		screenprof_live.tx_buffer_full++;
	}
	screenprof_live.tx_queue_last = diag->tx_queue_len;
	if (diag->tx_queue_len > screenprof_live.tx_queue_max) {
		screenprof_live.tx_queue_max = diag->tx_queue_len;
	}
	screenprof_live.tx_queue_capacity = diag->tx_queue_capacity;
	screenprof_record_probe_time(probe_us);
	taskEXIT_CRITICAL();
}
#endif

static int screenprof_name_equal(const char *left, const char *right)
{
	if ((left == NULL) || (right == NULL)) {
		return 0;
	}
	while (*left == *right) {
		if (*left == '\0') {
			return 1;
		}
		left++;
		right++;
	}
	return 0;
}

static int screenprof_is_task(TaskHandle_t *cached, const char *name)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();

	if ((*cached != NULL) && (current == *cached)) {
		return 1;
	}
	if (screenprof_name_equal(pcTaskGetName(current), name)) {
		*cached = current;
		return 1;
	}
	return 0;
}

static int screenprof_mark_active(TaskHandle_t task)
{
	uint32_t i;
	int marked = 0;

	taskENTER_CRITICAL();
	for (i = 0U; i < SCREENPROF_ACTIVE_TASKS; i++) {
		if (screenprof_active_tasks[i] == task) {
			/* A nested call remains owned by the outer wrapper. */
			break;
		}
		if (screenprof_active_tasks[i] == NULL) {
			screenprof_active_tasks[i] = task;
			screenprof_active_inputs[i] =
				(screenprof_active_input_t){ 0 };
			marked = 1;
			break;
		}
	}
	taskEXIT_CRITICAL();
	return marked;
}

static void screenprof_unmark_active(TaskHandle_t task)
{
	uint32_t i;

	taskENTER_CRITICAL();
	for (i = 0U; i < SCREENPROF_ACTIVE_TASKS; i++) {
		if (screenprof_active_tasks[i] == task) {
			screenprof_active_tasks[i] = NULL;
			screenprof_active_inputs[i] =
				(screenprof_active_input_t){ 0 };
			break;
		}
	}
	taskEXIT_CRITICAL();
}

static void screenprof_set_active_input(TaskHandle_t task,
					const screenprof_active_input_t *input)
{
	uint32_t i;

	taskENTER_CRITICAL();
	for (i = 0U; i < SCREENPROF_ACTIVE_TASKS; i++) {
		if (screenprof_active_tasks[i] == task) {
			screenprof_active_inputs[i] = *input;
			break;
		}
	}
	taskEXIT_CRITICAL();
}

static int screenprof_get_active_input(TaskHandle_t task,
				       screenprof_active_input_t *input)
{
	uint32_t i;
	int found = 0;

	taskENTER_CRITICAL();
	for (i = 0U; i < SCREENPROF_ACTIVE_TASKS; i++) {
		if (screenprof_active_tasks[i] == task) {
			*input = screenprof_active_inputs[i];
			found = 1;
			break;
		}
	}
	taskEXIT_CRITICAL();
	return found;
}

/* Caller holds the task critical section. */
static void screenprof_record_metric(screenprof_metric_id_t metric_id,
				     uint32_t elapsed_us)
{
	screenprof_metric_t *metric = &screenprof_metrics[metric_id];
	uint32_t variance_us = elapsed_us;

	metric->count++;
	metric->sum_us += elapsed_us;
	if ((metric->count == 1U) || (elapsed_us < metric->min_us)) {
		metric->min_us = elapsed_us;
	}
	if (elapsed_us > metric->max_us) {
		metric->max_us = elapsed_us;
	}
	if (variance_us > SCREENPROF_VARIANCE_CLAMP_US) {
		variance_us = SCREENPROF_VARIANCE_CLAMP_US;
		metric->variance_clipped++;
	}
	metric->variance_sum_us += variance_us;
	metric->sum_square_us += (uint64_t)variance_us * variance_us;
	if (metric->sample_count < SCREENPROF_METRIC_SAMPLES) {
		screenprof_metric_samples[metric_id][metric->sample_count++] =
			elapsed_us;
	} else {
		metric->sample_overflow++;
	}
}

/* Keep the worst events without printing from the video hot path. */
static void screenprof_record_anomaly(const screenprof_anomaly_t *anomaly)
{
	uint32_t i;
	uint32_t smallest = 0U;

	if ((anomaly->arrival_delta_us <= 50000U) &&
	    (anomaly->queue_us <= 10000U) &&
	    (anomaly->prepare_us <= 10000U) &&
	    (anomaly->socket_us <= 20000U) &&
	    (anomaly->handoff_us <= 40000U)) {
		return;
	}
	if (screenprof_anomaly_count < SCREENPROF_ANOMALIES) {
		screenprof_anomalies[screenprof_anomaly_count++] = *anomaly;
		return;
	}
	for (i = 1U; i < SCREENPROF_ANOMALIES; i++) {
		if (screenprof_anomalies[i].score_us <
		    screenprof_anomalies[smallest].score_us) {
			smallest = i;
		}
	}
	if (anomaly->score_us > screenprof_anomalies[smallest].score_us) {
		screenprof_anomalies[smallest] = *anomaly;
	}
}

static int screenprof_current_is_active(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t i;
	int active = 0;

	taskENTER_CRITICAL();
	for (i = 0U; i < SCREENPROF_ACTIVE_TASKS; i++) {
		if (screenprof_active_tasks[i] == current) {
			active = 1;
			break;
		}
	}
	taskEXIT_CRITICAL();
	return active;
}

/*
 * This is deliberately a bounded, read-only format probe, not a decoder.
 * AirPlayScreen_SendVideo() is supplied by a closed library, so data[0]
 * cannot be assumed to be the first H.264 NAL byte.  The first test image
 * records where an Annex-B prefix is actually found.  If no prefix occurs in
 * the first 256 bytes, validate an AVCC length chain without scanning payload
 * bytes.  A malformed or proprietary envelope is reported as unknown rather
 * than guessed from frame size.
 */
static int screenprof_find_annexb_prefix(const uint8_t *data, uint32_t bytes,
					 uint32_t scan_limit,
					 uint32_t *offset, uint32_t *prefix_bytes)
{
	uint32_t scan_bytes;
	uint32_t i;

	if ((data == NULL) || (bytes < 3U)) {
		return 0;
	}
	scan_bytes = bytes;
	if (scan_bytes > scan_limit) {
		scan_bytes = scan_limit;
	}
	for (i = 0U; (i + 3U) <= scan_bytes; i++) {
		if ((i + 4U <= scan_bytes) &&
		    (data[i] == 0U) && (data[i + 1U] == 0U) &&
		    (data[i + 2U] == 0U) && (data[i + 3U] == 1U)) {
			*offset = i;
			*prefix_bytes = 4U;
			return 1;
		}
		if ((data[i] == 0U) && (data[i + 1U] == 0U) &&
		    (data[i + 2U] == 1U)) {
			*offset = i;
			*prefix_bytes = 3U;
			return 1;
		}
	}
	return 0;
}

static uint32_t screenprof_annexb_frame_class(const uint8_t *data,
					      uint32_t bytes,
					      uint32_t first_offset,
					      uint32_t first_prefix_bytes,
					      uint32_t *first_nal_type,
					      int *secondary_scan,
					      int *secondary_miss)
{
	uint32_t header_offset = first_offset + first_prefix_bytes;
	uint32_t nal_type;
	uint32_t cursor;
	uint32_t scan_end;

	*first_nal_type = 0U;
	*secondary_scan = 0;
	*secondary_miss = 0;
	if (header_offset >= bytes) {
		return SCREENPROF_FRAME_UNKNOWN;
	}
	nal_type = data[header_offset] & 0x1FU;
	*first_nal_type = nal_type;
	if (nal_type == 5U) {
		return SCREENPROF_FRAME_IDR;
	}
	if ((nal_type >= 1U) && (nal_type <= 4U)) {
		return SCREENPROF_FRAME_NON_IDR;
	}

	/*
	 * AUD/SPS/PPS may precede the VCL NAL in an access unit.  Only those
	 * uncommon frames pay for a bounded follow-up scan; ordinary type 1/5
	 * frames require one header-byte read.
	 */
	*secondary_scan = 1;
	cursor = header_offset + 1U;
	scan_end = bytes;
	if ((scan_end - cursor) > SCREENPROF_VCL_SCAN_BYTES) {
		scan_end = cursor + SCREENPROF_VCL_SCAN_BYTES;
	}
	while ((cursor + 3U) <= scan_end) {
		uint32_t relative_offset;
		uint32_t prefix_bytes;
		uint32_t remaining = scan_end - cursor;

		if (!screenprof_find_annexb_prefix(data + cursor, remaining,
						 remaining, &relative_offset,
						 &prefix_bytes)) {
			break;
		}
		header_offset = cursor + relative_offset + prefix_bytes;
		if (header_offset >= scan_end) {
			break;
		}
		nal_type = data[header_offset] & 0x1FU;
		if (nal_type == 5U) {
			return SCREENPROF_FRAME_IDR;
		}
		if ((nal_type >= 1U) && (nal_type <= 4U)) {
			return SCREENPROF_FRAME_NON_IDR;
		}
		cursor = header_offset + 1U;
	}
	*secondary_miss = 1;
	return SCREENPROF_FRAME_UNKNOWN;
}

static int screenprof_is_valid_avcc(const uint8_t *data, uint32_t bytes)
{
	uint32_t offset = 0U;
	uint32_t nals = 0U;
	uint32_t vcl_nals = 0U;

	if ((data == NULL) || (bytes < 5U)) {
		return 0;
	}
	while ((offset + 5U <= bytes) &&
	       (nals < SCREENPROF_AVCC_MAX_NALS)) {
		uint32_t nal_bytes = ((uint32_t)data[offset] << 24) |
			((uint32_t)data[offset + 1U] << 16) |
			((uint32_t)data[offset + 2U] << 8) |
			(uint32_t)data[offset + 3U];
		uint32_t nal_type;

		if ((nal_bytes == 0U) || (nal_bytes > bytes - offset - 4U)) {
			return 0;
		}
		nal_type = data[offset + 4U] & 0x1FU;
		if ((nal_type == 0U) || (nal_type > 23U)) {
			return 0;
		}
		if (nal_type <= 5U) {
			vcl_nals++;
		}
		offset += 4U + nal_bytes;
		nals++;
	}
	return (offset == bytes) && (nals != 0U) && (vcl_nals != 0U);
}

static screenprof_probe_result_t
screenprof_probe_frame_format(const void *buffer, int buffer_bytes)
{
	screenprof_probe_result_t result = {
		.frame_class = SCREENPROF_FRAME_UNKNOWN,
		.first_nal_type = 0U
	};
	const uint8_t *data = (const uint8_t *)buffer;
	uint32_t bytes = (buffer_bytes > 0) ? (uint32_t)buffer_bytes : 0U;
	uint32_t offset = 0U;
	uint32_t prefix_bytes = 0U;
	int annexb;
	int avcc = 0;
	int secondary_scan = 0;
	int secondary_miss = 0;

	/*
	 * Validate AVCC first.  A legitimate AVCC NAL length such as 0x00000100
	 * contains the Annex-B byte pattern and would otherwise be a false hit.
	 */
	avcc = screenprof_is_valid_avcc(data, bytes);
	annexb = avcc ? 0 :
		screenprof_find_annexb_prefix(data, bytes,
						 SCREENPROF_START_CODE_SCAN_BYTES,
						 &offset,
						 &prefix_bytes);
	if (annexb) {
		result.frame_class = screenprof_annexb_frame_class(
			data, bytes, offset, prefix_bytes, &result.first_nal_type,
			&secondary_scan, &secondary_miss);
	}

	taskENTER_CRITICAL();
	screenprof_live.format_samples++;
	if (annexb) {
		screenprof_live.annexb_frames++;
		screenprof_live.annexb_offset_sum += offset;
		if ((screenprof_live.annexb_frames == 1U) ||
		    (offset < screenprof_live.annexb_offset_min)) {
			screenprof_live.annexb_offset_min = offset;
		}
		if (offset > screenprof_live.annexb_offset_max) {
			screenprof_live.annexb_offset_max = offset;
		}
		if (offset == 0U) {
			screenprof_live.annexb_start_at_0++;
		} else if (offset == 4U) {
			screenprof_live.annexb_start_at_4++;
		} else if (offset <= 15U) {
			screenprof_live.annexb_start_1_15++;
		} else if (offset <= 63U) {
			screenprof_live.annexb_start_16_63++;
		} else {
			screenprof_live.annexb_start_64_255++;
		}
		if (prefix_bytes == 4U) {
			screenprof_live.annexb_4byte_prefix++;
		} else {
			screenprof_live.annexb_3byte_prefix++;
		}
		if (result.first_nal_type < 32U) {
			screenprof_live.first_nal_type[result.first_nal_type]++;
		}
		screenprof_live.secondary_vcl_scans +=
			(uint32_t)secondary_scan;
		screenprof_live.secondary_vcl_misses +=
			(uint32_t)secondary_miss;
	} else if (avcc) {
		screenprof_live.avcc_frames++;
	} else {
		screenprof_live.unknown_frames++;
	}
	taskEXIT_CRITICAL();
	return result;
}

/* Caller holds the task critical section. */
static void screenprof_record_class_arrival(uint32_t frame_class,
					    uint32_t bytes,
					    uint32_t arrival_delta_us,
					    int have_delta)
{
	screenprof_class_stats_t *stats;

	if (frame_class >= SCREENPROF_FRAME_CLASS_COUNT) {
		frame_class = SCREENPROF_FRAME_UNKNOWN;
	}
	stats = &screenprof_live.class_stats[frame_class];
	stats->frames++;
	stats->bytes += bytes;
	if (have_delta) {
		stats->delta_samples++;
		stats->delta_sum_us += arrival_delta_us;
		if (arrival_delta_us > stats->delta_max_us) {
			stats->delta_max_us = arrival_delta_us;
		}
	}
}

/* Caller holds the task critical section. */
static void screenprof_record_class_completion(uint32_t frame_class,
					       uint32_t queue_us,
					       uint32_t prepare_us,
					       uint32_t socket_us,
					       uint32_t handoff_us)
{
	screenprof_class_stats_t *stats;

	if (frame_class >= SCREENPROF_FRAME_CLASS_COUNT) {
		frame_class = SCREENPROF_FRAME_UNKNOWN;
	}
	stats = &screenprof_live.class_stats[frame_class];
	stats->completed++;
	stats->queue_sum_us += queue_us;
	stats->prepare_sum_us += prepare_us;
	stats->socket_sum_us += socket_us;
	stats->handoff_sum_us += handoff_us;
	if (queue_us > stats->queue_max_us) {
		stats->queue_max_us = queue_us;
	}
	if (prepare_us > stats->prepare_max_us) {
		stats->prepare_max_us = prepare_us;
	}
	if (socket_us > stats->socket_max_us) {
		stats->socket_max_us = socket_us;
	}
	if (handoff_us > stats->handoff_max_us) {
		stats->handoff_max_us = handoff_us;
	}
}

void __wrap_AirPlayScreen_SendVideo(const void *data, int bytes)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	screenprof_active_input_t input;
	uint32_t now_us = hal_read_curtime_us();
	int marked = screenprof_mark_active(current);
	int have_arrival_delta;

	taskENTER_CRITICAL();
	input.arrival_us = now_us;
	input.sequence = ++screenprof_next_sequence;
	input.frame_class = SCREENPROF_FRAME_UNKNOWN;
	input.first_nal_type = 0U;
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
	input.source_ntp_valid = screenprof_pending_source_ntp_valid;
	input.source_ntp = screenprof_pending_source_ntp;
	if (input.source_ntp_valid) {
		screenprof_live.source_callback_paired++;
		screenprof_pending_source_ntp_valid = 0U;
	} else {
		screenprof_live.source_callback_missing++;
	}
#endif
	taskEXIT_CRITICAL();
#if CONFIG_SCREEN_FRAME_FORMAT_PROFILE
	{
		screenprof_probe_result_t probe =
			screenprof_probe_frame_format(data, bytes);
		input.frame_class = probe.frame_class;
		input.first_nal_type = probe.first_nal_type;
	}
#else
	(void)data;
#endif
	taskENTER_CRITICAL();
	have_arrival_delta = screenprof_have_last_arrival != 0U;
	input.arrival_delta_us = have_arrival_delta ?
		(now_us - screenprof_last_arrival_us) : 0U;
	if (have_arrival_delta) {
		screenprof_record_metric(SCREENPROF_METRIC_ARRIVAL_DELTA,
					 input.arrival_delta_us);
		if (input.arrival_delta_us > 40000U) {
			screenprof_live.gap_over_40ms++;
		}
		if (input.arrival_delta_us > 50000U) {
			screenprof_live.gap_over_50ms++;
		}
		if (input.arrival_delta_us > 100000U) {
			screenprof_live.gap_over_100ms++;
		}
	}
	screenprof_last_arrival_us = now_us;
	screenprof_have_last_arrival = 1U;
	screenprof_live.video_calls++;
	if (bytes > 0) {
		screenprof_live.video_bytes += (uint32_t)bytes;
	}
	screenprof_record_class_arrival(input.frame_class,
		(bytes > 0) ? (uint32_t)bytes : 0U,
		input.arrival_delta_us, have_arrival_delta);
	taskEXIT_CRITICAL();
	screenprof_set_active_input(current, &input);
	__real_AirPlayScreen_SendVideo(data, bytes);
	if (marked) {
		screenprof_unmark_active(current);
	}
}

#if CONFIG_SCREEN_TIMESTAMP_PROFILE
/*
 * The closed Accessory AirPlayScreen.o obtains UpTicks() immediately before
 * this conversion for every normal screen frame.  UpTicks() is implemented as
 * GetTickCount(), so the input is the local millisecond clock, not a source
 * timestamp carried by AirPlayScreen_SendVideo(data, bytes).
 *
 * Capture only while a dequeued frame is active.  This preserves the original
 * conversion and wire value while allowing its cadence to be compared with
 * the complete-frame arrival cadence measured above.  A config frame can call
 * the converter more than once; only the first call is paired with the frame.
 */
uint64_t __wrap_UpTicksToNTP(uint64_t ticks)
{
	uint64_t ntp = __real_UpTicksToNTP(ticks);
	uint32_t now_us = hal_read_curtime_us();
	uint32_t ticks_ms = (uint32_t)ticks;

	taskENTER_CRITICAL();
	screenprof_live.timestamp_calls++;
	if (screenprof_active_frame.valid &&
	    !screenprof_active_frame.timestamp_captured) {
		uint32_t after_arrival_us =
			now_us - screenprof_active_frame.arrival_us;

		screenprof_active_frame.timestamp_captured = 1U;
		screenprof_live.timestamp_paired++;
		screenprof_live.timestamp_tick_last_ms = ticks_ms;
		screenprof_live.timestamp_ntp_last = ntp;
		screenprof_record_metric(SCREENPROF_METRIC_STAMP_AFTER_ARRIVAL,
					 after_arrival_us);

		if (screenprof_timestamp_have_prev) {
			uint32_t delta_ms = ticks_ms -
				screenprof_timestamp_prev_ms;
			uint32_t delta_us;
			uint32_t arrival_delta_us =
				screenprof_active_frame.arrival_delta_us;
			uint32_t error_us;

			if (delta_ms > 0x7FFFFFFFU) {
				screenprof_live.timestamp_tick_regressions++;
			} else {
				delta_us = (delta_ms <= (UINT32_MAX / 1000U)) ?
					delta_ms * 1000U : UINT32_MAX;
				error_us = (delta_us >= arrival_delta_us) ?
					delta_us - arrival_delta_us :
					arrival_delta_us - delta_us;
				screenprof_live.timestamp_delta_samples++;
				screenprof_record_metric(
					SCREENPROF_METRIC_STAMP_DELTA, delta_us);
				screenprof_record_metric(
					SCREENPROF_METRIC_STAMP_ERROR, error_us);
				if (error_us <= 1000U) {
					screenprof_live.timestamp_error_le_1ms++;
				}
				if (error_us <= 5000U) {
					screenprof_live.timestamp_error_le_5ms++;
				}
				if (error_us <= 20000U) {
					screenprof_live.timestamp_error_le_20ms++;
				}
			}
		}
		screenprof_timestamp_prev_ms = ticks_ms;
		screenprof_timestamp_have_prev = 1U;
	} else if (screenprof_active_frame.valid) {
		screenprof_live.timestamp_duplicate++;
	} else {
		screenprof_live.timestamp_unpaired++;
	}
	taskEXIT_CRITICAL();

	return ntp;
}
#endif

void __wrap_CVector_push_back(void *vector, const void *element)
{
	screenprof_frame_item_t item = { NULL, 0 };
	screenprof_active_input_t input = { 0 };
	uint32_t now_us = 0U;
	int is_screen = (vector == screenprof_vector);

	if (!is_screen && screenprof_current_is_active()) {
		screenprof_vector_view_t *view = (screenprof_vector_view_t *)vector;
		/* AirPlayScreen stores {frame pointer, frame length}: exactly 8 bytes. */
		if ((view != NULL) && (view->element_size == 8)) {
			screenprof_vector = vector;
			is_screen = 1;
		}
	}
	if (is_screen && (element != NULL)) {
		item = *(const screenprof_frame_item_t *)element;
		now_us = hal_read_curtime_us();
		(void)screenprof_get_active_input(xTaskGetCurrentTaskHandle(),
					  &input);
		if (input.arrival_us == 0U) {
			input.arrival_us = now_us;
		}
	}

	__real_CVector_push_back(vector, element);
	if (is_screen) {
		screenprof_vector_view_t *view = (screenprof_vector_view_t *)vector;
		taskENTER_CRITICAL();
		screenprof_live.enqueue_frames++;
		if (item.bytes > 0) {
			screenprof_live.enqueue_bytes += (uint32_t)item.bytes;
		}
		if (!screenprof_frame_tracking_invalid &&
		    (screenprof_frame_count < SCREENPROF_FRAME_TIMES)) {
			uint32_t tail = (screenprof_frame_head + screenprof_frame_count) %
					SCREENPROF_FRAME_TIMES;
			screenprof_frames[tail].arrival_us = input.arrival_us;
			screenprof_frames[tail].sequence = input.sequence;
			screenprof_frames[tail].arrival_delta_us =
				input.arrival_delta_us;
			screenprof_frames[tail].bytes =
				(item.bytes > 0) ? (uint32_t)item.bytes : 0U;
			screenprof_frames[tail].frame_class = input.frame_class;
			screenprof_frames[tail].first_nal_type =
				input.first_nal_type;
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
			screenprof_frames[tail].source_ntp_valid =
				input.source_ntp_valid;
			screenprof_frames[tail].source_ntp = input.source_ntp;
#endif
			screenprof_frame_count++;
		} else if (!screenprof_frame_tracking_invalid) {
			/*
			 * Do not pair later timestamps with older, untracked frames.
			 * Depth/counters remain valid; age tracking resumes when the
			 * real CVector drains completely.
			 */
			screenprof_live.queue_ring_overflow++;
			screenprof_frame_tracking_invalid = 1;
			screenprof_frame_head = 0U;
			screenprof_frame_count = 0U;
		}
		if ((view != NULL) && (view->size > 0) &&
		    ((uint32_t)view->size > screenprof_live.queue_max_depth)) {
			screenprof_live.queue_max_depth = (uint32_t)view->size;
		}
		taskEXIT_CRITICAL();
	}
}

void __wrap_CVector_erase(void *vector, int index)
{
	int is_screen = (vector == screenprof_vector) && (index == 0);
	screenprof_vector_view_t *view =
		is_screen ? (screenprof_vector_view_t *)vector : NULL;
	uint32_t now_us = is_screen ? hal_read_curtime_us() : 0U;
	uint32_t age_us = 0U;
	uint32_t bytes = 0U;
	screenprof_frame_t dequeued_frame = { 0 };
	int have_age = 0;
	int valid_erase = (view != NULL) && (view->size > 0);

	if (is_screen && valid_erase) {
		screenprof_frame_item_t *item =
			(screenprof_frame_item_t *)view->data;
		if ((item != NULL) && (item[0].bytes > 0)) {
			bytes = (uint32_t)item[0].bytes;
		}
		taskENTER_CRITICAL();
		if (!screenprof_frame_tracking_invalid &&
		    (screenprof_frame_count != 0U)) {
			screenprof_frame_t *frame =
				&screenprof_frames[screenprof_frame_head];
			dequeued_frame = *frame;
			age_us = now_us - frame->arrival_us;
			screenprof_frame_head = (screenprof_frame_head + 1U) %
						SCREENPROF_FRAME_TIMES;
			screenprof_frame_count--;
			have_age = 1;
		} else if (!screenprof_frame_tracking_invalid) {
			screenprof_live.queue_desync++;
		}
		taskEXIT_CRITICAL();
	}

	__real_CVector_erase(vector, index);
	if (is_screen && valid_erase) {
		taskENTER_CRITICAL();
		if (screenprof_active_frame.valid) {
			screenprof_live.frames_without_write++;
		}
		screenprof_active_frame = (screenprof_active_frame_t){ 0 };
		if (have_age) {
			screenprof_active_frame.valid = 1U;
			screenprof_active_frame.sequence = dequeued_frame.sequence;
			screenprof_active_frame.bytes = dequeued_frame.bytes;
			screenprof_active_frame.arrival_us =
				dequeued_frame.arrival_us;
			screenprof_active_frame.arrival_delta_us =
				dequeued_frame.arrival_delta_us;
			screenprof_active_frame.dequeue_us = now_us;
			screenprof_active_frame.queue_us = age_us;
			screenprof_active_frame.frame_class =
				dequeued_frame.frame_class;
			screenprof_active_frame.first_nal_type =
				dequeued_frame.first_nal_type;
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
			screenprof_active_frame.source_ntp_valid =
				dequeued_frame.source_ntp_valid;
			screenprof_active_frame.source_ntp =
				dequeued_frame.source_ntp;
#endif
			screenprof_record_metric(SCREENPROF_METRIC_QUEUE, age_us);
		}
		screenprof_live.dequeue_frames++;
		screenprof_live.dequeue_bytes += bytes;
		if (have_age) {
			screenprof_live.age_samples++;
			screenprof_live.age_sum_us += age_us;
			if (age_us > screenprof_live.age_max_us) {
				screenprof_live.age_max_us = age_us;
			}
		}
		if ((view->size == 0) && screenprof_frame_tracking_invalid) {
			screenprof_frame_tracking_invalid = 0;
			screenprof_frame_head = 0U;
			screenprof_frame_count = 0U;
		}
		taskEXIT_CRITICAL();
	}
}

void __wrap_CVector_delete(void *vector)
{
	if (vector == screenprof_vector) {
		taskENTER_CRITICAL();
		screenprof_vector = NULL;
		screenprof_frame_head = 0U;
		screenprof_frame_count = 0U;
		screenprof_frame_tracking_invalid = 0;
		taskEXIT_CRITICAL();
	}
	__real_CVector_delete(vector);
}

int __wrap_lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset,
		       fd_set *exceptset, struct timeval *timeout)
{
	int measured = screenprof_is_task(&screenprof_receiver_task,
					  "AirPlayScreenReceiver");
	uint32_t start_us = measured ? hal_read_curtime_us() : 0U;
	int result = __real_lwip_select(maxfdp1, readset, writeset, exceptset,
					timeout);

	if (measured) {
		uint32_t now_us = hal_read_curtime_us();
		uint32_t elapsed_us = now_us - start_us;

		taskENTER_CRITICAL();
		screenprof_live.select_calls++;
		screenprof_live.select_time_sum_us += elapsed_us;
		if (elapsed_us > screenprof_live.select_time_max_us) {
			screenprof_live.select_time_max_us = elapsed_us;
		}
		if (result > 0) {
			screenprof_live.select_ready++;
			if (elapsed_us <= SCREENPROF_IMMEDIATE_US) {
				uint32_t streak_us;

				screenprof_live.select_immediate++;
				if (screenprof_select_streak == 0U) {
					screenprof_select_streak_start_us = start_us;
				}
				screenprof_select_streak++;
				streak_us = now_us -
					screenprof_select_streak_start_us;
				if (screenprof_select_streak >
				    screenprof_live.select_streak_max) {
					screenprof_live.select_streak_max =
						screenprof_select_streak;
				}
				if (streak_us >
				    screenprof_live.select_streak_max_us) {
					screenprof_live.select_streak_max_us =
						streak_us;
				}
			} else {
				if (elapsed_us <= SCREENPROF_SHORT_WAIT_US) {
					screenprof_live.select_short_wait++;
				} else {
					screenprof_live.select_long_wait++;
				}
				screenprof_select_streak = 0U;
			}
		} else {
			if (result == 0) {
				screenprof_live.select_timeout++;
			} else {
				screenprof_live.select_error++;
			}
			screenprof_select_streak = 0U;
		}
		taskEXIT_CRITICAL();
	}
	return result;
}

ssize_t __wrap_lwip_recv(int socket, void *buffer, size_t bytes, int flags)
{
	int measured = screenprof_is_task(&screenprof_receiver_task,
					  "AirPlayScreenReceiver");
	uint32_t start_us;
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
	uint64_t received_source_ntp = 0U;
	int received_source_header = 0;
#endif

#if CONFIG_SCREEN_TCP_BUFFER_PROFILE
	if (measured && (bytes > 64U)) {
		struct lwip_tcp_buffer_diag diag;
		uint32_t probe_start_us = hal_read_curtime_us();
		int probe_result = lwip_diag_tcp_buffer_state(socket, &diag);
		uint32_t probe_us = hal_read_curtime_us() - probe_start_us;

		if (probe_result == 0) {
			screenprof_record_rx_buffer(&diag, probe_us);
		} else {
			taskENTER_CRITICAL();
			screenprof_live.rx_buffer_errors++;
			screenprof_record_probe_time(probe_us);
			taskEXIT_CRITICAL();
		}
	}
#endif
	start_us = measured ? hal_read_curtime_us() : 0U;
	ssize_t result = __real_lwip_recv(socket, buffer, bytes, flags);

#if CONFIG_SCREEN_TIMESTAMP_PROFILE
	if (measured && (bytes == 128U) && (result == 128) &&
	    (buffer != NULL)) {
		const uint8_t *header = (const uint8_t *)buffer;
		uint32_t body_len = screenprof_load_le32(header);
		uint8_t frame_type = header[4];

		/*
		 * AirPlayReceiverSessionScreen_ProcessFrames reads a native LE
		 * uint64 timestamp from header offset 8 and passes it to its time
		 * synchronizer. Validate the surrounding length/type fields so an
		 * uncommon 128-byte payload read cannot be mistaken for a header.
		 */
		if ((body_len >= 16U) && (body_len <= (4U * 1024U * 1024U)) &&
		    ((frame_type <= 2U) || (frame_type == 4U) ||
		     (frame_type == 5U))) {
			received_source_ntp = screenprof_load_le64(header + 8U);
			received_source_header = received_source_ntp != 0U;
		}
	}
#endif

	if (measured) {
		uint32_t elapsed_us = hal_read_curtime_us() - start_us;
		taskENTER_CRITICAL();
		screenprof_live.recv_calls++;
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
		if (received_source_header) {
			screenprof_pending_source_ntp = received_source_ntp;
			screenprof_pending_source_ntp_valid = 1U;
			screenprof_live.source_header_ntp++;
		}
#endif
		/*
		 * The closed Screen receiver requests its fixed protocol header as
		 * 128 bytes, its command socket in <=64-byte units, and the frame
		 * body as a larger exact-length read.  A residual 65..127-byte read
		 * is counted separately because it is normally the tail of a body.
		 */
		if (bytes <= 64U) {
			screenprof_live.recv_control_calls++;
		} else if (bytes == 128U) {
			screenprof_live.recv_header_calls++;
		} else if (bytes > 128U) {
			screenprof_live.recv_payload_calls++;
		} else {
			screenprof_live.recv_tail_calls++;
		}
		if (result > 0) {
			screenprof_live.recv_bytes += (uint32_t)result;
		} else if (result == 0) {
			screenprof_live.recv_zero++;
		} else {
			screenprof_live.recv_error++;
		}
		screenprof_live.recv_time_sum_us += elapsed_us;
		if (elapsed_us > screenprof_live.recv_time_max_us) {
			screenprof_live.recv_time_max_us = elapsed_us;
		}
		if (elapsed_us <= SCREENPROF_IMMEDIATE_US) {
			screenprof_live.recv_immediate++;
			if (result > 0) {
				screenprof_live.recv_immediate_bytes +=
					(uint32_t)result;
			}
			if ((bytes > 128U) && (result > 0)) {
				screenprof_live.recv_payload_immediate++;
				screenprof_live.recv_payload_immediate_bytes +=
					(uint32_t)result;
			}
		} else if (elapsed_us <= SCREENPROF_SHORT_WAIT_US) {
			screenprof_live.recv_short_wait++;
		} else {
			screenprof_live.recv_long_wait++;
		}
		taskEXIT_CRITICAL();
	}
	return result;
}

ssize_t __wrap_lwip_write(int socket, const void *buffer, size_t bytes)
{
	int measured = screenprof_is_task(&screenprof_sender_task, "ScreenThread");
	uint32_t start_us = measured ? hal_read_curtime_us() : 0U;

	if (measured) {
		taskENTER_CRITICAL();
		if (screenprof_active_frame.valid &&
		    (screenprof_active_frame.first_write_us == 0U)) {
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
			if ((buffer != NULL) && (bytes >= 16U)) {
				uint64_t wire_ntp = screenprof_load_le64(
					(const uint8_t *)buffer + 8U);

				screenprof_active_frame.wire_ntp = wire_ntp;
				screenprof_active_frame.wire_ntp_captured =
					wire_ntp != 0U;
				if (screenprof_active_frame.source_ntp_valid &&
				    screenprof_active_frame.wire_ntp_captured) {
					screenprof_live.wire_source_paired++;
					screenprof_record_wire_source_timeline(
						screenprof_active_frame.source_ntp,
						wire_ntp);
				} else {
					screenprof_live.wire_source_missing++;
				}
			} else {
				screenprof_live.wire_source_missing++;
			}
#endif
			screenprof_active_frame.first_write_us = start_us;
			screenprof_active_frame.expected_write_bytes =
				(uint32_t)bytes;
			screenprof_active_frame.prepare_us =
				start_us - screenprof_active_frame.dequeue_us;
			screenprof_record_metric(SCREENPROF_METRIC_PREPARE,
					 screenprof_active_frame.prepare_us);
		}
		taskEXIT_CRITICAL();
	}
	ssize_t result = __real_lwip_write(socket, buffer, bytes);

	if (measured) {
		uint32_t now_us = hal_read_curtime_us();
		uint32_t elapsed_us = now_us - start_us;
#if CONFIG_SCREEN_TCP_BUFFER_PROFILE
		struct lwip_tcp_buffer_diag diag;
		uint32_t probe_start_us = hal_read_curtime_us();
		int probe_result = lwip_diag_tcp_buffer_state(socket, &diag);
		uint32_t probe_us = hal_read_curtime_us() - probe_start_us;

		if (probe_result == 0) {
			screenprof_record_tx_buffer(&diag, probe_us);
		} else {
			taskENTER_CRITICAL();
			screenprof_live.tx_buffer_errors++;
			screenprof_record_probe_time(probe_us);
			 taskEXIT_CRITICAL();
		}
#endif
		taskENTER_CRITICAL();
		screenprof_live.write_calls++;
		screenprof_live.write_request_bytes += bytes;
		if (result >= 0) {
			screenprof_live.write_return_bytes += (uint32_t)result;
			if ((size_t)result < bytes) {
				screenprof_live.write_partial++;
			}
		} else {
			screenprof_live.write_error++;
		}
		screenprof_live.write_time_sum_us += elapsed_us;
		if (elapsed_us > screenprof_live.write_time_max_us) {
			screenprof_live.write_time_max_us = elapsed_us;
		}
		if (screenprof_active_frame.valid && (result > 0)) {
			screenprof_active_frame.accepted_write_bytes +=
				(uint32_t)result;
			if ((screenprof_active_frame.expected_write_bytes != 0U) &&
			    (screenprof_active_frame.accepted_write_bytes >=
			     screenprof_active_frame.expected_write_bytes)) {
				screenprof_anomaly_t anomaly;
				uint32_t socket_us =
					now_us - screenprof_active_frame.first_write_us;
				uint32_t handoff_us =
					now_us - screenprof_active_frame.arrival_us;
				uint32_t score_us = handoff_us;

				screenprof_record_metric(SCREENPROF_METRIC_SOCKET,
						 socket_us);
				screenprof_record_metric(SCREENPROF_METRIC_HANDOFF,
						 handoff_us);
				screenprof_record_class_completion(
					screenprof_active_frame.frame_class,
					screenprof_active_frame.queue_us,
					screenprof_active_frame.prepare_us,
					socket_us, handoff_us);
				screenprof_live.completed_frames++;
				if (screenprof_active_frame.arrival_delta_us > score_us) {
					score_us =
						screenprof_active_frame.arrival_delta_us;
				}
				anomaly = (screenprof_anomaly_t) {
					.sequence = screenprof_active_frame.sequence,
					.bytes = screenprof_active_frame.bytes,
					.arrival_delta_us =
						screenprof_active_frame.arrival_delta_us,
					.queue_us = screenprof_active_frame.queue_us,
					.prepare_us =
						screenprof_active_frame.prepare_us,
					.socket_us = socket_us,
					.handoff_us = handoff_us,
					.score_us = score_us,
					.frame_class =
						screenprof_active_frame.frame_class,
					.first_nal_type =
						screenprof_active_frame.first_nal_type
				};
				screenprof_record_anomaly(&anomaly);
				screenprof_active_frame.valid = 0U;
			}
		}
		taskEXIT_CRITICAL();
	}
	return result;
}

static uint32_t screenprof_average(uint64_t total, uint32_t count)
{
	return (count != 0U) ? (uint32_t)(total / count) : 0U;
}

static uint32_t screenprof_ratio_x10(uint32_t value, uint32_t total)
{
	return (total != 0U) ? (value * 1000U) / total : 0U;
}

static uint32_t screenprof_sqrt_u64(uint64_t value)
{
	uint64_t bit = (uint64_t)1U << 62;
	uint64_t result = 0U;

	while (bit > value) {
		bit >>= 2;
	}
	while (bit != 0U) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t)result;
}

static void screenprof_sort(uint32_t *values, uint32_t count)
{
	uint32_t i;

	/* Frame counts are normally about 300 per window; insertion sort keeps the
	 * profiler self-contained and runs only in the 10-second reporter task. */
	for (i = 1U; i < count; i++) {
		uint32_t value = values[i];
		uint32_t j = i;
		while ((j != 0U) && (values[j - 1U] > value)) {
			values[j] = values[j - 1U];
			j--;
		}
		values[j] = value;
	}
}

static uint32_t screenprof_percentile(const uint32_t *values, uint32_t count,
				      uint32_t percentile)
{
	uint32_t index;

	if (count == 0U) {
		return 0U;
	}
	index = (uint32_t)((((uint64_t)count * percentile) + 99U) / 100U);
	if (index == 0U) {
		index = 1U;
	}
	return values[index - 1U];
}

static uint32_t screenprof_metric_sigma(const screenprof_metric_t *metric)
{
	uint64_t mean;
	uint64_t mean_square;
	uint64_t variance;

	if (metric->count == 0U) {
		return 0U;
	}
	mean = metric->variance_sum_us / metric->count;
	mean_square = metric->sum_square_us / metric->count;
	variance = (mean_square > (mean * mean)) ?
		(mean_square - (mean * mean)) : 0U;
	return screenprof_sqrt_u64(variance);
}

static void screenprof_report_metric(uint32_t sequence, const char *name,
				     screenprof_metric_id_t metric_id)
{
	screenprof_metric_t *metric = &screenprof_metrics_copy[metric_id];
	uint32_t count = metric->sample_count;
	uint32_t *samples = screenprof_metric_samples_copy[metric_id];

	screenprof_sort(samples, count);
	rt_printf("[FRAMEPROF][%lu] %s_us n=%lu avg/sigma=%lu/%lu "
		  "p50/p95/p99/max=%lu/%lu/%lu/%lu sample_overflow=%lu "
		  "variance_clipped=%lu\r\n",
		  (unsigned long)sequence, name,
		  (unsigned long)metric->count,
		  (unsigned long)screenprof_average(metric->sum_us,
						      metric->count),
		  (unsigned long)screenprof_metric_sigma(metric),
		  (unsigned long)screenprof_percentile(samples, count, 50U),
		  (unsigned long)screenprof_percentile(samples, count, 95U),
		  (unsigned long)screenprof_percentile(samples, count, 99U),
		  (unsigned long)metric->max_us,
		  (unsigned long)metric->sample_overflow,
		  (unsigned long)metric->variance_clipped);
}

static void screenprof_report_arrival_jitter(uint32_t sequence)
{
	screenprof_metric_t *metric =
		&screenprof_metrics_copy[SCREENPROF_METRIC_ARRIVAL_DELTA];
	uint32_t count = metric->sample_count;
	uint32_t *samples =
		screenprof_metric_samples_copy[SCREENPROF_METRIC_ARRIVAL_DELTA];
	uint32_t median;
	uint64_t deviation_sum = 0U;
	uint32_t i;

	if (count == 0U) {
		return;
	}
	/* The delta metric was sorted by screenprof_report_metric(). */
	median = screenprof_percentile(samples, count, 50U);
	for (i = 0U; i < count; i++) {
		uint32_t sample = samples[i];
		uint32_t deviation = (sample >= median) ?
			(sample - median) : (median - sample);
		screenprof_sort_scratch[i] = deviation;
		deviation_sum += deviation;
	}
	screenprof_sort(screenprof_sort_scratch, count);
	rt_printf("[FRAMEPROF][%lu] input_jitter_us baseline=median:%lu "
		  "mean_abs/MAD/p95/p99/max=%lu/%lu/%lu/%lu/%lu "
		  "gaps_gt_40/50/100ms=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence, (unsigned long)median,
		  (unsigned long)(deviation_sum / count),
		  (unsigned long)screenprof_percentile(screenprof_sort_scratch,
							 count, 50U),
		  (unsigned long)screenprof_percentile(screenprof_sort_scratch,
							 count, 95U),
		  (unsigned long)screenprof_percentile(screenprof_sort_scratch,
							 count, 99U),
		  (unsigned long)screenprof_sort_scratch[count - 1U],
		  (unsigned long)screenprof_copy.gap_over_40ms,
		  (unsigned long)screenprof_copy.gap_over_50ms,
		  (unsigned long)screenprof_copy.gap_over_100ms);
}

static const char *screenprof_frame_class_name(uint32_t frame_class)
{
	if (frame_class == SCREENPROF_FRAME_IDR) {
		return "idr";
	}
	if (frame_class == SCREENPROF_FRAME_NON_IDR) {
		return "non_idr";
	}
	return "unknown";
}

static void screenprof_report_frame_class(uint32_t sequence,
					  uint32_t frame_class)
{
	screenprof_class_stats_t *stats =
		&screenprof_copy.class_stats[frame_class];

	rt_printf("[FRAMECLASS][%lu] %s frames=%lu bytes=%llu avg_size=%lu "
		  "delta_n/avg/max=%lu/%lu/%lu completed=%lu "
		  "queue_avg/max=%lu/%lu prepare_avg/max=%lu/%lu "
		  "socket_avg/max=%lu/%lu handoff_avg/max=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  screenprof_frame_class_name(frame_class),
		  (unsigned long)stats->frames,
		  (unsigned long long)stats->bytes,
		  (unsigned long)screenprof_average(stats->bytes,
						      stats->frames),
		  (unsigned long)stats->delta_samples,
		  (unsigned long)screenprof_average(stats->delta_sum_us,
						      stats->delta_samples),
		  (unsigned long)stats->delta_max_us,
		  (unsigned long)stats->completed,
		  (unsigned long)screenprof_average(stats->queue_sum_us,
						      stats->completed),
		  (unsigned long)stats->queue_max_us,
		  (unsigned long)screenprof_average(stats->prepare_sum_us,
						      stats->completed),
		  (unsigned long)stats->prepare_max_us,
		  (unsigned long)screenprof_average(stats->socket_sum_us,
						      stats->completed),
		  (unsigned long)stats->socket_max_us,
		  (unsigned long)screenprof_average(stats->handoff_sum_us,
						      stats->completed),
		  (unsigned long)stats->handoff_max_us);
}

void screen_queue_profiler_report(uint32_t sequence)
{
	uint32_t current_depth;
	uint32_t vector_depth = 0U;
	uint32_t metric_id;
	uint32_t sample;
	uint32_t anomaly;
#if CONFIG_SCREEN_FRAME_FORMAT_PROFILE
	uint32_t nal_type;
	uint32_t first_nal_other = 0U;
#endif
	uint32_t fps_x100;
	uint32_t select_immediate_x10;
	uint32_t recv_immediate_x10;
	uint32_t recv_immediate_bytes_x10;
	int age_valid;
	void *vector;

	taskENTER_CRITICAL();
	screenprof_copy = screenprof_live;
	screenprof_live = (screenprof_stats_t){ 0 };
	for (metric_id = 0U; metric_id < SCREENPROF_METRIC_COUNT;
	     metric_id++) {
		screenprof_metrics_copy[metric_id] =
			screenprof_metrics[metric_id];
		for (sample = 0U;
		     sample < screenprof_metrics[metric_id].sample_count;
		     sample++) {
			screenprof_metric_samples_copy[metric_id][sample] =
				screenprof_metric_samples[metric_id][sample];
		}
		screenprof_metrics[metric_id] = (screenprof_metric_t){ 0 };
	}
	screenprof_anomaly_count_copy = screenprof_anomaly_count;
	for (anomaly = 0U; anomaly < screenprof_anomaly_count; anomaly++) {
		screenprof_anomalies_copy[anomaly] =
			screenprof_anomalies[anomaly];
	}
	screenprof_anomaly_count = 0U;
	/* Keep every report window independent, including backlog streaks. */
	screenprof_select_streak = 0U;
	screenprof_select_streak_start_us = 0U;
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
	/* Do not compare the first generated stamp with the prior report window. */
	screenprof_timestamp_have_prev = 0U;
#endif
	current_depth = screenprof_frame_count;
	age_valid = !screenprof_frame_tracking_invalid;
	vector = screenprof_vector;
	if (vector != NULL) {
		screenprof_vector_view_t *view =
			(screenprof_vector_view_t *)vector;
		if (view->size > 0) {
			vector_depth = (uint32_t)view->size;
		}
	}
	screenprof_live.queue_max_depth = vector_depth;
	taskEXIT_CRITICAL();

	fps_x100 = screenprof_copy.video_calls * 10U;
	rt_printf("[FRAMEPROF][%lu] window_ms=10000 frames=%lu bytes=%llu "
		  "fps=%lu.%02lu completed=%lu no_write=%lu "
		  "boundary=complete-frame-to-lwip-write-accepted\r\n",
		  (unsigned long)sequence,
		  (unsigned long)screenprof_copy.video_calls,
		  (unsigned long long)screenprof_copy.video_bytes,
		  (unsigned long)(fps_x100 / 100U),
		  (unsigned long)(fps_x100 % 100U),
		  (unsigned long)screenprof_copy.completed_frames,
		  (unsigned long)screenprof_copy.frames_without_write);
#if CONFIG_SCREEN_FRAME_FORMAT_PROFILE
	rt_printf("[FRAMEFMT][%lu] samples=%lu annexb=%lu avcc=%lu unknown=%lu "
		  "scan_prefix_bytes=%u avcc_max_nals=%u\r\n",
		  (unsigned long)sequence,
		  (unsigned long)screenprof_copy.format_samples,
		  (unsigned long)screenprof_copy.annexb_frames,
		  (unsigned long)screenprof_copy.avcc_frames,
		  (unsigned long)screenprof_copy.unknown_frames,
		  SCREENPROF_START_CODE_SCAN_BYTES,
		  SCREENPROF_AVCC_MAX_NALS);
	rt_printf("[FRAMEFMT][%lu] annexb_offset at0=%lu at4=%lu "
		  "at1_15=%lu at16_63=%lu at64_255=%lu avg/min/max=%lu/%lu/%lu "
		  "prefix3/4=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)screenprof_copy.annexb_start_at_0,
		  (unsigned long)screenprof_copy.annexb_start_at_4,
		  (unsigned long)screenprof_copy.annexb_start_1_15,
		  (unsigned long)screenprof_copy.annexb_start_16_63,
		  (unsigned long)screenprof_copy.annexb_start_64_255,
		  (unsigned long)screenprof_average(
			  screenprof_copy.annexb_offset_sum,
			  screenprof_copy.annexb_frames),
		  (unsigned long)screenprof_copy.annexb_offset_min,
		  (unsigned long)screenprof_copy.annexb_offset_max,
		  (unsigned long)screenprof_copy.annexb_3byte_prefix,
		  (unsigned long)screenprof_copy.annexb_4byte_prefix);
	for (nal_type = 0U; nal_type < 32U; nal_type++) {
		if ((nal_type != 1U) && (nal_type != 5U) &&
		    (nal_type != 6U) && (nal_type != 7U) &&
		    (nal_type != 8U) && (nal_type != 9U)) {
			first_nal_other +=
				screenprof_copy.first_nal_type[nal_type];
		}
	}
	rt_printf("[FRAMEFMT][%lu] first_nal t1/t5/t6/t7/t8/t9/other="
		  "%lu/%lu/%lu/%lu/%lu/%lu/%lu secondary_scan/miss=%lu/%lu "
		  "vcl_scan_bytes=%u\r\n",
		  (unsigned long)sequence,
		  (unsigned long)screenprof_copy.first_nal_type[1],
		  (unsigned long)screenprof_copy.first_nal_type[5],
		  (unsigned long)screenprof_copy.first_nal_type[6],
		  (unsigned long)screenprof_copy.first_nal_type[7],
		  (unsigned long)screenprof_copy.first_nal_type[8],
		  (unsigned long)screenprof_copy.first_nal_type[9],
		  (unsigned long)first_nal_other,
		  (unsigned long)screenprof_copy.secondary_vcl_scans,
		  (unsigned long)screenprof_copy.secondary_vcl_misses,
		  SCREENPROF_VCL_SCAN_BYTES);
	screenprof_report_frame_class(sequence, SCREENPROF_FRAME_IDR);
	screenprof_report_frame_class(sequence, SCREENPROF_FRAME_NON_IDR);
	screenprof_report_frame_class(sequence, SCREENPROF_FRAME_UNKNOWN);
#endif
	screenprof_report_metric(sequence, "arrival_delta",
				 SCREENPROF_METRIC_ARRIVAL_DELTA);
	screenprof_report_arrival_jitter(sequence);
	screenprof_report_metric(sequence, "queue",
				 SCREENPROF_METRIC_QUEUE);
	screenprof_report_metric(sequence, "prepare",
				 SCREENPROF_METRIC_PREPARE);
	screenprof_report_metric(sequence, "socket",
				 SCREENPROF_METRIC_SOCKET);
	screenprof_report_metric(sequence, "socket_handoff",
				 SCREENPROF_METRIC_HANDOFF);
#if CONFIG_SCREEN_TIMESTAMP_PROFILE
	screenprof_report_metric(sequence, "out_stamp_delta",
				 SCREENPROF_METRIC_STAMP_DELTA);
	screenprof_report_metric(sequence, "stamp_vs_arrival_abs_error",
				 SCREENPROF_METRIC_STAMP_ERROR);
	screenprof_report_metric(sequence, "stamp_after_arrival",
				 SCREENPROF_METRIC_STAMP_AFTER_ARRIVAL);
	{
		uint32_t samples = screenprof_copy.timestamp_delta_samples;
		uint32_t le1_x10 = screenprof_ratio_x10(
			screenprof_copy.timestamp_error_le_1ms, samples);
		uint32_t le5_x10 = screenprof_ratio_x10(
			screenprof_copy.timestamp_error_le_5ms, samples);
		uint32_t le20_x10 = screenprof_ratio_x10(
			screenprof_copy.timestamp_error_le_20ms, samples);

		rt_printf("[TIMESTAMPPROF][%lu] source=local-GetTickCount-ms "
			  "calls=%lu paired=%lu unpaired=%lu duplicate=%lu "
			  "delta_samples=%lu regress=%lu tick_last_ms=%lu "
			  "ntp_last=%08lx:%08lx\r\n",
			  (unsigned long)sequence,
			  (unsigned long)screenprof_copy.timestamp_calls,
			  (unsigned long)screenprof_copy.timestamp_paired,
			  (unsigned long)screenprof_copy.timestamp_unpaired,
			  (unsigned long)screenprof_copy.timestamp_duplicate,
			  (unsigned long)samples,
			  (unsigned long)
				screenprof_copy.timestamp_tick_regressions,
			  (unsigned long)
				screenprof_copy.timestamp_tick_last_ms,
			  (unsigned long)
				(screenprof_copy.timestamp_ntp_last >> 32),
			  (unsigned long)
				(uint32_t)screenprof_copy.timestamp_ntp_last);
	rt_printf("[TIMESTAMPPROF][%lu] abs(out_stamp_delta-arrival_delta) "
			  "le1/le5/le20ms=%lu(%lu.%lu%%)/%lu(%lu.%lu%%)/"
			  "%lu(%lu.%lu%%)\r\n",
			  (unsigned long)sequence,
			  (unsigned long)
				screenprof_copy.timestamp_error_le_1ms,
			  (unsigned long)(le1_x10 / 10U),
			  (unsigned long)(le1_x10 % 10U),
			  (unsigned long)
				screenprof_copy.timestamp_error_le_5ms,
			  (unsigned long)(le5_x10 / 10U),
			  (unsigned long)(le5_x10 % 10U),
			  (unsigned long)
				screenprof_copy.timestamp_error_le_20ms,
			  (unsigned long)(le20_x10 / 10U),
			  (unsigned long)(le20_x10 % 10U));

	screenprof_report_metric(sequence, "iphone_ntp_delta",
				 SCREENPROF_METRIC_SOURCE_NTP_DELTA);
	screenprof_report_metric(sequence, "wire_ntp_delta",
				 SCREENPROF_METRIC_WIRE_NTP_DELTA);
	screenprof_report_metric(sequence, "wire_vs_iphone_delta_abs_error",
				 SCREENPROF_METRIC_WIRE_SOURCE_ERROR);
	{
		int64_t error_change =
			screenprof_copy.timeline_error_last_us -
			screenprof_copy.timeline_error_first_us;
		int64_t drift_ppm = 0;

		if (screenprof_copy.timeline_window_source_elapsed_us != 0U) {
			drift_ppm = (error_change * 1000000LL) /
				(int64_t)
				screenprof_copy.timeline_window_source_elapsed_us;
		}
		rt_printf("[TIMELINEPROF][%lu] iphone_header=%lu "
			  "callback paired/missing=%lu/%lu "
			  "wire paired/missing=%lu/%lu intervals=%lu "
			  "baseline_reset/regress=%lu/%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)screenprof_copy.source_header_ntp,
			  (unsigned long)
				screenprof_copy.source_callback_paired,
			  (unsigned long)
				screenprof_copy.source_callback_missing,
			  (unsigned long)screenprof_copy.wire_source_paired,
			  (unsigned long)screenprof_copy.wire_source_missing,
			  (unsigned long)
				screenprof_copy.timeline_interval_samples,
			  (unsigned long)
				screenprof_copy.timeline_baseline_resets,
			  (unsigned long)
				screenprof_copy.timeline_regressions);
		rt_printf("[TIMELINEPROF][%lu] normalized_error_us "
			  "current/min/max=%lld/%lld/%lld "
			  "window_change=%lld source_elapsed=%lluus "
			  "slope=%lldppm abs_avg/max=%llu/%lluus samples=%lu\r\n",
			  (unsigned long)sequence,
			  (long long)screenprof_copy.timeline_error_last_us,
			  (long long)screenprof_copy.timeline_error_min_us,
			  (long long)screenprof_copy.timeline_error_max_us,
			  (long long)error_change,
			  (unsigned long long)
				screenprof_copy.timeline_window_source_elapsed_us,
			  (long long)drift_ppm,
			  (unsigned long long)(screenprof_copy.timeline_samples ?
				screenprof_copy.timeline_abs_error_sum_us /
				screenprof_copy.timeline_samples : 0U),
			  (unsigned long long)
				screenprof_copy.timeline_abs_error_max_us,
			  (unsigned long)screenprof_copy.timeline_samples);
	}
	}
#endif
	select_immediate_x10 = screenprof_copy.select_ready ?
		(screenprof_copy.select_immediate * 1000U) /
		 screenprof_copy.select_ready : 0U;
	recv_immediate_x10 = screenprof_copy.recv_calls ?
		(screenprof_copy.recv_immediate * 1000U) /
		 screenprof_copy.recv_calls : 0U;
	recv_immediate_bytes_x10 = screenprof_copy.recv_bytes ?
		(uint32_t)((screenprof_copy.recv_immediate_bytes * 1000ULL) /
			   screenprof_copy.recv_bytes) : 0U;
	rt_printf("[RXBACKLOG][%lu] select calls/ready=%lu/%lu "
		  "immediate=%lu(%lu.%lu%%) wait_le_1ms/gt_1ms=%lu/%lu "
		  "timeout/error=%lu/%lu time_avg/max_us=%lu/%lu "
		  "immediate_streak_max=%lu/%luus\r\n",
		  (unsigned long)sequence,
		  (unsigned long)screenprof_copy.select_calls,
		  (unsigned long)screenprof_copy.select_ready,
		  (unsigned long)screenprof_copy.select_immediate,
		  (unsigned long)(select_immediate_x10 / 10U),
		  (unsigned long)(select_immediate_x10 % 10U),
		  (unsigned long)screenprof_copy.select_short_wait,
		  (unsigned long)screenprof_copy.select_long_wait,
		  (unsigned long)screenprof_copy.select_timeout,
		  (unsigned long)screenprof_copy.select_error,
		  (unsigned long)screenprof_average(
			  screenprof_copy.select_time_sum_us,
			  screenprof_copy.select_calls),
		  (unsigned long)screenprof_copy.select_time_max_us,
		  (unsigned long)screenprof_copy.select_streak_max,
		  (unsigned long)screenprof_copy.select_streak_max_us);
	rt_printf("[RXBACKLOG][%lu] recv calls=%lu "
		  "class=control/header/payload/tail:%lu/%lu/%lu/%lu "
		  "immediate=%lu(%lu.%lu%%)/%lluB(%lu.%lu%%) "
		  "wait_le_1ms/gt_1ms=%lu/%lu "
		  "payload_immediate=%lu/%lluB thresholds_us=%u/%u\r\n",
		  (unsigned long)sequence,
		  (unsigned long)screenprof_copy.recv_calls,
		  (unsigned long)screenprof_copy.recv_control_calls,
		  (unsigned long)screenprof_copy.recv_header_calls,
		  (unsigned long)screenprof_copy.recv_payload_calls,
		  (unsigned long)screenprof_copy.recv_tail_calls,
		  (unsigned long)screenprof_copy.recv_immediate,
		  (unsigned long)(recv_immediate_x10 / 10U),
		  (unsigned long)(recv_immediate_x10 % 10U),
		  (unsigned long long)screenprof_copy.recv_immediate_bytes,
		  (unsigned long)(recv_immediate_bytes_x10 / 10U),
		  (unsigned long)(recv_immediate_bytes_x10 % 10U),
		  (unsigned long)screenprof_copy.recv_short_wait,
		  (unsigned long)screenprof_copy.recv_long_wait,
		  (unsigned long)screenprof_copy.recv_payload_immediate,
		  (unsigned long long)
			screenprof_copy.recv_payload_immediate_bytes,
		  SCREENPROF_IMMEDIATE_US, SCREENPROF_SHORT_WAIT_US);
#if CONFIG_SCREEN_TCP_BUFFER_PROFILE
	{
		uint32_t rx75 = screenprof_ratio_x10(
			screenprof_copy.rx_window_ge75,
			screenprof_copy.rx_buffer_samples);
		uint32_t rx90 = screenprof_ratio_x10(
			screenprof_copy.rx_window_ge90,
			screenprof_copy.rx_buffer_samples);
		uint32_t rx100 = screenprof_ratio_x10(
			screenprof_copy.rx_window_full,
			screenprof_copy.rx_buffer_samples);
		uint32_t tx75 = screenprof_ratio_x10(
			screenprof_copy.tx_buffer_ge75,
			screenprof_copy.tx_buffer_samples);
		uint32_t tx90 = screenprof_ratio_x10(
			screenprof_copy.tx_buffer_ge90,
			screenprof_copy.tx_buffer_samples);
		uint32_t tx100 = screenprof_ratio_x10(
			screenprof_copy.tx_buffer_full,
			screenprof_copy.tx_buffer_samples);
		uint32_t probe_count = screenprof_copy.rx_buffer_samples +
			screenprof_copy.rx_buffer_errors +
			screenprof_copy.tx_buffer_samples +
			screenprof_copy.tx_buffer_errors;

		rt_printf("[BUFPROF][%lu] RX samples/error=%lu/%lu "
			  "pending_last/avg/max=%lu/%lu/%luB "
			  "window_used_last/avg/max/cap=%lu/%lu/%lu/%luB "
			  "ge75/ge90/full=%lu(%lu.%lu%%)/%lu(%lu.%lu%%)/"
			  "%lu(%lu.%lu%%)\r\n",
			  (unsigned long)sequence,
			  (unsigned long)screenprof_copy.rx_buffer_samples,
			  (unsigned long)screenprof_copy.rx_buffer_errors,
			  (unsigned long)screenprof_copy.rx_pending_last,
			  (unsigned long)screenprof_average(
				  screenprof_copy.rx_pending_sum,
				  screenprof_copy.rx_buffer_samples),
			  (unsigned long)screenprof_copy.rx_pending_max,
			  (unsigned long)screenprof_copy.rx_window_used_last,
			  (unsigned long)screenprof_average(
				  screenprof_copy.rx_window_used_sum,
				  screenprof_copy.rx_buffer_samples),
			  (unsigned long)screenprof_copy.rx_window_used_max,
			  (unsigned long)screenprof_copy.rx_window_capacity,
			  (unsigned long)screenprof_copy.rx_window_ge75,
			  (unsigned long)(rx75 / 10U),
			  (unsigned long)(rx75 % 10U),
			  (unsigned long)screenprof_copy.rx_window_ge90,
			  (unsigned long)(rx90 / 10U),
			  (unsigned long)(rx90 % 10U),
			  (unsigned long)screenprof_copy.rx_window_full,
			  (unsigned long)(rx100 / 10U),
			  (unsigned long)(rx100 % 10U));
		rt_printf("[BUFPROF][%lu] TX samples/error=%lu/%lu "
			  "used_last/avg/max/cap=%lu/%lu/%lu/%luB "
			  "ge75/ge90/full=%lu(%lu.%lu%%)/%lu(%lu.%lu%%)/"
			  "%lu(%lu.%lu%%) queue_last/max/cap=%lu/%lu/%lu "
			  "probe_us_avg/max=%lu/%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)screenprof_copy.tx_buffer_samples,
			  (unsigned long)screenprof_copy.tx_buffer_errors,
			  (unsigned long)screenprof_copy.tx_buffer_used_last,
			  (unsigned long)screenprof_average(
				  screenprof_copy.tx_buffer_used_sum,
				  screenprof_copy.tx_buffer_samples),
			  (unsigned long)screenprof_copy.tx_buffer_used_max,
			  (unsigned long)screenprof_copy.tx_buffer_capacity,
			  (unsigned long)screenprof_copy.tx_buffer_ge75,
			  (unsigned long)(tx75 / 10U),
			  (unsigned long)(tx75 % 10U),
			  (unsigned long)screenprof_copy.tx_buffer_ge90,
			  (unsigned long)(tx90 / 10U),
			  (unsigned long)(tx90 % 10U),
			  (unsigned long)screenprof_copy.tx_buffer_full,
			  (unsigned long)(tx100 / 10U),
			  (unsigned long)(tx100 % 10U),
			  (unsigned long)screenprof_copy.tx_queue_last,
			  (unsigned long)screenprof_copy.tx_queue_max,
			  (unsigned long)screenprof_copy.tx_queue_capacity,
			  (unsigned long)screenprof_average(
				  screenprof_copy.buffer_probe_time_sum_us,
				  probe_count),
			  (unsigned long)screenprof_copy.buffer_probe_time_max_us);
	}
#endif
	for (anomaly = 0U; anomaly < screenprof_anomaly_count_copy;
	     anomaly++) {
		screenprof_anomaly_t *event =
			&screenprof_anomalies_copy[anomaly];
		rt_printf("[FRAMEPROF][%lu][SLOW] frame=%lu size=%lu "
#if CONFIG_SCREEN_FRAME_FORMAT_PROFILE
			  "class=%s first_nal=%lu "
#endif
			  "delta/queue/prepare/socket/handoff_us="
			  "%lu/%lu/%lu/%lu/%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)event->sequence,
			  (unsigned long)event->bytes,
#if CONFIG_SCREEN_FRAME_FORMAT_PROFILE
			  screenprof_frame_class_name(event->frame_class),
			  (unsigned long)event->first_nal_type,
#endif
			  (unsigned long)event->arrival_delta_us,
			  (unsigned long)event->queue_us,
			  (unsigned long)event->prepare_us,
			  (unsigned long)event->socket_us,
			  (unsigned long)event->handoff_us);
	}

	rt_printf("[SCREENQ][%lu] window_ms=10000 vector=0x%08lx "
		  "video=%lu/%lluB enq=%lu/%lluB deq=%lu/%lluB "
		  "depth=%lu tracked=%lu max=%lu age_valid=%d overflow=%lu desync=%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)(uintptr_t)vector,
		  (unsigned long)screenprof_copy.video_calls,
		  (unsigned long long)screenprof_copy.video_bytes,
		  (unsigned long)screenprof_copy.enqueue_frames,
		  (unsigned long long)screenprof_copy.enqueue_bytes,
		  (unsigned long)screenprof_copy.dequeue_frames,
		  (unsigned long long)screenprof_copy.dequeue_bytes,
		  (unsigned long)vector_depth, (unsigned long)current_depth,
		  (unsigned long)screenprof_copy.queue_max_depth,
		  age_valid,
		  (unsigned long)screenprof_copy.queue_ring_overflow,
		  (unsigned long)screenprof_copy.queue_desync);
	rt_printf("[SCREENQ][%lu] frame_age_us samples=%lu avg/max=%lu/%lu "
		  "recv=%lu/%lluB time_avg/max=%lu/%lu zero/error=%lu/%lu "
		  "write=%lu req/ret=%llu/%lluB time_avg/max=%lu/%lu partial/error=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)screenprof_copy.age_samples,
		  (unsigned long)screenprof_average(screenprof_copy.age_sum_us,
						      screenprof_copy.age_samples),
		  (unsigned long)screenprof_copy.age_max_us,
		  (unsigned long)screenprof_copy.recv_calls,
		  (unsigned long long)screenprof_copy.recv_bytes,
		  (unsigned long)screenprof_average(screenprof_copy.recv_time_sum_us,
						      screenprof_copy.recv_calls),
		  (unsigned long)screenprof_copy.recv_time_max_us,
		  (unsigned long)screenprof_copy.recv_zero,
		  (unsigned long)screenprof_copy.recv_error,
		  (unsigned long)screenprof_copy.write_calls,
		  (unsigned long long)screenprof_copy.write_request_bytes,
		  (unsigned long long)screenprof_copy.write_return_bytes,
		  (unsigned long)screenprof_average(screenprof_copy.write_time_sum_us,
						      screenprof_copy.write_calls),
		  (unsigned long)screenprof_copy.write_time_max_us,
		  (unsigned long)screenprof_copy.write_partial,
		  (unsigned long)screenprof_copy.write_error);
}

#else

void screen_queue_profiler_report(uint32_t sequence)
{
	(void)sequence;
}

#endif /* CONFIG_SCREEN_QUEUE_PROFILE */
