#include "video_handover_zero_copy.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "screen_tx_direct_crypto.h"

#ifndef CONFIG_VIDEO_HANDOVER_ZERO_COPY
#define CONFIG_VIDEO_HANDOVER_ZERO_COPY 0
#endif

#ifndef VIDEO_HANDOVER_ZERO_COPY_MIN_BYTES
#define VIDEO_HANDOVER_ZERO_COPY_MIN_BYTES 4096U
#endif

#ifndef CONFIG_SCREEN_QUEUE_PROFILE
#define CONFIG_SCREEN_QUEUE_PROFILE 0
#endif

#if CONFIG_VIDEO_HANDOVER_ZERO_COPY

#define VIDEO_HANDOVER_SOURCE_SLOTS 64U
#define VIDEO_HANDOVER_ACTIVE_SLOTS  4U
#define VIDEO_HANDOVER_REF_PRODUCER  1U
#define VIDEO_HANDOVER_REF_CONSUMER  2U

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
	TaskHandle_t task;
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

static video_handover_owner_t video_handover_owners[
	VIDEO_HANDOVER_SOURCE_SLOTS];
static video_handover_active_t video_handover_active[
	VIDEO_HANDOVER_ACTIVE_SLOTS];
static video_handover_stats_t video_handover_stats;
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

void *carbox_video_handover_source_malloc(size_t length)
{
	void *pointer = malloc(length);
	TaskHandle_t task;
	uint32_t i;

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
	active->task = task;
	active->source = (void *)source;
	active->frame_length = (size_t)frame_length;
	active->owner_index = (uint32_t)owner_index;
	video_handover_stats.eligible++;
	taskEXIT_CRITICAL();
}

void *carbox_video_handover_destination_malloc(size_t length)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	video_handover_active_t *active;
	void *result = malloc(length);

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
	int elide = 0;

	if ((destination == NULL) || (source == NULL) ||
	    (destination == source)) {
		return 0;
	}
	task = xTaskGetCurrentTaskHandle();
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
	video_handover_release(pointer, VIDEO_HANDOVER_REF_PRODUCER,
				 "duplicate-producer-release");
}

void carbox_video_handover_consumer_free(void *pointer)
{
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

#if !CONFIG_SCREEN_QUEUE_PROFILE
extern void __real_AirPlayScreen_SendVideo(const void *data, int bytes);

void __wrap_AirPlayScreen_SendVideo(const void *data, int bytes)
{
	carbox_video_handover_begin(data, bytes);
	__real_AirPlayScreen_SendVideo(data, bytes);
	carbox_video_handover_end();
}
#endif

#else

void carbox_video_handover_begin(const void *source, int frame_length)
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

#endif
