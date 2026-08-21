#include "screen_queue_wait.h"

#include <stddef.h>

#include "semphr.h"
#include "task.h"
#include "hal_timer.h"
#include "diag.h"

#ifndef CONFIG_SCREEN_QUEUE_EVENT_WAIT
#define CONFIG_SCREEN_QUEUE_EVENT_WAIT 0
#endif

#ifndef CONFIG_VIDEO_HANDOVER_ZERO_COPY
#define CONFIG_VIDEO_HANDOVER_ZERO_COPY 0
#endif

#ifndef CONFIG_SCREEN_QUEUE_PROFILE
#define CONFIG_SCREEN_QUEUE_PROFILE 0
#endif

#if CONFIG_SCREEN_QUEUE_EVENT_WAIT

typedef struct screen_queue_wait_stats_s {
	uint32_t signals;
	uint32_t coalesced;
	uint32_t rearms;
	uint32_t waits;
	uint32_t wakes;
	uint32_t timeouts;
	uint32_t immediate;
	uint64_t wait_us_total;
	uint32_t wait_us_max;
	uint32_t scope_errors;
	uint32_t consumer_switches;
} screen_queue_wait_stats_t;

static StaticSemaphore_t screen_queue_wait_storage;
static SemaphoreHandle_t screen_queue_wait_semaphore;
static TaskHandle_t screen_queue_publish_task;
static uint32_t screen_queue_publish_depth;
static void *screen_queue_vector;
static TaskHandle_t screen_queue_consumer_task;
static screen_queue_wait_stats_t screen_queue_wait_stats
	__attribute__((section(".lpddr.bss.screen_queue_wait_stats")));

static SemaphoreHandle_t screen_queue_wait_get_semaphore(void)
{
	SemaphoreHandle_t semaphore;

	/* Static creation does not allocate or schedule.  Protect the one-time
	 * initialization because the first producer and ScreenThread may arrive on
	 * adjacent ticks when a session starts. */
	taskENTER_CRITICAL();
	if (screen_queue_wait_semaphore == NULL) {
		screen_queue_wait_semaphore = xSemaphoreCreateBinaryStatic(
			&screen_queue_wait_storage);
	}
	semaphore = screen_queue_wait_semaphore;
	taskEXIT_CRITICAL();
	return semaphore;
}

void carbox_screen_queue_publish_begin(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();

	(void)screen_queue_wait_get_semaphore();
	taskENTER_CRITICAL();
	if (screen_queue_publish_depth == 0U) {
		screen_queue_publish_task = current;
		screen_queue_publish_depth = 1U;
	} else if (screen_queue_publish_task == current) {
		screen_queue_publish_depth++;
	} else {
		screen_queue_wait_stats.scope_errors++;
	}
	taskEXIT_CRITICAL();
}

void carbox_screen_queue_publish_end(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();

	taskENTER_CRITICAL();
	if (screen_queue_publish_depth != 0U &&
	    screen_queue_publish_task == current) {
		screen_queue_publish_depth--;
		if (screen_queue_publish_depth == 0U)
			screen_queue_publish_task = NULL;
	} else {
		screen_queue_wait_stats.scope_errors++;
	}
	taskEXIT_CRITICAL();
}

static void screen_queue_wait_signal(int rearm)
{
	SemaphoreHandle_t semaphore = screen_queue_wait_get_semaphore();
	BaseType_t result = pdFALSE;

	if (semaphore != NULL)
		result = xSemaphoreGive(semaphore);
	taskENTER_CRITICAL();
	if (result == pdTRUE) {
		screen_queue_wait_stats.signals++;
		if (rearm)
			screen_queue_wait_stats.rearms++;
	} else {
		/* A binary semaphore intentionally coalesces multiple arrivals.  The
		 * erase hook rearms it while the actual vector remains non-empty. */
		screen_queue_wait_stats.coalesced++;
	}
	taskEXIT_CRITICAL();
}

void carbox_screen_queue_push_result(void *vector, int pushed)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	int is_screen_publish;

	taskENTER_CRITICAL();
	is_screen_publish = pushed && screen_queue_publish_depth != 0U &&
		screen_queue_publish_task == current;
	if (is_screen_publish)
		screen_queue_vector = vector;
	taskEXIT_CRITICAL();
	if (is_screen_publish)
		screen_queue_wait_signal(0);
}

void carbox_screen_queue_after_erase(void *vector, int remaining)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	int is_screen_vector;

	taskENTER_CRITICAL();
	/* The vector address can be reused after a CarPlay session is destroyed.
	 * Requiring the task that actually executes the patched ScreenThread wait
	 * prevents an unrelated CVector erase from rearming a stale semaphore. */
	is_screen_vector = current == screen_queue_consumer_task &&
		vector != NULL && vector == screen_queue_vector;
	taskEXIT_CRITICAL();
	if (is_screen_vector && remaining > 0)
		screen_queue_wait_signal(1);
}

void carbox_screen_queue_wait(TickType_t timeout_ticks)
{
	SemaphoreHandle_t semaphore = screen_queue_wait_get_semaphore();
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t start_us = hal_read_curtime_us();
	uint32_t elapsed_us;
	BaseType_t result = pdFALSE;

	taskENTER_CRITICAL();
	if (screen_queue_consumer_task != current) {
		screen_queue_consumer_task = current;
		screen_queue_wait_stats.consumer_switches++;
	}
	taskEXIT_CRITICAL();

	if (semaphore != NULL)
		result = xSemaphoreTake(semaphore, timeout_ticks);
	else
		vTaskDelay(timeout_ticks);
	elapsed_us = hal_read_curtime_us() - start_us;

	taskENTER_CRITICAL();
	screen_queue_wait_stats.waits++;
	screen_queue_wait_stats.wait_us_total += elapsed_us;
	if (elapsed_us > screen_queue_wait_stats.wait_us_max)
		screen_queue_wait_stats.wait_us_max = elapsed_us;
	if (result == pdTRUE) {
		screen_queue_wait_stats.wakes++;
		if (elapsed_us <= 1000U)
			screen_queue_wait_stats.immediate++;
	} else {
		screen_queue_wait_stats.timeouts++;
	}
	taskEXIT_CRITICAL();
}

void carbox_screen_queue_wait_report(uint32_t sequence)
{
	screen_queue_wait_stats_t stats;

	taskENTER_CRITICAL();
	stats = screen_queue_wait_stats;
	screen_queue_wait_stats = (screen_queue_wait_stats_t){ 0 };
	taskEXIT_CRITICAL();
	printf("[SCREENWAKE][%u] signal/coalesced/rearm=%u/%u/%u "
	       "wait/wake/timeout/immediate=%u/%u/%u/%u wait_us avg/max=%u/%u "
	       "scope_error/consumer_switch=%u/%u\n",
	       (unsigned int)sequence,
	       (unsigned int)stats.signals,
	       (unsigned int)stats.coalesced,
	       (unsigned int)stats.rearms,
	       (unsigned int)stats.waits,
	       (unsigned int)stats.wakes,
	       (unsigned int)stats.timeouts,
	       (unsigned int)stats.immediate,
	       stats.waits ? (unsigned int)(stats.wait_us_total / stats.waits) : 0U,
	       (unsigned int)stats.wait_us_max,
	       (unsigned int)stats.scope_errors,
	       (unsigned int)stats.consumer_switches);
}

#if !CONFIG_VIDEO_HANDOVER_ZERO_COPY && !CONFIG_SCREEN_QUEUE_PROFILE
/* Event-wait must not depend on zero-copy being enabled.  These minimal
 * wrappers provide only exact ScreenVideo publication scoping and queue
 * notification; the zero-copy and full-profiler builds provide their richer
 * versions in their existing translation units. */
typedef struct screen_queue_wait_vector_view_s {
	int capacity;
	int size;
	void *data;
	int element_size;
} screen_queue_wait_vector_view_t;

extern void __real_AirPlayScreen_SendVideo(const void *data, int bytes);
extern void __real_CVector_push_back(void *vector, const void *element);
extern void __real_CVector_erase(void *vector, int index);

void __wrap_AirPlayScreen_SendVideo(const void *data, int bytes)
{
	carbox_screen_queue_publish_begin();
	__real_AirPlayScreen_SendVideo(data, bytes);
	carbox_screen_queue_publish_end();
}

void __wrap_CVector_push_back(void *vector, const void *element)
{
	screen_queue_wait_vector_view_t *view =
		(screen_queue_wait_vector_view_t *)vector;
	int size_before = view != NULL ? view->size : 0;

	__real_CVector_push_back(vector, element);
	carbox_screen_queue_push_result(vector,
		view != NULL && view->size == size_before + 1);
}

void __wrap_CVector_erase(void *vector, int index)
{
	screen_queue_wait_vector_view_t *view =
		(screen_queue_wait_vector_view_t *)vector;

	__real_CVector_erase(vector, index);
	carbox_screen_queue_after_erase(vector,
		view != NULL ? view->size : 0);
}
#endif

#else

void carbox_screen_queue_publish_begin(void) { }
void carbox_screen_queue_publish_end(void) { }
void carbox_screen_queue_push_result(void *vector, int pushed)
{
	(void)vector;
	(void)pushed;
}
void carbox_screen_queue_after_erase(void *vector, int remaining)
{
	(void)vector;
	(void)remaining;
}
void carbox_screen_queue_wait(TickType_t timeout_ticks)
{
	vTaskDelay(timeout_ticks);
}
void carbox_screen_queue_wait_report(uint32_t sequence) { (void)sequence; }

#endif
