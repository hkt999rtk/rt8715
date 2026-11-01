/* Low-rate diagnostics and adaptive retention for the customer RTP buffer.
 * AirPlayUtils.o layout and call relocations are audited by the companion
 * archive patch. No UART output is made from the packet or PCM hot paths. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"

#define JITTER_SLOTS 8U
#define JITTER_BASE_MARGIN_MS 20U
#define JITTER_HIGH_MARGIN_MS 200U
#define JITTER_ACTIVE_READ_US 100000U
#define JITTER_PAUSE_GAP_US 250000U
#define JITTER_QUIET_HOLD_US 5000000U
#define JITTER_DECAY_MS 10U
#define JITTER_EXCESS_TRIGGER_US 8000U
#define JITTER_ELD_FRAME_SAMPLES 480U
#define JITTER_TRIM_GAIN_DIV 32U
#define JITTER_TRIM_MAX_DIV 500U /* At most 2 ms of PCM per read. */

typedef struct {
	void *jitter;
	TaskHandle_t reader;
	uint32_t last_activity_us;
	uint32_t last_packet_us;
	uint32_t last_packet_ts;
	uint32_t last_read_us;
	uint32_t last_disturbance_us;
	uint32_t last_trim_zero_us;
	uint32_t sample_rate;
	uint32_t frame_samples;
	uint32_t frame_duration_us;
	uint32_t packet_count;
	uint32_t packet_gap_max_us;
	uint32_t excess_peak_us;
	uint32_t excess_ewma_us;
	uint32_t rtp_jumps;
	uint32_t zero_pcm_bytes;
	uint32_t zero_pcm_ms;
	uint32_t drop_nodes;
	uint32_t drop_events;
	uint32_t drop_before_ms;
	uint32_t drop_after_ms;
	uint32_t max_busy_nodes;
	uint32_t reads;
	uint32_t output_samples_min;
	uint32_t output_samples_max;
	uint32_t trim_calls;
	uint32_t trim_samples;
	uint32_t trim_max_samples;
	uint32_t trim_zero_bytes;
	uint32_t emergency_drops;
	uint16_t margin_ms;
	uint16_t target_ms;
	uint16_t target_nodes;
	uint8_t adaptive_enabled;
	uint8_t skip_phase;
} jitter_slot_t;

static jitter_slot_t jitter_slots[JITTER_SLOTS];
static uint8_t jitter_report_task_started;

void carbox_jitter_diag_report(uint32_t now_us);

static void jitter_report_task(void *argument)
{
	(void)argument;
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(1000U));
		carbox_jitter_diag_report(hal_read_curtime_us());
	}
}

static uint32_t jitter_field(const void *context, unsigned offset)
{
	uint32_t value;
	memcpy(&value, (const uint8_t *)context + offset, sizeof(value));
	return value;
}

static void jitter_set_field(void *context, unsigned offset, uint32_t value)
{
	memcpy((uint8_t *)context + offset, &value, sizeof(value));
}

static uint16_t jitter_clamp_margin(uint32_t value)
{
	if (value < JITTER_BASE_MARGIN_MS) return JITTER_BASE_MARGIN_MS;
	if (value > JITTER_HIGH_MARGIN_MS) return JITTER_HIGH_MARGIN_MS;
	return (uint16_t)value;
}

static void jitter_raise_locked(jitter_slot_t *slot, uint32_t target,
	uint32_t now_us)
{
	uint16_t bounded = jitter_clamp_margin(target);
	if (bounded > slot->target_ms) slot->target_ms = bounded;
	if (bounded > slot->margin_ms) slot->margin_ms = bounded;
	slot->last_disturbance_us = now_us;
}

static void jitter_set_format_locked(jitter_slot_t *slot, void *jitter)
{
	slot->sample_rate = jitter_field(jitter, 0x150);
	slot->frame_samples = jitter_field(jitter, 0x160);
	slot->adaptive_enabled = (slot->frame_samples == JITTER_ELD_FRAME_SAMPLES);
	if (slot->sample_rate && slot->frame_samples <= UINT32_MAX / 1000000U)
		slot->frame_duration_us =
			slot->frame_samples * 1000000U / slot->sample_rate;
}

static jitter_slot_t *jitter_slot_locked(void *jitter, uint32_t now_us)
{
	jitter_slot_t *oldest = &jitter_slots[0];
	unsigned i;
	for (i = 0; i < JITTER_SLOTS; ++i) {
		if (jitter_slots[i].jitter == jitter) return &jitter_slots[i];
		if (jitter_slots[i].jitter == NULL) return &jitter_slots[i];
		if ((uint32_t)(now_us - jitter_slots[i].last_activity_us) >
		    (uint32_t)(now_us - oldest->last_activity_us)) oldest = &jitter_slots[i];
	}
	return oldest;
}

static jitter_slot_t *jitter_get_locked(void *jitter, uint32_t now_us)
{
	jitter_slot_t *slot = jitter_slot_locked(jitter, now_us);
	if (slot->jitter != jitter) {
		memset(slot, 0, sizeof(*slot));
		slot->jitter = jitter;
		slot->margin_ms = JITTER_BASE_MARGIN_MS;
		slot->target_ms = JITTER_BASE_MARGIN_MS;
		jitter_set_format_locked(slot, jitter);
	}
	slot->last_activity_us = now_us;
	return slot;
}

extern int __real_RTPJitterBufferInit(void *, const void *, const void *, uint32_t);
int __wrap_RTPJitterBufferInit(void *jitter, const void *in_format,
	const void *out_format, uint32_t latency_ms)
{
	int result = __real_RTPJitterBufferInit(jitter, in_format, out_format, latency_ms);
	if (result == 0 && jitter != NULL) {
		uint32_t now_us = hal_read_curtime_us();
		jitter_slot_t *slot;
		uint8_t start_report_task = 0U;
		taskENTER_CRITICAL();
		slot = jitter_slot_locked(jitter, now_us);
		memset(slot, 0, sizeof(*slot));
		slot->jitter = jitter;
		slot->margin_ms = JITTER_BASE_MARGIN_MS;
		slot->target_ms = JITTER_BASE_MARGIN_MS;
		jitter_set_format_locked(slot, jitter);
		slot->last_activity_us = now_us;
		if (!jitter_report_task_started) {
			jitter_report_task_started = 1U;
			start_report_task = 1U;
		}
		taskEXIT_CRITICAL();
		if (start_report_task && xTaskCreate(jitter_report_task, "audio_jbuf",
			1024U, NULL, 1U, NULL) != pdPASS) {
			taskENTER_CRITICAL();
			jitter_report_task_started = 0U;
			taskEXIT_CRITICAL();
			printf("[AUDIO_JBUF] reporter task creation failed\n");
		}
		printf("[AUDIO_JBUF_INIT] ctx=%p latency_ms=%lu nodes=%lu rate=%lu "
			"frame_samples=%lu adaptive=%u margin_ms=%u\n",
			jitter, (unsigned long)jitter_field(jitter, 0x1b4),
			(unsigned long)jitter_field(jitter, 0x148),
			(unsigned long)jitter_field(jitter, 0x150),
			(unsigned long)jitter_field(jitter, 0x160),
			(unsigned)(jitter_field(jitter, 0x160) == JITTER_ELD_FRAME_SAMPLES),
			JITTER_BASE_MARGIN_MS);
	}
	return result;
}

void carbox_jitter_note_packet(void *jitter, uint32_t rtp_timestamp,
	uint32_t arrival_us)
{
	uint32_t now_us = arrival_us;
	jitter_slot_t *slot;
	if (jitter == NULL) return;
	taskENTER_CRITICAL();
	slot = jitter_get_locked(jitter, now_us);
	if (slot->packet_count != 0U) {
		uint32_t gap = now_us - slot->last_packet_us;
		if (gap > slot->packet_gap_max_us) slot->packet_gap_max_us = gap;
		if (slot->adaptive_enabled) {
			uint32_t delta_ts = rtp_timestamp - slot->last_packet_ts;
			uint32_t frame_samples = slot->frame_samples;
			uint32_t rate = slot->sample_rate;
			if (rate && frame_samples && delta_ts > 0U &&
			    delta_ts <= 10U * frame_samples &&
			    (gap <= JITTER_PAUSE_GAP_US ||
			     (slot->last_read_us &&
			      now_us - slot->last_read_us <= JITTER_ACTIVE_READ_US))) {
				uint32_t expected_us =
					(delta_ts / frame_samples) * slot->frame_duration_us +
					((delta_ts % frame_samples) * 1000000U) / rate;
				uint32_t excess_us = gap > expected_us ? gap - expected_us : 0U;
				if (excess_us > slot->excess_peak_us)
					slot->excess_peak_us = excess_us;
				/* An eight-packet EWMA prevents one delayed packet from
				 * defining the steady-state target. */
				slot->excess_ewma_us =
					(slot->excess_ewma_us * 7U + excess_us) / 8U;
				if (excess_us >= JITTER_EXCESS_TRIGGER_US)
					jitter_raise_locked(slot,
						JITTER_BASE_MARGIN_MS + (excess_us + 999U) / 1000U,
						now_us);
			} else {
				slot->rtp_jumps++;
			}
		}
	}
	slot->last_packet_us = now_us;
	slot->last_packet_ts = rtp_timestamp;
	slot->packet_count++;
	taskEXIT_CRITICAL();
}

extern int __real_RTPJitterBufferRead(void *, void *, uint32_t);
int __wrap_RTPJitterBufferRead(void *jitter, void *output, uint32_t bytes)
{
	uint32_t now_us = hal_read_curtime_us();
	jitter_slot_t *slot;
	uint32_t busy, rate, frame_bytes, frame_samples, sample_bytes, latency;
	uint32_t target_samples, target_nodes, output_samples, extra_samples = 0U;
	uint32_t trim_zero_before = 0U;
	int result;
	if (jitter == NULL) return __real_RTPJitterBufferRead(jitter, output, bytes);
	taskENTER_CRITICAL();
	slot = jitter_get_locked(jitter, now_us);
	slot->reader = xTaskGetCurrentTaskHandle();
	slot->last_read_us = now_us;
	slot->reads++;
	/* Busy-node count is an aligned atomic word. Do not walk either native
	 * linked list without its semaphore. Trim in PCM samples. */
	busy = jitter_field(jitter, 0x14c);
	if (busy > slot->max_busy_nodes) slot->max_busy_nodes = busy;
	rate = jitter_field(jitter, 0x150);
	frame_samples = jitter_field(jitter, 0x160);
	sample_bytes = jitter_field(jitter, 0x188);
	latency = jitter_field(jitter, 0x1b4);
	frame_bytes = frame_samples * sample_bytes;
	if (sample_bytes) {
		uint32_t requested_samples = bytes / sample_bytes;
		if (slot->output_samples_min == 0U ||
		    requested_samples < slot->output_samples_min)
			slot->output_samples_min = requested_samples;
		if (requested_samples > slot->output_samples_max)
			slot->output_samples_max = requested_samples;
	}
	if (slot->adaptive_enabled && rate && frame_samples && sample_bytes &&
	    frame_bytes && bytes >= sample_bytes &&
	    jitter_field(jitter, 0x138) != 0U &&
	    (slot->last_trim_zero_us == 0U ||
	     (uint32_t)(now_us - slot->last_trim_zero_us) > 500000U)) {
		uint32_t target_ms = latency + slot->margin_ms;
		if (target_ms < 200U) target_ms = 200U;
		target_samples = (uint32_t)
			(((uint64_t)target_ms * rate + 999U) / 1000U);
		target_nodes = (target_samples + frame_samples - 1U) / frame_samples;
		if (target_nodes > UINT16_MAX) target_nodes = UINT16_MAX;
		slot->target_nodes = (uint16_t)target_nodes;
		output_samples = bytes / sample_bytes;
		/* The busy-node count gives a conservative capacity estimate. Keep
		 * the target plus two nodes and the next output call untouched. */
		if (busy > target_nodes + 2U &&
		    busy * frame_samples > target_samples +
		    2U * frame_samples + output_samples) {
			uint32_t excess_samples =
				(busy - target_nodes - 2U) * frame_samples;
			extra_samples = excess_samples / JITTER_TRIM_GAIN_DIV;
			if (extra_samples > rate / JITTER_TRIM_MAX_DIV)
				extra_samples = rate / JITTER_TRIM_MAX_DIV;
			if (extra_samples > bytes / sample_bytes)
				extra_samples = bytes / sample_bytes;
			trim_zero_before = slot->trim_zero_bytes;
		}
	}
	taskEXIT_CRITICAL();
	if (extra_samples) {
		taskENTER_CRITICAL();
		slot->skip_phase = 1U;
		taskEXIT_CRITICAL();
		/* The following normal read overwrites these discarded samples. */
		result = __real_RTPJitterBufferRead(jitter, output,
			extra_samples * sample_bytes);
		taskENTER_CRITICAL();
		slot->skip_phase = 0U;
		if (result == 0 && slot->trim_zero_bytes == trim_zero_before) {
			slot->trim_calls++;
			slot->trim_samples += extra_samples;
			if (extra_samples > slot->trim_max_samples)
				slot->trim_max_samples = extra_samples;
		}
		taskEXIT_CRITICAL();
		if (result != 0) {
			taskENTER_CRITICAL();
			if (slot->reader == xTaskGetCurrentTaskHandle()) slot->reader = NULL;
			taskEXIT_CRITICAL();
			return result;
		}
	}
	result = __real_RTPJitterBufferRead(jitter, output, bytes);
	taskENTER_CRITICAL();
	if (slot->reader == xTaskGetCurrentTaskHandle()) slot->reader = NULL;
	taskEXIT_CRITICAL();
	return result;
}

extern void __real_RTPJitterBufferFree(void *);
void __wrap_RTPJitterBufferFree(void *jitter)
{
	unsigned i;
	taskENTER_CRITICAL();
	for (i = 0; i < JITTER_SLOTS; ++i)
		if (jitter_slots[i].jitter == jitter)
			memset(&jitter_slots[i], 0, sizeof(jitter_slots[i]));
	taskEXIT_CRITICAL();
	__real_RTPJitterBufferFree(jitter);
}

/* These four memset calls in RTPJitterBufferRead fill missing PCM with zero. */
void *carbox_jitter_zero_pcm(void *destination, int value, size_t bytes)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	uint32_t now_us = hal_read_curtime_us();
	unsigned i;
	taskENTER_CRITICAL();
	for (i = 0; i < JITTER_SLOTS; ++i) {
		jitter_slot_t *slot = &jitter_slots[i];
		if (slot->jitter == NULL || slot->reader != task) continue;
		if (slot->skip_phase) {
			slot->trim_zero_bytes += (uint32_t)bytes;
			slot->last_trim_zero_us = now_us;
			break;
		}
		slot->zero_pcm_bytes += (uint32_t)bytes;
		slot->last_activity_us = now_us;
		if (slot->adaptive_enabled && slot->packet_count > 50U) {
			uint32_t rate = jitter_field(slot->jitter, 0x150);
			uint32_t sample_bytes = jitter_field(slot->jitter, 0x188);
			uint32_t zero_ms = rate && sample_bytes ?
				(uint32_t)((uint64_t)bytes * 1000U /
				((uint64_t)rate * sample_bytes)) : 0U;
			slot->zero_pcm_ms += zero_ms;
			if (zero_ms) jitter_raise_locked(slot,
				JITTER_BASE_MARGIN_MS + 2U * zero_ms, now_us);
		}
		break;
	}
	taskEXIT_CRITICAL();
	return memset(destination, value, bytes);
}

extern void _RTPJitterBufferDiscardExcess(void *);
extern uint32_t _RTPJitterBufferBufferedSamples(void *, int);
void carbox_jitter_discard_probe(void *jitter)
{
	uint32_t now_us = hal_read_curtime_us();
	uint32_t latency, rate, before_nodes, before_samples, after_nodes, after_samples;
	uint32_t temporary_latency;
	uint8_t adaptive, from_read;
	jitter_slot_t *slot;
	if (jitter == NULL) return;
	taskENTER_CRITICAL();
	slot = jitter_get_locked(jitter, now_us);
	adaptive = slot->adaptive_enabled;
	from_read = (slot->reader == xTaskGetCurrentTaskHandle());
	taskEXIT_CRITICAL();
	/* Normal ELD retention is controlled by small extra PCM reads. The native
	 * routine can empty hundreds of milliseconds in one call. */
	if (adaptive && from_read) return;
	latency = jitter_field(jitter, 0x1b4);
	rate = jitter_field(jitter, 0x150);
	before_nodes = jitter_field(jitter, 0x14c);
	before_samples = rate ? _RTPJitterBufferBufferedSamples(jitter, 0) : 0U;
	temporary_latency = latency;
	if (adaptive && rate && before_nodes) {
		/* PutBusyNode invokes this only near a full node pool. Select a
		 * threshold halfway through the oldest node so the native routine
		 * returns at most that one node to the free list. */
		uint32_t head = jitter_field(jitter, 0x138);
		uint32_t sample_bytes = jitter_field(jitter, 0x188);
		uint32_t first_samples = 0U;
		if (head && sample_bytes)
			first_samples = jitter_field((void *)(uintptr_t)head, 0x5c8) /
				sample_bytes;
		else {
			head = jitter_field(jitter, 0x140);
			if (head) first_samples = jitter_field(jitter, 0x160);
		}
		if (head && first_samples == 0U)
			first_samples = jitter_field(jitter, 0x160);
		if (first_samples && before_samples > first_samples / 2U) {
			uint32_t threshold_samples =
				before_samples - (first_samples + 1U) / 2U;
			uint32_t threshold_ms = (uint32_t)
				(((uint64_t)threshold_samples * 1000U + rate - 1U) / rate);
			if (threshold_ms < 200U) threshold_ms = 200U;
			temporary_latency = threshold_ms > JITTER_BASE_MARGIN_MS ?
				threshold_ms - JITTER_BASE_MARGIN_MS : 1U;
		} else return;
	}
	if (temporary_latency != latency)
		jitter_set_field(jitter, 0x1b4, temporary_latency);
	_RTPJitterBufferDiscardExcess(jitter);
	if (temporary_latency != latency) jitter_set_field(jitter, 0x1b4, latency);
	after_nodes = jitter_field(jitter, 0x14c);
	if (rate && after_nodes < before_nodes) {
		after_samples = rate ? _RTPJitterBufferBufferedSamples(jitter, 0) : 0U;
		taskENTER_CRITICAL();
		slot->drop_events++;
		slot->drop_nodes += before_nodes - after_nodes;
		if (adaptive) slot->emergency_drops += before_nodes - after_nodes;
		slot->drop_before_ms = (uint32_t)((uint64_t)before_samples * 1000U / rate);
		slot->drop_after_ms = (uint32_t)((uint64_t)after_samples * 1000U / rate);
		taskEXIT_CRITICAL();
	}
	taskENTER_CRITICAL();
	if (before_nodes > slot->max_busy_nodes) slot->max_busy_nodes = before_nodes;
	taskEXIT_CRITICAL();
}

/* Called by the low-priority one-second reporter task. */
void carbox_jitter_diag_report(uint32_t now_us)
{
	unsigned i;
	for (i = 0; i < JITTER_SLOTS; ++i) {
		jitter_slot_t copy;
		uint32_t latency, nodes, busy, rate;
		taskENTER_CRITICAL();
		if (jitter_slots[i].jitter != NULL && jitter_slots[i].adaptive_enabled) {
			jitter_slot_t *slot = &jitter_slots[i];
			uint32_t measured_ms = slot->excess_peak_us / 1000U;
			if ((uint32_t)(now_us - slot->last_packet_us) > 1000000U)
				slot->excess_ewma_us = 0U;
			uint32_t smooth_ms = 2U * slot->excess_ewma_us / 1000U;
			uint16_t target = jitter_clamp_margin(JITTER_BASE_MARGIN_MS +
				(measured_ms > smooth_ms ? measured_ms : smooth_ms));
			slot->target_ms = target;
			if (target > slot->margin_ms) {
				slot->margin_ms = target;
				slot->last_disturbance_us = now_us;
			} else if (slot->margin_ms > JITTER_BASE_MARGIN_MS &&
				   (uint32_t)(now_us - slot->last_disturbance_us) >
				   JITTER_QUIET_HOLD_US) {
				slot->margin_ms = jitter_clamp_margin(
					slot->margin_ms - JITTER_DECAY_MS);
				slot->target_ms = target;
			}
		}
		copy = jitter_slots[i];
		jitter_slots[i].packet_gap_max_us = 0U;
		jitter_slots[i].excess_peak_us = 0U;
		jitter_slots[i].rtp_jumps = 0U;
		jitter_slots[i].zero_pcm_bytes = 0U;
		jitter_slots[i].zero_pcm_ms = 0U;
		jitter_slots[i].drop_nodes = 0U;
		jitter_slots[i].drop_events = 0U;
		jitter_slots[i].trim_calls = 0U;
		jitter_slots[i].trim_samples = 0U;
		jitter_slots[i].trim_max_samples = 0U;
		jitter_slots[i].trim_zero_bytes = 0U;
		jitter_slots[i].emergency_drops = 0U;
		jitter_slots[i].drop_before_ms = 0U;
		jitter_slots[i].drop_after_ms = 0U;
		jitter_slots[i].max_busy_nodes = 0U;
		jitter_slots[i].reads = 0U;
		jitter_slots[i].output_samples_min = 0U;
		jitter_slots[i].output_samples_max = 0U;
		taskEXIT_CRITICAL();
		if (copy.jitter == NULL || (uint32_t)(now_us - copy.last_activity_us) > 5000000U)
			continue;
		taskENTER_CRITICAL();
		if (jitter_slots[i].jitter != copy.jitter) {
			taskEXIT_CRITICAL();
			continue;
		}
		latency = jitter_field(copy.jitter, 0x1b4);
		nodes = jitter_field(copy.jitter, 0x148);
		busy = jitter_field(copy.jitter, 0x14c);
		rate = jitter_field(copy.jitter, 0x150);
		taskEXIT_CRITICAL();
		printf("[AUDIO_JBUF] ctx=%p rate=%lu adaptive=%u latency=%lu margin=%u target=%u limit_ms=%lu "
			"nodes=%lu busy=%lu max_busy=%lu gap_max_ms=%lu excess_peak_ms=%lu "
			"excess_ewma_ms=%lu rtp_jumps=%lu zero_bytes=%lu zero_ms=%lu "
			"target_nodes=%u trim_calls=%lu trim_samples=%lu trim_max=%lu trim_zero_bytes=%lu "
			"emergency_drops=%lu purge_events=%lu purged_nodes=%lu "
			"drop_span_ms=%lu->%lu reads=%lu output_samples=%lu..%lu\n",
			copy.jitter, (unsigned long)rate, (unsigned)copy.adaptive_enabled,
			(unsigned long)latency, copy.margin_ms, copy.target_ms,
			(unsigned long)((latency + copy.margin_ms > 200U) ?
				(latency + copy.margin_ms) : 200U),
			(unsigned long)nodes, (unsigned long)busy,
			(unsigned long)copy.max_busy_nodes,
			(unsigned long)(copy.packet_gap_max_us / 1000U),
			(unsigned long)(copy.excess_peak_us / 1000U),
			(unsigned long)(copy.excess_ewma_us / 1000U),
			(unsigned long)copy.rtp_jumps,
			(unsigned long)copy.zero_pcm_bytes,
			(unsigned long)copy.zero_pcm_ms,
			copy.target_nodes,
			(unsigned long)copy.trim_calls,
			(unsigned long)copy.trim_samples,
			(unsigned long)copy.trim_max_samples,
			(unsigned long)copy.trim_zero_bytes,
			(unsigned long)copy.emergency_drops,
			(unsigned long)copy.drop_events,
			(unsigned long)copy.drop_nodes,
			(unsigned long)copy.drop_before_ms,
			(unsigned long)copy.drop_after_ms,
			(unsigned long)copy.reads,
			(unsigned long)copy.output_samples_min,
			(unsigned long)copy.output_samples_max);
	}
}
