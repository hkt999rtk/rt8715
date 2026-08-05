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

#if CONFIG_SCREEN_QUEUE_PROFILE

#define SCREENPROF_ACTIVE_TASKS       4U
#define SCREENPROF_FRAME_TIMES      256U

typedef struct screenprof_frame_s {
	uint32_t enqueue_us;
	uint32_t bytes;
} screenprof_frame_t;

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

	uint32_t write_calls;
	uint64_t write_request_bytes;
	uint64_t write_return_bytes;
	uint64_t write_time_sum_us;
	uint32_t write_time_max_us;
	uint32_t write_partial;
	uint32_t write_error;
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
static uint32_t screenprof_frame_head;
static uint32_t screenprof_frame_count;
static int screenprof_frame_tracking_invalid;
static void *screenprof_vector;
static TaskHandle_t screenprof_active_tasks[SCREENPROF_ACTIVE_TASKS];
static TaskHandle_t screenprof_receiver_task;
static TaskHandle_t screenprof_sender_task;

extern void __real_AirPlayScreen_SendVideo(const void *data, int bytes);
extern void __real_CVector_push_back(void *vector, const void *element);
extern void __real_CVector_erase(void *vector, int index);
extern void __real_CVector_delete(void *vector);
extern ssize_t __real_lwip_recv(int socket, void *buffer, size_t bytes,
				int flags);
extern ssize_t __real_lwip_write(int socket, const void *buffer, size_t bytes);

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
			break;
		}
	}
	taskEXIT_CRITICAL();
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

void __wrap_AirPlayScreen_SendVideo(const void *data, int bytes)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	int marked = screenprof_mark_active(current);

	taskENTER_CRITICAL();
	screenprof_live.video_calls++;
	if (bytes > 0) {
		screenprof_live.video_bytes += (uint32_t)bytes;
	}
	taskEXIT_CRITICAL();
	__real_AirPlayScreen_SendVideo(data, bytes);
	if (marked) {
		screenprof_unmark_active(current);
	}
}

void __wrap_CVector_push_back(void *vector, const void *element)
{
	screenprof_frame_item_t item = { NULL, 0 };
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
			screenprof_frames[tail].enqueue_us = now_us;
			screenprof_frames[tail].bytes =
				(item.bytes > 0) ? (uint32_t)item.bytes : 0U;
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
			age_us = now_us - frame->enqueue_us;
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

ssize_t __wrap_lwip_recv(int socket, void *buffer, size_t bytes, int flags)
{
	int measured = screenprof_is_task(&screenprof_receiver_task,
					  "AirPlayScreenReceiver");
	uint32_t start_us = measured ? hal_read_curtime_us() : 0U;
	ssize_t result = __real_lwip_recv(socket, buffer, bytes, flags);

	if (measured) {
		uint32_t elapsed_us = hal_read_curtime_us() - start_us;
		taskENTER_CRITICAL();
		screenprof_live.recv_calls++;
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
		taskEXIT_CRITICAL();
	}
	return result;
}

ssize_t __wrap_lwip_write(int socket, const void *buffer, size_t bytes)
{
	int measured = screenprof_is_task(&screenprof_sender_task, "ScreenThread");
	uint32_t start_us = measured ? hal_read_curtime_us() : 0U;
	ssize_t result = __real_lwip_write(socket, buffer, bytes);

	if (measured) {
		uint32_t elapsed_us = hal_read_curtime_us() - start_us;
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
		taskEXIT_CRITICAL();
	}
	return result;
}

static uint32_t screenprof_average(uint64_t total, uint32_t count)
{
	return (count != 0U) ? (uint32_t)(total / count) : 0U;
}

void screen_queue_profiler_report(uint32_t sequence)
{
	uint32_t current_depth;
	uint32_t vector_depth = 0U;
	int age_valid;
	void *vector;

	taskENTER_CRITICAL();
	screenprof_copy = screenprof_live;
	screenprof_live = (screenprof_stats_t){ 0 };
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
