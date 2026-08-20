#include "video_handover_zero_copy.h"
#include "screen_rx_record_profiler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "hal_timer.h"
#include "screen_tx_direct_crypto.h"

#ifndef CONFIG_VIDEO_HANDOVER_ZERO_COPY
#define CONFIG_VIDEO_HANDOVER_ZERO_COPY 0
#endif

#ifndef VIDEO_HANDOVER_ZERO_COPY_MIN_BYTES
#define VIDEO_HANDOVER_ZERO_COPY_MIN_BYTES 4096U
#endif

#ifndef CONFIG_VIDEO_HANDOVER_BACKPRESSURE
#define CONFIG_VIDEO_HANDOVER_BACKPRESSURE 0
#endif

#ifndef VIDEO_HANDOVER_MAX_INFLIGHT
#define VIDEO_HANDOVER_MAX_INFLIGHT 2U
#endif

#ifndef VIDEO_HANDOVER_GATE_POLL_TICKS
#define VIDEO_HANDOVER_GATE_POLL_TICKS 1U
#endif

#ifndef CONFIG_SCREEN_QUEUE_PROFILE
#define CONFIG_SCREEN_QUEUE_PROFILE 0
#endif

#ifndef CONFIG_SCREEN_FPS_PROFILE
#define CONFIG_SCREEN_FPS_PROFILE 0
#endif

#if CONFIG_VIDEO_HANDOVER_ZERO_COPY

#define VIDEO_HANDOVER_SOURCE_SLOTS 64U
#define VIDEO_HANDOVER_ACTIVE_SLOTS  4U
#define VIDEO_HANDOVER_REF_PRODUCER  1U
#define VIDEO_HANDOVER_REF_CONSUMER  2U

#if CONFIG_SCREEN_FPS_PROFILE
#define SCREEN_FPS_QUEUE_TIMES 32U

typedef struct screen_fps_stats_s {
	uint32_t frames;
	uint64_t bytes;
	uint32_t interval_samples;
	uint64_t interval_sum_us;
	uint32_t interval_max_us;
	uint32_t gap_over_20ms;
	uint32_t gap_over_33ms;
	uint32_t gap_over_50ms;
	uint32_t gap_over_100ms;
	uint64_t send_sum_us;
	uint32_t send_max_us;
	uint32_t send_over_16ms;
	uint32_t send_over_33ms;
	uint32_t pushes;
	uint64_t push_sum_us;
	uint32_t push_max_us;
	uint32_t dequeues;
	uint32_t queue_age_samples;
	uint64_t queue_age_sum_us;
	uint32_t queue_age_max_us;
	uint32_t queue_age_over_16ms;
	uint32_t queue_age_over_33ms;
	uint32_t queue_depth_max;
	uint32_t tracking_lost;
} screen_fps_stats_t;

static screen_fps_stats_t screen_fps_stats
	__attribute__((section(".lpddr.bss.screen_fps_stats")));
static TaskHandle_t screen_fps_active_task;
static void *screen_fps_vector;
static uint32_t screen_fps_active_arrival_us;
static uint32_t screen_fps_last_arrival_us;
static uint32_t screen_fps_queue_times[SCREEN_FPS_QUEUE_TIMES]
	__attribute__((section(".lpddr.bss.screen_fps_queue_times")));
static uint32_t screen_fps_queue_head;
static uint32_t screen_fps_queue_count;
static uint8_t screen_fps_have_arrival;

static uint32_t screen_fps_begin(int bytes)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	uint32_t now_us = hal_read_curtime_us();
	uint32_t delta_us = 0U;

	taskENTER_CRITICAL();
	if (screen_fps_have_arrival) {
		delta_us = now_us - screen_fps_last_arrival_us;
		screen_fps_stats.interval_samples++;
		screen_fps_stats.interval_sum_us += delta_us;
		if (delta_us > screen_fps_stats.interval_max_us) {
			screen_fps_stats.interval_max_us = delta_us;
		}
		screen_fps_stats.gap_over_20ms += delta_us > 20000U;
		screen_fps_stats.gap_over_33ms += delta_us > 33333U;
		screen_fps_stats.gap_over_50ms += delta_us > 50000U;
		screen_fps_stats.gap_over_100ms += delta_us > 100000U;
	}
	screen_fps_last_arrival_us = now_us;
	screen_fps_have_arrival = 1U;
	screen_fps_stats.frames++;
	if (bytes > 0) {
		screen_fps_stats.bytes += (uint32_t)bytes;
	}
	screen_fps_active_task = task;
	screen_fps_active_arrival_us = now_us;
	taskEXIT_CRITICAL();
	return now_us;
}

static void screen_fps_end(uint32_t start_us)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	uint32_t elapsed_us = hal_read_curtime_us() - start_us;

	taskENTER_CRITICAL();
	screen_fps_stats.send_sum_us += elapsed_us;
	if (elapsed_us > screen_fps_stats.send_max_us) {
		screen_fps_stats.send_max_us = elapsed_us;
	}
	screen_fps_stats.send_over_16ms += elapsed_us > 16667U;
	screen_fps_stats.send_over_33ms += elapsed_us > 33333U;
	if (screen_fps_active_task == task) {
		screen_fps_active_task = NULL;
		screen_fps_active_arrival_us = 0U;
	}
	taskEXIT_CRITICAL();
}

static int screen_fps_push_begin(void *vector, int element_size,
				 uint32_t *arrival_us)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	int measured = 0;

	taskENTER_CRITICAL();
	if ((screen_fps_active_task == task) &&
	    (element_size == (int)(sizeof(void *) + sizeof(int)))) {
		if (screen_fps_vector == NULL) {
			screen_fps_vector = vector;
		}
		if (screen_fps_vector == vector) {
			*arrival_us = screen_fps_active_arrival_us;
			measured = 1;
		}
	}
	taskEXIT_CRITICAL();
	return measured;
}

static void screen_fps_push_end(uint32_t start_us, uint32_t arrival_us,
				int pushed, uint32_t depth)
{
	uint32_t elapsed_us = hal_read_curtime_us() - start_us;

	taskENTER_CRITICAL();
	if (pushed) {
		screen_fps_stats.pushes++;
		screen_fps_stats.push_sum_us += elapsed_us;
		if (elapsed_us > screen_fps_stats.push_max_us) {
			screen_fps_stats.push_max_us = elapsed_us;
		}
		if (depth > screen_fps_stats.queue_depth_max) {
			screen_fps_stats.queue_depth_max = depth;
		}
		if (screen_fps_queue_count < SCREEN_FPS_QUEUE_TIMES) {
			uint32_t tail = (screen_fps_queue_head +
				screen_fps_queue_count) % SCREEN_FPS_QUEUE_TIMES;

			screen_fps_queue_times[tail] = arrival_us;
			screen_fps_queue_count++;
		} else {
			screen_fps_stats.tracking_lost++;
		}
	}
	taskEXIT_CRITICAL();
}

static int screen_fps_erase_begin(void *vector, int index,
				  uint32_t *arrival_us)
{
	int measured = 0;

	taskENTER_CRITICAL();
	if ((vector == screen_fps_vector) && (index == 0)) {
		measured = 1;
		if (screen_fps_queue_count != 0U) {
			*arrival_us = screen_fps_queue_times[screen_fps_queue_head];
			screen_fps_queue_head = (screen_fps_queue_head + 1U) %
				SCREEN_FPS_QUEUE_TIMES;
			screen_fps_queue_count--;
		} else {
			*arrival_us = 0U;
			screen_fps_stats.tracking_lost++;
		}
	}
	taskEXIT_CRITICAL();
	return measured;
}

static void screen_fps_erase_end(uint32_t arrival_us, uint32_t depth)
{
	uint32_t age_us = arrival_us != 0U ?
		hal_read_curtime_us() - arrival_us : 0U;

	taskENTER_CRITICAL();
	screen_fps_stats.dequeues++;
	if (arrival_us != 0U) {
		screen_fps_stats.queue_age_samples++;
		screen_fps_stats.queue_age_sum_us += age_us;
		if (age_us > screen_fps_stats.queue_age_max_us) {
			screen_fps_stats.queue_age_max_us = age_us;
		}
		screen_fps_stats.queue_age_over_16ms += age_us > 16667U;
		screen_fps_stats.queue_age_over_33ms += age_us > 33333U;
	}
	if (depth > screen_fps_stats.queue_depth_max) {
		screen_fps_stats.queue_depth_max = depth;
	}
	taskEXIT_CRITICAL();
}
#endif

typedef enum video_handover_state_e {
	VIDEO_HANDOVER_EMPTY = 0,
	VIDEO_HANDOVER_SOURCE,
	VIDEO_HANDOVER_OWNED
} video_handover_state_t;

typedef struct video_handover_owner_s {
	void *pointer;
	size_t allocation_length;
	size_t frame_length;
	TaskHandle_t producer_task;
	uint32_t sequence;
	uint8_t state;
	uint8_t references;
} video_handover_owner_t;

typedef struct video_handover_active_s {
	/* Read without the critical section by the global memcpy fast reject. */
	TaskHandle_t volatile task;
	void *source;
	void *temporary;
	size_t frame_length;
	uint32_t owner_index;
	uint8_t memcpy_elided;
	uint8_t ownership_published;
	uint8_t queue_committed;
} video_handover_active_t;

typedef struct video_handover_stats_s {
	uint32_t source_allocations;
	uint32_t source_table_full;
	uint32_t callbacks;
	uint32_t eligible;
	uint32_t no_source;
	uint32_t active_table_full;
	uint32_t small_frames;
	uint32_t substitutions;
	uint32_t memcpy_elisions;
	uint32_t destination_allocations;
	uint32_t push_prepared;
	uint32_t push_committed;
	uint32_t push_failed;
	uint32_t producer_releases;
	uint32_t consumer_releases;
	uint32_t final_frees;
	uint32_t anomalies;
	uint64_t bytes_saved;
	uint32_t inflight_max;
} video_handover_stats_t;

typedef struct video_handover_gate_stats_s {
	uint32_t calls;
	uint32_t waits;
	uint32_t wait_loops;
	uint64_t wait_sum_us;
	uint32_t wait_max_us;
	uint32_t observed_max;
} video_handover_gate_stats_t;

static video_handover_owner_t video_handover_owners[
	VIDEO_HANDOVER_SOURCE_SLOTS];
static video_handover_active_t video_handover_active[
	VIDEO_HANDOVER_ACTIVE_SLOTS];
static video_handover_stats_t video_handover_stats;
static video_handover_gate_stats_t video_handover_gate_stats
	__attribute__((section(".lpddr.bss.video_handover_gate_stats")));
static uint32_t video_handover_sequence;
static uint8_t video_handover_enabled = 1U;
static uint8_t video_handover_first_commit_logged;
static uint8_t video_handover_first_producer_release_logged;
static uint8_t video_handover_first_consumer_release_logged;

static uint32_t video_handover_inflight_locked(void)
{
	uint32_t count = 0U;
	uint32_t i;

	for (i = 0U; i < VIDEO_HANDOVER_SOURCE_SLOTS; i++) {
		if (video_handover_owners[i].state == VIDEO_HANDOVER_OWNED) {
			count++;
		}
	}
	return count;
}

static video_handover_active_t *video_handover_find_active_locked(
	TaskHandle_t task)
{
	uint32_t i;

	for (i = 0U; i < VIDEO_HANDOVER_ACTIVE_SLOTS; i++) {
		if (video_handover_active[i].task == task) {
			return &video_handover_active[i];
		}
	}
	return NULL;
}

static int video_handover_find_owner_locked(const void *pointer)
{
	uint32_t i;

	for (i = 0U; i < VIDEO_HANDOVER_SOURCE_SLOTS; i++) {
		if ((video_handover_owners[i].state != VIDEO_HANDOVER_EMPTY) &&
		    (video_handover_owners[i].pointer == pointer)) {
			return (int)i;
		}
	}
	return -1;
}

static void video_handover_clear_owner_locked(video_handover_owner_t *owner)
{
	*owner = (video_handover_owner_t){ 0 };
}

static void video_handover_disable(const char *reason, const void *pointer)
{
	taskENTER_CRITICAL();
	video_handover_enabled = 0U;
	video_handover_stats.anomalies++;
	taskEXIT_CRITICAL();
	rt_printf("[VIDEOHOF][FATAL] reason=%s pointer=0x%08lx; "
		  "new substitutions disabled\r\n", reason,
		  (unsigned long)(uintptr_t)pointer);
}

void carbox_video_handover_gate(const void *source, int frame_length)
{
#if CONFIG_VIDEO_HANDOVER_BACKPRESSURE
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	uint32_t start_us = 0U;
	uint32_t wait_us;
	uint32_t inflight;
	uint32_t loops = 0U;
	int owner_index;
	int waited = 0;

	if ((source == NULL) ||
	    (frame_length < (int)VIDEO_HANDOVER_ZERO_COPY_MIN_BYTES) ||
	    (VIDEO_HANDOVER_MAX_INFLIGHT == 0U)) {
		return;
	}

	/* This hook runs before the closed AirPlayScreen_SendVideo() takes
	 * mutexScreen.  Waiting inside CVector_push_back() would deadlock the
	 * ScreenThread that needs the same mutex to dequeue a frame. */
	for (;;) {
		taskENTER_CRITICAL();
		owner_index = video_handover_find_owner_locked(source);
		if (!video_handover_enabled || (owner_index < 0) ||
		    (video_handover_owners[owner_index].state !=
			VIDEO_HANDOVER_SOURCE) ||
		    (video_handover_owners[owner_index].producer_task != task) ||
		    (video_handover_owners[owner_index].allocation_length <
			(size_t)frame_length)) {
			taskEXIT_CRITICAL();
			return;
		}
		inflight = video_handover_inflight_locked();
		if (!waited) {
			video_handover_gate_stats.calls++;
			if (inflight > video_handover_gate_stats.observed_max) {
				video_handover_gate_stats.observed_max = inflight;
			}
		}
		if (inflight < VIDEO_HANDOVER_MAX_INFLIGHT) {
			taskEXIT_CRITICAL();
			break;
		}
		if (!waited) {
			video_handover_gate_stats.waits++;
			start_us = hal_read_curtime_us();
			waited = 1;
		}
		taskEXIT_CRITICAL();
		loops++;
		vTaskDelay((TickType_t)VIDEO_HANDOVER_GATE_POLL_TICKS);
	}

	if (waited) {
		wait_us = hal_read_curtime_us() - start_us;
		taskENTER_CRITICAL();
		video_handover_gate_stats.wait_loops += loops;
		video_handover_gate_stats.wait_sum_us += wait_us;
		if (wait_us > video_handover_gate_stats.wait_max_us) {
			video_handover_gate_stats.wait_max_us = wait_us;
		}
		taskEXIT_CRITICAL();
	}
#else
	(void)source;
	(void)frame_length;
#endif
}

void *carbox_video_handover_source_malloc(size_t length)
{
	/*
	 * The source may later become the zero-copy plaintext input of RTL8195B's
	 * combined ChaCha DMA. The ROM accepts the logical partial length but may
	 * fetch through ROUND_UP(length, 16), so retain the logical allocation
	 * metadata while providing 15 bytes of physically readable tail padding.
	 */
	void *pointer = length <= SIZE_MAX - 15U ? malloc(length + 15U) : NULL;
	TaskHandle_t task;
	uint32_t i;

	carbox_screen_rx_record_alloc(pointer, length);
	if ((pointer == NULL) || (length < VIDEO_HANDOVER_ZERO_COPY_MIN_BYTES)) {
		return pointer;
	}
	task = xTaskGetCurrentTaskHandle();
	taskENTER_CRITICAL();
	if (!video_handover_enabled) {
		taskEXIT_CRITICAL();
		return pointer;
	}
	for (i = 0U; i < VIDEO_HANDOVER_SOURCE_SLOTS; i++) {
		if (video_handover_owners[i].state == VIDEO_HANDOVER_EMPTY) {
			video_handover_owners[i].pointer = pointer;
			video_handover_owners[i].allocation_length = length;
			video_handover_owners[i].producer_task = task;
			video_handover_owners[i].state = VIDEO_HANDOVER_SOURCE;
			video_handover_stats.source_allocations++;
			taskEXIT_CRITICAL();
			return pointer;
		}
	}
	video_handover_stats.source_table_full++;
	taskEXIT_CRITICAL();
	return pointer;
}

void carbox_video_handover_begin(const void *source, int frame_length)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	video_handover_active_t *active = NULL;
	uint32_t i;
	int owner_index;

	taskENTER_CRITICAL();
	video_handover_stats.callbacks++;
	if (!video_handover_enabled || (source == NULL) ||
	    (frame_length < (int)VIDEO_HANDOVER_ZERO_COPY_MIN_BYTES)) {
		if ((source != NULL) && (frame_length > 0) &&
		    ((uint32_t)frame_length < VIDEO_HANDOVER_ZERO_COPY_MIN_BYTES)) {
			video_handover_stats.small_frames++;
		}
		taskEXIT_CRITICAL();
		return;
	}
	owner_index = video_handover_find_owner_locked(source);
	if ((owner_index < 0) ||
	    (video_handover_owners[owner_index].state != VIDEO_HANDOVER_SOURCE) ||
	    (video_handover_owners[owner_index].producer_task != task) ||
	    (video_handover_owners[owner_index].allocation_length <
		(size_t)frame_length)) {
		video_handover_stats.no_source++;
		taskEXIT_CRITICAL();
		return;
	}
	for (i = 0U; i < VIDEO_HANDOVER_ACTIVE_SLOTS; i++) {
		if (video_handover_active[i].task == NULL) {
			active = &video_handover_active[i];
			break;
		}
	}
	if (active == NULL) {
		video_handover_stats.active_table_full++;
		taskEXIT_CRITICAL();
		return;
	}
	active->source = (void *)source;
	active->frame_length = (size_t)frame_length;
	active->owner_index = (uint32_t)owner_index;
	/* Publish the task last, after the transaction fields are initialized. */
	active->task = task;
	video_handover_stats.eligible++;
	taskEXIT_CRITICAL();
}

void *carbox_video_handover_destination_malloc(size_t length)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	video_handover_active_t *active;
	void *result;

	/*
	 * AirPlayScreen.o is a closed customer object. Disassembly of
	 * AirPlayScreen_SendScreenNormalFrame() shows this exact layout:
	 *
	 *     malloc(payload_length + 16-byte tag + 128-byte header)
	 *     ciphertext = allocation + 128
	 *     chacha20_poly1305_encrypt(..., payload_length, ciphertext)
	 *
	 * RTL8195B's ROM combined ChaCha20-Poly1305 encrypt accepts the logical
	 * non-16-byte length, but DMA writes through ROUND_UP(length, 16). The tag
	 * area already provides 16 writable bytes after ciphertext; add a further
	 * 15-byte physical guard to make the closed-object contract explicit and
	 * keep the same rule as the zero-copy source allocation. Retain the
	 * customer's original logical length everywhere else.
	 * free() remains ABI-compatible because the returned pointer is unchanged.
	 */
	result = length <= SIZE_MAX - 15U ? malloc(length + 15U) : NULL;

	taskENTER_CRITICAL();
	active = video_handover_find_active_locked(task);
	if ((result != NULL) && (active != NULL) &&
	    (active->temporary == NULL) &&
	    (active->frame_length == length)) {
		/* Phase B deliberately keeps the real temporary allocation.  The
		 * queue wrapper, not malloc(), performs the pointer replacement and
		 * can therefore prove/rollback CVector publication. */
		active->temporary = result;
		video_handover_stats.destination_allocations++;
	}
	taskEXIT_CRITICAL();

	carbox_screen_tx_allocation(result, length);
	return result;
}

int carbox_video_handover_memcpy_is_elided(void *destination,
					   const void *source, size_t length)
{
	TaskHandle_t task;
	video_handover_active_t *active;
	uint32_t i;
	uint32_t ipsr;
	int elide = 0;

	if ((destination == NULL) || (source == NULL) ||
	    (destination == source)) {
		return 0;
	}
	/* Defense in depth for this globally reachable hook.  Its transaction
	 * table is task-owned and uses taskENTER_CRITICAL(), so it must never be
	 * inspected from an interrupt.  ISR memcpy always remains a real copy. */
	__asm volatile("mrs %0, ipsr" : "=r" (ipsr));
	if (ipsr != 0U) {
		return 0;
	}
	task = xTaskGetCurrentTaskHandle();
	/* __wrap_memcpy() is global, while handover transactions belong only to
	 * the AirPlay producer task.  Inspect the volatile task publications
	 * without entering a critical section; the locked lookup below remains
	 * authoritative.  A concurrent teardown can only cause a harmless false
	 * positive here, while begin() publishes task last to avoid false matches
	 * against a partially initialized transaction. */
	if (task == NULL) {
		return 0;
	}
	for (i = 0U; i < VIDEO_HANDOVER_ACTIVE_SLOTS; i++) {
		if (video_handover_active[i].task == task) {
			break;
		}
	}
	if (i == VIDEO_HANDOVER_ACTIVE_SLOTS) {
		return 0;
	}
	taskENTER_CRITICAL();
	active = video_handover_find_active_locked(task);
	if ((active != NULL) &&
	    (active->temporary == destination) &&
	    (active->source == source) && (active->frame_length == length) &&
	    !active->memcpy_elided) {
		active->memcpy_elided = 1U;
		video_handover_stats.memcpy_elisions++;
		elide = 1;
	}
	taskEXIT_CRITICAL();
	return elide;
}

int carbox_video_handover_prepare_push(void *temporary, int frame_length,
					void **replacement)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	video_handover_active_t *active;
	video_handover_owner_t *owner;
	uint32_t inflight;
	int prepared = 0;

	if (replacement != NULL) {
		*replacement = temporary;
	}
	taskENTER_CRITICAL();
	active = video_handover_find_active_locked(task);
	if ((active != NULL) && video_handover_enabled &&
	    active->memcpy_elided && !active->ownership_published &&
	    (active->temporary == temporary) &&
	    (frame_length > 0) &&
	    (active->frame_length == (size_t)frame_length) &&
	    (active->owner_index < VIDEO_HANDOVER_SOURCE_SLOTS)) {
		owner = &video_handover_owners[active->owner_index];
		if ((owner->state == VIDEO_HANDOVER_SOURCE) &&
		    (owner->pointer == active->source) &&
		    (owner->producer_task == task)) {
			owner->state = VIDEO_HANDOVER_OWNED;
			owner->frame_length = active->frame_length;
			owner->references = VIDEO_HANDOVER_REF_PRODUCER |
				VIDEO_HANDOVER_REF_CONSUMER;
			owner->sequence = ++video_handover_sequence;
			active->ownership_published = 1U;
			video_handover_stats.substitutions++;
			video_handover_stats.push_prepared++;
			inflight = video_handover_inflight_locked();
			if (inflight > video_handover_stats.inflight_max) {
				video_handover_stats.inflight_max = inflight;
			}
			if (replacement != NULL) {
				*replacement = active->source;
			}
			prepared = 1;
		}
	}
	taskEXIT_CRITICAL();
	return prepared;
}

void carbox_video_handover_finish_push(void *temporary, int pushed)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	video_handover_active_t *active;
	video_handover_owner_t *owner;
	void *free_temporary = NULL;
	void *trace_source = NULL;
	size_t trace_length = 0U;
	int fatal = 0;

	taskENTER_CRITICAL();
	active = video_handover_find_active_locked(task);
	if ((active != NULL) && active->ownership_published &&
	    (active->temporary == temporary) &&
	    (active->owner_index < VIDEO_HANDOVER_SOURCE_SLOTS)) {
		owner = &video_handover_owners[active->owner_index];
		if (pushed) {
			active->queue_committed = 1U;
			video_handover_stats.push_committed++;
			video_handover_stats.bytes_saved += active->frame_length;
			free_temporary = active->temporary;
			active->temporary = NULL;
			if (!video_handover_first_commit_logged) {
				video_handover_first_commit_logged = 1U;
				trace_source = active->source;
				trace_length = active->frame_length;
			}
		} else if ((owner->state == VIDEO_HANDOVER_OWNED) &&
			   (owner->references == (VIDEO_HANDOVER_REF_PRODUCER |
				VIDEO_HANDOVER_REF_CONSUMER))) {
			/* No consumer can see an item which was not inserted. */
			owner->state = VIDEO_HANDOVER_SOURCE;
			owner->references = 0U;
			owner->frame_length = 0U;
			active->ownership_published = 0U;
			video_handover_stats.push_failed++;
			free_temporary = active->temporary;
			active->temporary = NULL;
		} else {
			fatal = 1;
		}
	}
	taskEXIT_CRITICAL();
	if (free_temporary != NULL) {
		carbox_screen_tx_release(free_temporary);
		free(free_temporary);
	}
	if (trace_source != NULL) {
		rt_printf("[VIDEOHOF][TRACE] first phase-b commit source=0x%08lx "
			  "len=%lu task=%s\r\n",
			  (unsigned long)(uintptr_t)trace_source,
			  (unsigned long)trace_length, pcTaskGetName(task));
	}
	if (fatal) {
		video_handover_disable("push-rollback-race", temporary);
	}
}

void carbox_video_handover_end(void)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	video_handover_active_t *active;
	void *bad_pointer = NULL;
	void *late_temporary = NULL;
	void *late_source = NULL;
	size_t late_length = 0U;

	taskENTER_CRITICAL();
	active = video_handover_find_active_locked(task);
	if (active != NULL) {
		if (active->ownership_published && !active->queue_committed) {
			bad_pointer = active->source;
		} else if (active->memcpy_elided &&
			   !active->ownership_published &&
			   (active->temporary != NULL)) {
			/* The expected CVector hook was not observed. Materialize the
			 * temporary buffer before disabling future substitutions. */
			late_temporary = active->temporary;
			late_source = active->source;
			late_length = active->frame_length;
			bad_pointer = active->source;
		}
		*active = (video_handover_active_t){ 0 };
	}
	taskEXIT_CRITICAL();
	if (late_temporary != NULL) {
		memcpy(late_temporary, late_source, late_length);
	}
	if (bad_pointer != NULL) {
		video_handover_disable("incomplete-queue-transaction", bad_pointer);
	}
}

static void video_handover_release(void *pointer, uint8_t reference,
				   const char *duplicate_reason)
{
	video_handover_owner_t *owner;
	int owner_index;
	int actual_free = 0;
	int duplicate = 0;
	int trace_release = 0;
	uint8_t references_after = 0U;
	uint32_t trace_sequence = 0U;

	if (pointer == NULL) {
		return;
	}
	taskENTER_CRITICAL();
	owner_index = video_handover_find_owner_locked(pointer);
	if (owner_index < 0) {
		actual_free = 1;
	} else {
		owner = &video_handover_owners[owner_index];
		if (owner->state == VIDEO_HANDOVER_SOURCE) {
			/* The receiver allocation was not selected for zero-copy. */
			video_handover_clear_owner_locked(owner);
			actual_free = 1;
		} else if ((owner->state == VIDEO_HANDOVER_OWNED) &&
			   ((owner->references & reference) != 0U)) {
			owner->references &= (uint8_t)~reference;
			references_after = owner->references;
			trace_sequence = owner->sequence;
			if (reference == VIDEO_HANDOVER_REF_PRODUCER) {
				video_handover_stats.producer_releases++;
				if (!video_handover_first_producer_release_logged) {
					video_handover_first_producer_release_logged = 1U;
					trace_release = 1;
				}
			} else {
				video_handover_stats.consumer_releases++;
				if (!video_handover_first_consumer_release_logged) {
					video_handover_first_consumer_release_logged = 1U;
					trace_release = 1;
				}
			}
			if (owner->references == 0U) {
				video_handover_clear_owner_locked(owner);
				video_handover_stats.final_frees++;
				actual_free = 1;
			}
		} else {
			duplicate = 1;
		}
	}
	taskEXIT_CRITICAL();
	if (actual_free) {
		free(pointer);
	} else if (duplicate) {
		/* Never turn a lifecycle anomaly into a double-free. */
		video_handover_disable(duplicate_reason, pointer);
	}
	if (trace_release) {
		rt_printf("[VIDEOHOF][TRACE] first %s release pointer=0x%08lx "
			  "seq=%lu refs_after=0x%02x final=%u task=%s\r\n",
			  (reference == VIDEO_HANDOVER_REF_PRODUCER) ?
			  "producer" : "consumer",
			  (unsigned long)(uintptr_t)pointer,
			  (unsigned long)trace_sequence,
			  (unsigned int)references_after,
			  (unsigned int)actual_free,
			  pcTaskGetName(xTaskGetCurrentTaskHandle()));
	}
}

void carbox_video_handover_producer_free(void *pointer)
{
	carbox_screen_rx_record_free(pointer);
	video_handover_release(pointer, VIDEO_HANDOVER_REF_PRODUCER,
				 "duplicate-producer-release");
}

void carbox_video_handover_consumer_free(void *pointer)
{
	if (carbox_screen_tx_owned_consumer_release(pointer)) {
		return;
	}
	carbox_screen_tx_release(pointer);
	video_handover_release(pointer, VIDEO_HANDOVER_REF_CONSUMER,
				 "duplicate-consumer-release");
}

void carbox_video_handover_report(uint32_t sequence)
{
	video_handover_stats_t stats;
	uint32_t candidates = 0U;
	uint32_t inflight = 0U;
	uint32_t active = 0U;
	uint32_t i;
	uint8_t enabled;

	taskENTER_CRITICAL();
	stats = video_handover_stats;
	video_handover_stats = (video_handover_stats_t){ 0 };
	for (i = 0U; i < VIDEO_HANDOVER_SOURCE_SLOTS; i++) {
		if (video_handover_owners[i].state == VIDEO_HANDOVER_SOURCE) {
			candidates++;
		} else if (video_handover_owners[i].state == VIDEO_HANDOVER_OWNED) {
			inflight++;
		}
	}
	for (i = 0U; i < VIDEO_HANDOVER_ACTIVE_SLOTS; i++) {
		active += video_handover_active[i].task != NULL ? 1U : 0U;
	}
	enabled = video_handover_enabled;
	taskEXIT_CRITICAL();

	rt_printf("[VIDEOHOF][%lu] enabled=%u callbacks/eligible/swap/elide="
		  "%lu/%lu/%lu/%lu bytes_saved=%llu small/no_source="
		  "%lu/%lu\r\n", (unsigned long)sequence, enabled,
		  (unsigned long)stats.callbacks,
		  (unsigned long)stats.eligible,
		  (unsigned long)stats.substitutions,
		  (unsigned long)stats.memcpy_elisions,
		  (unsigned long long)stats.bytes_saved,
		  (unsigned long)stats.small_frames,
		  (unsigned long)stats.no_source);
	rt_printf("[VIDEOHOF][%lu] source_alloc/table_full/active_full="
		  "%lu/%lu/%lu dst_alloc/push prep/ok/fail=%lu/%lu/%lu/%lu "
		  "release producer/consumer/final="
		  "%lu/%lu/%lu live candidate/owned/active=%lu/%lu/%lu "
		  "owned_max=%lu anomaly=%lu\r\n", (unsigned long)sequence,
		  (unsigned long)stats.source_allocations,
		  (unsigned long)stats.source_table_full,
		  (unsigned long)stats.active_table_full,
		  (unsigned long)stats.destination_allocations,
		  (unsigned long)stats.push_prepared,
		  (unsigned long)stats.push_committed,
		  (unsigned long)stats.push_failed,
		  (unsigned long)stats.producer_releases,
		  (unsigned long)stats.consumer_releases,
		  (unsigned long)stats.final_frees,
		  (unsigned long)candidates, (unsigned long)inflight,
		  (unsigned long)active, (unsigned long)stats.inflight_max,
		  (unsigned long)stats.anomalies);
}

void carbox_video_handover_gate_report(uint32_t sequence)
{
#if CONFIG_VIDEO_HANDOVER_BACKPRESSURE
	video_handover_gate_stats_t stats;
	uint32_t live;

	taskENTER_CRITICAL();
	stats = video_handover_gate_stats;
	video_handover_gate_stats = (video_handover_gate_stats_t){ 0 };
	live = video_handover_inflight_locked();
	taskEXIT_CRITICAL();
	if (stats.calls == 0U) {
		return;
	}
	rt_printf("[VIDEOHOF_GATE][%lu] calls/wait/loops=%lu/%lu/%lu "
		  "wait_us avg/max=%llu/%lu inflight live/seen/limit=%lu/%lu/%u\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.calls,
		  (unsigned long)stats.waits,
		  (unsigned long)stats.wait_loops,
		  (unsigned long long)(stats.waits != 0U ?
			stats.wait_sum_us / stats.waits : 0U),
		  (unsigned long)stats.wait_max_us,
		  (unsigned long)live,
		  (unsigned long)stats.observed_max,
		  (unsigned int)VIDEO_HANDOVER_MAX_INFLIGHT);
#else
	(void)sequence;
#endif
}

#if !CONFIG_SCREEN_QUEUE_PROFILE
extern void __real_AirPlayScreen_SendVideo(const void *data, int bytes);
extern void __real_CVector_push_back(void *vector, const void *element);
#if CONFIG_SCREEN_FPS_PROFILE
extern void __real_CVector_erase(void *vector, int index);
#endif

/* Minimal closed CVector views required by the production handover hook.
 * Profiling builds use screen_queue_profiler.c's richer wrapper instead. */
typedef struct video_handover_vector_view_s {
	int capacity;
	int size;
	void *data;
	int element_size;
} video_handover_vector_view_t;

typedef struct video_handover_frame_item_s {
	void *data;
	int bytes;
} video_handover_frame_item_t;

void __wrap_AirPlayScreen_SendVideo(const void *data, int bytes)
{
#if CONFIG_SCREEN_FPS_PROFILE
	uint32_t start_us = screen_fps_begin(bytes);
#endif
	carbox_screen_rx_record_send_video(data, bytes);
	carbox_video_handover_gate(data, bytes);
	carbox_video_handover_begin(data, bytes);
	__real_AirPlayScreen_SendVideo(data, bytes);
	carbox_video_handover_end();
#if CONFIG_SCREEN_FPS_PROFILE
	screen_fps_end(start_us);
#endif
}

void __wrap_CVector_push_back(void *vector, const void *element)
{
	video_handover_vector_view_t *view =
		(video_handover_vector_view_t *)vector;
	video_handover_frame_item_t original;
	video_handover_frame_item_t replacement;
	int size_before = 0;
	int prepared = 0;
#if CONFIG_SCREEN_FPS_PROFILE
	uint32_t profile_start_us = 0U;
	uint32_t profile_arrival_us = 0U;
	int profile_measured = 0;
#endif

	/* prepare_push() also validates the active task, exact temporary pointer
	 * and exact frame length.  The element-size check prevents interpreting an
	 * unrelated CVector item as AirPlay's {pointer, length} pair. */
	if ((view != NULL) && (element != NULL) &&
	    (view->element_size == (int)sizeof(video_handover_frame_item_t))) {
		original = *(const video_handover_frame_item_t *)element;
		replacement = original;
		size_before = view->size;
		prepared = carbox_video_handover_prepare_push(original.data,
			original.bytes, &replacement.data);
#if CONFIG_SCREEN_FPS_PROFILE
		profile_start_us = hal_read_curtime_us();
		profile_measured = screen_fps_push_begin(vector,
			view->element_size, &profile_arrival_us);
#endif
	}

	__real_CVector_push_back(vector,
		prepared ? (const void *)&replacement : element);
	if (prepared) {
		int pushed = (view->size == size_before + 1);

		carbox_video_handover_finish_push(original.data, pushed);
	}
#if CONFIG_SCREEN_FPS_PROFILE
	if (profile_measured) {
		screen_fps_push_end(profile_start_us, profile_arrival_us,
			view->size == size_before + 1,
			view->size > 0 ? (uint32_t)view->size : 0U);
	}
#endif
}

#if CONFIG_SCREEN_FPS_PROFILE
void __wrap_CVector_erase(void *vector, int index)
{
	video_handover_vector_view_t *view =
		(video_handover_vector_view_t *)vector;
	uint32_t arrival_us = 0U;
	int valid = (view != NULL) && (view->size > 0);
	int measured = valid ?
		screen_fps_erase_begin(vector, index, &arrival_us) : 0;

	__real_CVector_erase(vector, index);
	if (measured) {
		screen_fps_erase_end(arrival_us,
			view->size > 0 ? (uint32_t)view->size : 0U);
	}
}
#endif
#endif

void carbox_screen_fps_report(uint32_t sequence)
{
#if CONFIG_SCREEN_FPS_PROFILE
	screen_fps_stats_t stats;
	uint32_t queue_depth;
	uint32_t fps_x100;
	uint32_t rate_kbps;

	taskENTER_CRITICAL();
	stats = screen_fps_stats;
	screen_fps_stats = (screen_fps_stats_t){ 0 };
	queue_depth = screen_fps_queue_count;
	taskEXIT_CRITICAL();
	if (stats.frames == 0U) {
		return;
	}
	/* The caller is the existing 10-second PC profiler window. */
	fps_x100 = stats.frames * 10U;
	rate_kbps = (uint32_t)(stats.bytes / 1250U);
	rt_printf("[SCREENFPS][%lu] rx frames/bytes/fps/kbps=%lu/%llu/%lu.%02lu/%lu "
		  "interval_us avg/max=%llu/%lu gaps >20/33/50/100ms=%lu/%lu/%lu/%lu "
		  "send_us avg/max=%llu/%lu over16/33ms=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.frames,
		  (unsigned long long)stats.bytes,
		  (unsigned long)(fps_x100 / 100U),
		  (unsigned long)(fps_x100 % 100U),
		  (unsigned long)rate_kbps,
		  (unsigned long long)(stats.interval_samples != 0U ?
			stats.interval_sum_us / stats.interval_samples : 0U),
		  (unsigned long)stats.interval_max_us,
		  (unsigned long)stats.gap_over_20ms,
		  (unsigned long)stats.gap_over_33ms,
		  (unsigned long)stats.gap_over_50ms,
		  (unsigned long)stats.gap_over_100ms,
		  (unsigned long long)(stats.frames != 0U ?
			stats.send_sum_us / stats.frames : 0U),
		  (unsigned long)stats.send_max_us,
		  (unsigned long)stats.send_over_16ms,
		  (unsigned long)stats.send_over_33ms);
	rt_printf("[SCREENFPS][%lu] queue enq/deq=%lu/%lu depth now/max=%lu/%lu "
		  "age_us samples/avg/max=%lu/%llu/%lu over16/33ms=%lu/%lu "
		  "push_us avg/max=%llu/%lu tracking_lost=%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.pushes,
		  (unsigned long)stats.dequeues,
		  (unsigned long)queue_depth,
		  (unsigned long)stats.queue_depth_max,
		  (unsigned long)stats.queue_age_samples,
		  (unsigned long long)(stats.queue_age_samples != 0U ?
			stats.queue_age_sum_us / stats.queue_age_samples : 0U),
		  (unsigned long)stats.queue_age_max_us,
		  (unsigned long)stats.queue_age_over_16ms,
		  (unsigned long)stats.queue_age_over_33ms,
		  (unsigned long long)(stats.pushes != 0U ?
			stats.push_sum_us / stats.pushes : 0U),
		  (unsigned long)stats.push_max_us,
		  (unsigned long)stats.tracking_lost);
#else
	(void)sequence;
#endif
}

#else

void carbox_video_handover_begin(const void *source, int frame_length)
{
	(void)source;
	(void)frame_length;
}
void carbox_video_handover_gate(const void *source, int frame_length)
{
	(void)source;
	(void)frame_length;
}
void carbox_video_handover_end(void) { }
void *carbox_video_handover_source_malloc(size_t length) { return malloc(length); }
void *carbox_video_handover_destination_malloc(size_t length)
{
	void *pointer = malloc(length);
	carbox_screen_tx_allocation(pointer, length);
	return pointer;
}
void carbox_video_handover_producer_free(void *pointer) { free(pointer); }
void carbox_video_handover_consumer_free(void *pointer)
{
	if (carbox_screen_tx_owned_consumer_release(pointer)) {
		return;
	}
	carbox_screen_tx_release(pointer);
	free(pointer);
}
int carbox_video_handover_memcpy_is_elided(void *destination,
					   const void *source, size_t length)
{
	(void)destination;
	(void)source;
	(void)length;
	return 0;
}
int carbox_video_handover_prepare_push(void *temporary, int frame_length,
					void **replacement)
{
	(void)frame_length;
	if (replacement != NULL) *replacement = temporary;
	return 0;
}
void carbox_video_handover_finish_push(void *temporary, int pushed)
{
	(void)temporary;
	(void)pushed;
}
void carbox_video_handover_report(uint32_t sequence) { (void)sequence; }
void carbox_video_handover_gate_report(uint32_t sequence) { (void)sequence; }
void carbox_screen_fps_report(uint32_t sequence) { (void)sequence; }

#endif
