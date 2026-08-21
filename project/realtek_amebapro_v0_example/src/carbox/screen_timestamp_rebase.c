#include "screen_timestamp_rebase.h"

#include <limits.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "hal_timer.h"

#ifndef CONFIG_SCREEN_TIMESTAMP_REBASE
#define CONFIG_SCREEN_TIMESTAMP_REBASE 0
#endif

#ifndef CONFIG_SCREEN_TIMESTAMP_REBASE_PROFILE
#define CONFIG_SCREEN_TIMESTAMP_REBASE_PROFILE 0
#endif

#ifndef CONFIG_SCREEN_TIMESTAMP_REBASE_APPLY
#define CONFIG_SCREEN_TIMESTAMP_REBASE_APPLY 0
#endif

#ifndef CONFIG_SCREEN_TIMESTAMP_REBASE_BOUNDED
#define CONFIG_SCREEN_TIMESTAMP_REBASE_BOUNDED 0
#endif

#if CONFIG_SCREEN_TIMESTAMP_REBASE

#define SCREEN_TS_HEADER_BYTES       128U
#define SCREEN_TS_TAG_BYTES          16U
#define SCREEN_TS_MAX_BODY_BYTES     (4U * 1024U * 1024U)
#define SCREEN_TS_QUEUE_SLOTS        8U
#define SCREEN_TS_CORRECTION_LIMIT_NTP ((8ULL << 32) / 1000ULL)
#define SCREEN_TS_MIN_STEP_NTP         ((17ULL << 32) / 1000ULL)
#define SCREEN_TS_SPRING_DIVISOR       8U
#define SCREEN_TS_LPDDR_BSS \
	__attribute__((section(".lpddr.bss.screen_timestamp_rebase")))

typedef struct screen_ts_pending_s {
	TaskHandle_t task;
	uint64_t source_ntp;
	uint32_t body_bytes;
	uint32_t capture_us;
	uint32_t sequence;
	int socket;
	uint8_t valid;
} screen_ts_pending_t;

typedef struct screen_ts_producer_s {
	TaskHandle_t task;
	const void *source;
	uint64_t source_ntp;
	uint32_t capture_us;
	uint32_t sequence;
	int bytes;
	uint8_t valid;
} screen_ts_producer_t;

typedef struct screen_ts_queue_entry_s {
	const void *pointer;
	uint64_t source_ntp;
	uint32_t capture_us;
	uint32_t sequence;
	int bytes;
	uint8_t valid;
} screen_ts_queue_entry_t;

typedef struct screen_ts_active_s {
	uint64_t source_ntp;
	uint32_t capture_us;
	uint32_t sequence;
	int bytes;
	uint8_t valid;
	uint8_t applied;
} screen_ts_active_t;

typedef struct screen_ts_stats_s {
	uint32_t rx_headers;
	uint32_t rx_invalid;
	uint32_t pending_overwrite;
	uint32_t callback_paired;
	uint32_t callback_missing;
	uint32_t callback_length_mismatch;
	uint32_t queue_pushed;
	uint32_t queue_push_failed;
	uint32_t queue_table_full;
	uint32_t dequeue_paired;
	uint32_t dequeue_missing;
	uint32_t active_overwrite;
	uint32_t tx_applied;
	uint32_t tx_observed;
	uint32_t tx_fallback;
	uint32_t baseline_sets;
	uint32_t baseline_resets;
	uint32_t source_regressions;
	uint32_t arithmetic_resets;
	uint32_t phase_clamps;
	uint32_t spring_pulls;
	uint32_t monotonic_fallbacks;
	uint32_t min_step_forced;
	uint32_t band_overrides;
	uint32_t correction_positive;
	uint32_t correction_negative;
	uint32_t target_abs_max_us;
	uint32_t correction_abs_max_us;
	uint32_t interval_samples;
	uint64_t source_interval_sum_us;
	uint32_t source_interval_max_us;
	uint64_t tx_interval_sum_us;
	uint32_t tx_interval_max_us;
	uint64_t interval_error_sum_us;
	uint32_t interval_error_max_us;
	uint64_t source_to_tx_sum_us;
	uint32_t source_to_tx_max_us;
} screen_ts_stats_t;

static screen_ts_pending_t screen_ts_pending SCREEN_TS_LPDDR_BSS;
static screen_ts_producer_t screen_ts_producer SCREEN_TS_LPDDR_BSS;
static screen_ts_queue_entry_t screen_ts_queue[SCREEN_TS_QUEUE_SLOTS]
	SCREEN_TS_LPDDR_BSS;
static screen_ts_active_t screen_ts_active SCREEN_TS_LPDDR_BSS;
static screen_ts_stats_t screen_ts_stats SCREEN_TS_LPDDR_BSS;
static TaskHandle_t screen_ts_receiver_task SCREEN_TS_LPDDR_BSS;
static void *screen_ts_vector SCREEN_TS_LPDDR_BSS;
static uint32_t screen_ts_sequence SCREEN_TS_LPDDR_BSS;
static int screen_ts_socket SCREEN_TS_LPDDR_BSS;
static uint8_t screen_ts_socket_valid SCREEN_TS_LPDDR_BSS;

/* Persistent stream anchors.  The outgoing base remains in the customer's
 * local NTP domain while all following deltas come from the iPhone header. */
static uint8_t screen_ts_have_baseline SCREEN_TS_LPDDR_BSS;
static uint64_t screen_ts_source_base SCREEN_TS_LPDDR_BSS;
static uint64_t screen_ts_tx_base SCREEN_TS_LPDDR_BSS;
static uint64_t screen_ts_source_prev SCREEN_TS_LPDDR_BSS;
static uint64_t screen_ts_tx_prev SCREEN_TS_LPDDR_BSS;

static uint32_t screen_ts_load_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t screen_ts_load_le64(const uint8_t *p)
{
	return (uint64_t)screen_ts_load_le32(p) |
		((uint64_t)screen_ts_load_le32(p + 4U) << 32);
}

static void screen_ts_store_le64(uint8_t *p, uint64_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
	p[4] = (uint8_t)(value >> 32);
	p[5] = (uint8_t)(value >> 40);
	p[6] = (uint8_t)(value >> 48);
	p[7] = (uint8_t)(value >> 56);
}

static uint32_t screen_ts_ntp_delta_us(uint64_t newer, uint64_t older)
{
	uint64_t delta = newer - older;
	uint64_t usec = (delta >> 32) * 1000000ULL +
		(((uint64_t)(uint32_t)delta * 1000000ULL) >> 32);

	return usec > UINT32_MAX ? UINT32_MAX : (uint32_t)usec;
}

static int screen_ts_is_receiver_task(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();

	if (screen_ts_receiver_task != NULL) {
		return current == screen_ts_receiver_task;
	}
	if ((current != NULL) &&
	    (strcmp(pcTaskGetName(current), "AirPlayScreenReceiver") == 0)) {
		screen_ts_receiver_task = current;
		return 1;
	}
	return 0;
}

static void screen_ts_clear_queue_locked(void)
{
	memset(screen_ts_queue, 0, sizeof(screen_ts_queue));
	screen_ts_active = (screen_ts_active_t){ 0 };
}

static void screen_ts_reset_baseline_locked(void)
{
	screen_ts_have_baseline = 0U;
	screen_ts_source_base = 0U;
	screen_ts_tx_base = 0U;
	screen_ts_source_prev = 0U;
	screen_ts_tx_prev = 0U;
}

void carbox_screen_timestamp_rx_header(int socket, const void *buffer,
				       size_t requested, int result)
{
	const uint8_t *header = (const uint8_t *)buffer;
	uint32_t body_bytes;
	uint8_t frame_type;
	uint64_t source_ntp;

	if ((requested != SCREEN_TS_HEADER_BYTES) ||
	    (result != (int)SCREEN_TS_HEADER_BYTES) || (header == NULL) ||
	    !screen_ts_is_receiver_task()) {
		return;
	}
	body_bytes = screen_ts_load_le32(header);
	frame_type = header[4];
	source_ntp = screen_ts_load_le64(header + 8U);
	if ((body_bytes < SCREEN_TS_TAG_BYTES) ||
	    (body_bytes > SCREEN_TS_MAX_BODY_BYTES) || (source_ntp == 0U) ||
	    !((frame_type <= 2U) || (frame_type == 4U) ||
	      (frame_type == 5U))) {
		taskENTER_CRITICAL();
		screen_ts_stats.rx_invalid++;
		taskEXIT_CRITICAL();
		return;
	}

	taskENTER_CRITICAL();
	if (screen_ts_pending.valid) {
		screen_ts_stats.pending_overwrite++;
	}
	screen_ts_pending = (screen_ts_pending_t){
		.task = xTaskGetCurrentTaskHandle(),
		.source_ntp = source_ntp,
		.body_bytes = body_bytes,
		.capture_us = hal_read_curtime_us(),
		.sequence = ++screen_ts_sequence,
		.socket = socket,
		.valid = 1U
	};
	screen_ts_socket = socket;
	screen_ts_socket_valid = 1U;
	screen_ts_stats.rx_headers++;
	taskEXIT_CRITICAL();
}

void carbox_screen_timestamp_send_begin(const void *data, int bytes)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t plain_bytes = bytes > 0 ? (uint32_t)bytes : 0U;
	int length_matches;

	taskENTER_CRITICAL();
	if (screen_ts_producer.valid) {
		screen_ts_stats.active_overwrite++;
	}
	screen_ts_producer = (screen_ts_producer_t){ 0 };
	length_matches = screen_ts_pending.valid &&
		((screen_ts_pending.body_bytes == plain_bytes) ||
		 (screen_ts_pending.body_bytes == plain_bytes +
			SCREEN_TS_TAG_BYTES));
	if (screen_ts_pending.valid &&
	    (screen_ts_pending.task == current) && (data != NULL) &&
	    (bytes > 0) && length_matches) {
		screen_ts_producer = (screen_ts_producer_t){
			.task = current,
			.source = data,
			.source_ntp = screen_ts_pending.source_ntp,
			.capture_us = screen_ts_pending.capture_us,
			.sequence = screen_ts_pending.sequence,
			.bytes = bytes,
			.valid = 1U
		};
		screen_ts_stats.callback_paired++;
	} else {
		if (screen_ts_pending.valid && !length_matches) {
			screen_ts_stats.callback_length_mismatch++;
		} else {
			screen_ts_stats.callback_missing++;
		}
	}
	screen_ts_pending = (screen_ts_pending_t){ 0 };
	taskEXIT_CRITICAL();
}

void carbox_screen_timestamp_send_end(void)
{
	taskENTER_CRITICAL();
	screen_ts_producer = (screen_ts_producer_t){ 0 };
	taskEXIT_CRITICAL();
}

void carbox_screen_timestamp_queue_push(void *vector, const void *pointer,
					int bytes, int pushed)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t i;

	taskENTER_CRITICAL();
	if (!screen_ts_producer.valid ||
	    (screen_ts_producer.task != current)) {
		taskEXIT_CRITICAL();
		return;
	}
	if (!pushed || (vector == NULL) || (pointer == NULL) ||
	    (bytes != screen_ts_producer.bytes)) {
		screen_ts_stats.queue_push_failed++;
		taskEXIT_CRITICAL();
		return;
	}
	if ((screen_ts_vector != NULL) && (screen_ts_vector != vector)) {
		screen_ts_clear_queue_locked();
		screen_ts_reset_baseline_locked();
		screen_ts_stats.baseline_resets++;
	}
	screen_ts_vector = vector;
	for (i = 0U; i < SCREEN_TS_QUEUE_SLOTS; i++) {
		if (!screen_ts_queue[i].valid ||
		    (screen_ts_queue[i].pointer == pointer)) {
			screen_ts_queue[i] = (screen_ts_queue_entry_t){
				.pointer = pointer,
				.source_ntp = screen_ts_producer.source_ntp,
				.capture_us = screen_ts_producer.capture_us,
				.sequence = screen_ts_producer.sequence,
				.bytes = bytes,
				.valid = 1U
			};
			screen_ts_stats.queue_pushed++;
			taskEXIT_CRITICAL();
			return;
		}
	}
	screen_ts_stats.queue_table_full++;
	taskEXIT_CRITICAL();
}

void carbox_screen_timestamp_queue_erase(void *vector, int index,
					 const void *pointer, int bytes)
{
	uint32_t i;

	if ((index != 0) || (vector == NULL) || (pointer == NULL)) {
		return;
	}
	taskENTER_CRITICAL();
	if (vector != screen_ts_vector) {
		taskEXIT_CRITICAL();
		return;
	}
	if (screen_ts_active.valid && !screen_ts_active.applied) {
		screen_ts_stats.active_overwrite++;
	}
	screen_ts_active = (screen_ts_active_t){ 0 };
	for (i = 0U; i < SCREEN_TS_QUEUE_SLOTS; i++) {
		if (screen_ts_queue[i].valid &&
		    (screen_ts_queue[i].pointer == pointer) &&
		    (screen_ts_queue[i].bytes == bytes)) {
			screen_ts_active = (screen_ts_active_t){
				.source_ntp = screen_ts_queue[i].source_ntp,
				.capture_us = screen_ts_queue[i].capture_us,
				.sequence = screen_ts_queue[i].sequence,
				.bytes = bytes,
				.valid = 1U
			};
			screen_ts_queue[i] = (screen_ts_queue_entry_t){ 0 };
			screen_ts_stats.dequeue_paired++;
			taskEXIT_CRITICAL();
			return;
		}
	}
	screen_ts_stats.dequeue_missing++;
	taskEXIT_CRITICAL();
}

void carbox_screen_timestamp_queue_delete(void *vector)
{
	taskENTER_CRITICAL();
	if ((vector != NULL) && (vector == screen_ts_vector)) {
		screen_ts_vector = NULL;
		screen_ts_clear_queue_locked();
		screen_ts_reset_baseline_locked();
		screen_ts_stats.baseline_resets++;
	}
	taskEXIT_CRITICAL();
}

void carbox_screen_timestamp_close(int socket)
{
	taskENTER_CRITICAL();
	if (screen_ts_socket_valid && (socket == screen_ts_socket)) {
		screen_ts_pending = (screen_ts_pending_t){ 0 };
		screen_ts_producer = (screen_ts_producer_t){ 0 };
		screen_ts_vector = NULL;
		screen_ts_clear_queue_locked();
		screen_ts_reset_baseline_locked();
		screen_ts_receiver_task = NULL;
		screen_ts_socket = 0;
		screen_ts_socket_valid = 0U;
		screen_ts_stats.baseline_resets++;
	}
	taskEXIT_CRITICAL();
}

int carbox_screen_timestamp_patch_normal_header(void *header, size_t length)
{
	uint8_t *bytes = (uint8_t *)header;
	uint64_t local_ntp;
	uint64_t source_ntp;
	uint64_t rebased_ntp;
	uint64_t wire_ntp;
	uint64_t source_offset;
	uint64_t phase_abs_ntp = 0U;
	uint64_t correction_abs_ntp = 0U;
	uint64_t minimum_ntp;
	int64_t output_correction = 0;
	uint32_t source_delta_us = 0U;
	uint32_t tx_delta_us = 0U;
	uint32_t error_us = 0U;
	uint32_t age_us;
	int reset = 0;
	int had_baseline;

	if ((bytes == NULL) || (length != SCREEN_TS_HEADER_BYTES)) {
		return 0;
	}
	local_ntp = screen_ts_load_le64(bytes + 8U);
	taskENTER_CRITICAL();
	if (!screen_ts_active.valid || screen_ts_active.applied ||
	    (screen_ts_active.source_ntp == 0U) || (local_ntp == 0U)) {
		screen_ts_stats.tx_fallback++;
		taskEXIT_CRITICAL();
		return 0;
	}
	source_ntp = screen_ts_active.source_ntp;
	had_baseline = screen_ts_have_baseline != 0U;
	if (!screen_ts_have_baseline) {
		reset = 1;
	} else if (source_ntp < screen_ts_source_prev) {
		screen_ts_stats.source_regressions++;
		reset = 1;
	} else {
		source_offset = source_ntp - screen_ts_source_base;
		if (source_offset > UINT64_MAX - screen_ts_tx_base) {
			screen_ts_stats.arithmetic_resets++;
			reset = 1;
		}
	}
	if (reset) {
		screen_ts_source_base = source_ntp;
		screen_ts_tx_base = local_ntp;
		screen_ts_source_prev = source_ntp;
		screen_ts_tx_prev = local_ntp;
		screen_ts_have_baseline = 1U;
		rebased_ntp = local_ntp;
		wire_ntp = local_ntp;
		screen_ts_stats.baseline_sets++;
		if (had_baseline) {
			screen_ts_stats.baseline_resets++;
		}
	} else {
		rebased_ntp = screen_ts_tx_base +
			(source_ntp - screen_ts_source_base);
		if (CONFIG_SCREEN_TIMESTAMP_REBASE_APPLY &&
		    !CONFIG_SCREEN_TIMESTAMP_REBASE_BOUNDED &&
		    (rebased_ntp < screen_ts_tx_prev)) {
			screen_ts_stats.arithmetic_resets++;
			screen_ts_source_base = source_ntp;
			screen_ts_tx_base = local_ntp;
			rebased_ntp = local_ntp;
			screen_ts_stats.baseline_resets++;
		}
		if (CONFIG_SCREEN_TIMESTAMP_REBASE_APPLY &&
		    CONFIG_SCREEN_TIMESTAMP_REBASE_BOUNDED) {
			if (rebased_ntp >= local_ntp) {
				phase_abs_ntp = rebased_ntp - local_ntp;
				output_correction = phase_abs_ntp >
					SCREEN_TS_CORRECTION_LIMIT_NTP ?
					(int64_t)SCREEN_TS_CORRECTION_LIMIT_NTP :
					(int64_t)phase_abs_ntp;
			} else {
				phase_abs_ntp = local_ntp - rebased_ntp;
				output_correction = -(int64_t)(phase_abs_ntp >
					SCREEN_TS_CORRECTION_LIMIT_NTP ?
					SCREEN_TS_CORRECTION_LIMIT_NTP : phase_abs_ntp);
			}
			if (phase_abs_ntp > SCREEN_TS_CORRECTION_LIMIT_NTP) {
				uint64_t excess = phase_abs_ntp -
					SCREEN_TS_CORRECTION_LIMIT_NTP;
				uint64_t pull = excess / SCREEN_TS_SPRING_DIVISOR;

				if (pull == 0U) {
					pull = 1U;
				}
				screen_ts_stats.phase_clamps++;
				screen_ts_stats.spring_pulls++;
				/* Pull only the excess beyond the safe local-clock band.
				 * Adjusting the anchor makes all following theoretical ticks
				 * converge smoothly instead of remaining pinned at +/-16 ms. */
				if (rebased_ntp > local_ntp) {
					screen_ts_tx_base -= pull;
				} else {
					screen_ts_tx_base += pull;
				}
			}
			wire_ntp = output_correction >= 0 ?
				local_ntp + (uint64_t)output_correction :
				local_ntp - (uint64_t)(-output_correction);
			minimum_ntp = screen_ts_tx_prev + SCREEN_TS_MIN_STEP_NTP;
			if (wire_ntp < minimum_ntp) {
				wire_ntp = minimum_ntp;
				screen_ts_stats.min_step_forced++;
				if ((wire_ntp > local_ntp) &&
				    (wire_ntp - local_ntp >
				     SCREEN_TS_CORRECTION_LIMIT_NTP)) {
					screen_ts_stats.band_overrides++;
				}
				output_correction = wire_ntp >= local_ntp ?
					(int64_t)(wire_ntp - local_ntp) :
					-(int64_t)(local_ntp - wire_ntp);
			}
			correction_abs_ntp = output_correction >= 0 ?
				(uint64_t)output_correction :
				(uint64_t)(-output_correction);
			if (output_correction > 0) {
				screen_ts_stats.correction_positive++;
			} else if (output_correction < 0) {
				screen_ts_stats.correction_negative++;
			}
			{
				uint32_t target_us = screen_ts_ntp_delta_us(
					phase_abs_ntp, 0U);
				uint32_t correction_us = screen_ts_ntp_delta_us(
					correction_abs_ntp, 0U);
				if (target_us > screen_ts_stats.target_abs_max_us) {
					screen_ts_stats.target_abs_max_us = target_us;
				}
				if (correction_us >
				    screen_ts_stats.correction_abs_max_us) {
					screen_ts_stats.correction_abs_max_us = correction_us;
				}
			}
		} else {
			wire_ntp = CONFIG_SCREEN_TIMESTAMP_REBASE_APPLY ?
				rebased_ntp : local_ntp;
		}
		source_delta_us = screen_ts_ntp_delta_us(source_ntp,
			screen_ts_source_prev);
		tx_delta_us = screen_ts_ntp_delta_us(wire_ntp,
			screen_ts_tx_prev);
		error_us = source_delta_us >= tx_delta_us ?
			source_delta_us - tx_delta_us :
			tx_delta_us - source_delta_us;
		screen_ts_stats.interval_samples++;
		screen_ts_stats.source_interval_sum_us += source_delta_us;
		screen_ts_stats.tx_interval_sum_us += tx_delta_us;
		screen_ts_stats.interval_error_sum_us += error_us;
		if (source_delta_us > screen_ts_stats.source_interval_max_us) {
			screen_ts_stats.source_interval_max_us = source_delta_us;
		}
		if (tx_delta_us > screen_ts_stats.tx_interval_max_us) {
			screen_ts_stats.tx_interval_max_us = tx_delta_us;
		}
		if (error_us > screen_ts_stats.interval_error_max_us) {
			screen_ts_stats.interval_error_max_us = error_us;
		}
		screen_ts_source_prev = source_ntp;
		screen_ts_tx_prev = wire_ntp;
	}
	age_us = hal_read_curtime_us() - screen_ts_active.capture_us;
	screen_ts_stats.source_to_tx_sum_us += age_us;
	if (age_us > screen_ts_stats.source_to_tx_max_us) {
		screen_ts_stats.source_to_tx_max_us = age_us;
	}
	screen_ts_active.applied = 1U;
	if (CONFIG_SCREEN_TIMESTAMP_REBASE_APPLY) {
		screen_ts_stats.tx_applied++;
	} else {
		screen_ts_stats.tx_observed++;
	}
	taskEXIT_CRITICAL();

	/* The 128-byte header is ChaCha/AES AAD.  Apply mode stores only after the
	 * closed sender copied the header and before crypto begins.  Observe mode
	 * intentionally leaves every customer byte unchanged. */
	if (CONFIG_SCREEN_TIMESTAMP_REBASE_APPLY) {
		screen_ts_store_le64(bytes + 8U, wire_ntp);
	}
	return 1;
}

void carbox_screen_timestamp_report(uint32_t sequence)
{
#if CONFIG_SCREEN_TIMESTAMP_REBASE_PROFILE
	screen_ts_stats_t stats;
	uint32_t queued = 0U;
	uint32_t i;
	uint32_t pending;
	uint32_t active;

	taskENTER_CRITICAL();
	stats = screen_ts_stats;
	screen_ts_stats = (screen_ts_stats_t){ 0 };
	for (i = 0U; i < SCREEN_TS_QUEUE_SLOTS; i++) {
		queued += screen_ts_queue[i].valid != 0U;
	}
	pending = screen_ts_pending.valid;
	active = screen_ts_active.valid && !screen_ts_active.applied;
	taskEXIT_CRITICAL();
	if ((stats.rx_headers == 0U) && (stats.tx_applied == 0U) &&
	    (stats.tx_observed == 0U) &&
	    (stats.tx_fallback == 0U)) {
		return;
	}
	rt_printf("[SCREENTS][%lu] mode=%s "
		  "rx valid/invalid/overwrite=%lu/%lu/%lu "
		  "callback pair/miss/len=%lu/%lu/%lu "
		  "queue push/fail/full deq/miss/active_ovr=%lu/%lu/%lu/%lu/%lu/%lu "
		  "tx apply/observe/fallback baseline/reset/regress/arith=%lu/%lu/%lu/%lu/%lu/%lu/%lu "
		  "live pending/queued/active=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  CONFIG_SCREEN_TIMESTAMP_REBASE_APPLY ?
			(CONFIG_SCREEN_TIMESTAMP_REBASE_BOUNDED ?
			 "bounded-local-spring" : "rx-rebased-apply") :
			"observe-original-tx",
		  (unsigned long)stats.rx_headers,
		  (unsigned long)stats.rx_invalid,
		  (unsigned long)stats.pending_overwrite,
		  (unsigned long)stats.callback_paired,
		  (unsigned long)stats.callback_missing,
		  (unsigned long)stats.callback_length_mismatch,
		  (unsigned long)stats.queue_pushed,
		  (unsigned long)stats.queue_push_failed,
		  (unsigned long)stats.queue_table_full,
		  (unsigned long)stats.dequeue_paired,
		  (unsigned long)stats.dequeue_missing,
		  (unsigned long)stats.active_overwrite,
		  (unsigned long)stats.tx_applied,
		  (unsigned long)stats.tx_observed,
		  (unsigned long)stats.tx_fallback,
		  (unsigned long)stats.baseline_sets,
		  (unsigned long)stats.baseline_resets,
		  (unsigned long)stats.source_regressions,
		  (unsigned long)stats.arithmetic_resets,
		  (unsigned long)pending, (unsigned long)queued,
		  (unsigned long)active);
	rt_printf("[SCREENTS][%lu] interval samples=%lu "
		  "rx_us avg/max=%llu/%lu tx_us avg/max=%llu/%lu "
		  "error_us avg/max=%llu/%lu source_to_tx_us avg/max=%llu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.interval_samples,
		  (unsigned long long)(stats.interval_samples != 0U ?
			stats.source_interval_sum_us / stats.interval_samples : 0U),
		  (unsigned long)stats.source_interval_max_us,
		  (unsigned long long)(stats.interval_samples != 0U ?
			stats.tx_interval_sum_us / stats.interval_samples : 0U),
		  (unsigned long)stats.tx_interval_max_us,
		  (unsigned long long)(stats.interval_samples != 0U ?
			stats.interval_error_sum_us / stats.interval_samples : 0U),
		  (unsigned long)stats.interval_error_max_us,
		  (unsigned long long)((stats.tx_applied + stats.tx_observed) != 0U ?
			stats.source_to_tx_sum_us /
			(stats.tx_applied + stats.tx_observed) : 0U),
		  (unsigned long)stats.source_to_tx_max_us);
	if (CONFIG_SCREEN_TIMESTAMP_REBASE_APPLY &&
	    CONFIG_SCREEN_TIMESTAMP_REBASE_BOUNDED) {
		rt_printf("[SCREENTSSPRING][%lu] band_ms=+-%lu min_step_ms=%lu "
			  "spring_excess=1/%lu clamp/spring_pull="
			  "%lu/%lu min_step_forced/band_override=%lu/%lu "
			  "correction positive/negative=%lu/%lu "
			  "target_abs_max/correction_abs_max_us=%lu/%lu\r\n",
			  (unsigned long)sequence, 8UL, 17UL,
			  (unsigned long)SCREEN_TS_SPRING_DIVISOR,
			  (unsigned long)stats.phase_clamps,
			  (unsigned long)stats.spring_pulls,
			  (unsigned long)stats.min_step_forced,
			  (unsigned long)stats.band_overrides,
			  (unsigned long)stats.correction_positive,
			  (unsigned long)stats.correction_negative,
			  (unsigned long)stats.target_abs_max_us,
			  (unsigned long)stats.correction_abs_max_us);
	}
#else
	(void)sequence;
#endif
}

#else

void carbox_screen_timestamp_rx_header(int socket, const void *buffer,
				       size_t requested, int result)
{ (void)socket; (void)buffer; (void)requested; (void)result; }
void carbox_screen_timestamp_send_begin(const void *data, int bytes)
{ (void)data; (void)bytes; }
void carbox_screen_timestamp_send_end(void) { }
void carbox_screen_timestamp_queue_push(void *vector, const void *pointer,
					int bytes, int pushed)
{ (void)vector; (void)pointer; (void)bytes; (void)pushed; }
void carbox_screen_timestamp_queue_erase(void *vector, int index,
					 const void *pointer, int bytes)
{ (void)vector; (void)index; (void)pointer; (void)bytes; }
void carbox_screen_timestamp_queue_delete(void *vector) { (void)vector; }
void carbox_screen_timestamp_close(int socket) { (void)socket; }
int carbox_screen_timestamp_patch_normal_header(void *header, size_t length)
{ (void)header; (void)length; return 0; }
void carbox_screen_timestamp_report(uint32_t sequence) { (void)sequence; }

#endif
