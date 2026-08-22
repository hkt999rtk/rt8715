#include "airplay_mutex_profiler.h"

#ifndef CONFIG_AIRPLAY_MUTEX_PROFILE
#define CONFIG_AIRPLAY_MUTEX_PROFILE 0
#endif

#if CONFIG_AIRPLAY_MUTEX_PROFILE

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "FreeRTOS_POSIX/pthread.h"
#include "diag.h"
#include "hal_timer.h"

enum airplay_mutex_path {
	AIRPLAY_MUTEX_PATH_OTHER = 0,
	AIRPLAY_MUTEX_PATH_TOUCH = 1,
	AIRPLAY_MUTEX_PATH_COUNT
};

typedef struct airplay_mutex_metric_s {
	uint64_t wait_total_us;
	uint64_t hold_total_us;
	uint32_t calls;
	uint32_t errors;
	uint32_t wait_max_us;
	uint32_t hold_max_us;
	uint32_t wait_ge_10us;
	uint32_t wait_ge_100us;
	uint32_t wait_ge_1ms;
	uint32_t wait_ge_5ms;
	uint32_t wait_ge_10ms;
	uint32_t hold_ge_1ms;
	uint32_t hold_ge_5ms;
	uint32_t hold_ge_10ms;
} airplay_mutex_metric_t;

typedef struct airplay_mutex_state_s {
	pthread_mutex_t *target;
	TaskHandle_t discovery_task;
	uint32_t discovery_depth;
	TaskHandle_t touch_task;
	uint32_t touch_depth;
	TaskHandle_t owner_task;
	uint32_t owner_acquired_us;
	uint8_t owner_path;
	TaskHandle_t max_waiter_task;
	TaskHandle_t max_wait_owner_task;
	uint8_t max_waiter_path;
	uint8_t max_wait_owner_path;
	uint32_t max_wait_us;
	uint32_t discovered;
	uint32_t unlock_mismatch;
	uint32_t owner_unknown;
} airplay_mutex_state_t;

static airplay_mutex_state_t airplay_mutex_state
	__attribute__((section(".lpddr.bss.airplay_mutex_state")));
static airplay_mutex_metric_t airplay_mutex_metrics[AIRPLAY_MUTEX_PATH_COUNT]
	__attribute__((section(".lpddr.bss.airplay_mutex_metrics")));

extern int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
extern int __real_pthread_mutex_unlock(pthread_mutex_t *mutex);
extern void __real_lib_carplay_hid_report(
	uint32_t uid, const void *report, uint32_t length);

static uint8_t airplay_mutex_current_path(TaskHandle_t task)
{
	if ((task != NULL) && (task == airplay_mutex_state.touch_task) &&
	    (airplay_mutex_state.touch_depth != 0U)) {
		return AIRPLAY_MUTEX_PATH_TOUCH;
	}
	return AIRPLAY_MUTEX_PATH_OTHER;
}

static const char *airplay_mutex_path_name(uint8_t path)
{
	return path == AIRPLAY_MUTEX_PATH_TOUCH ? "TOUCH" : "OTHER";
}

static const char *airplay_mutex_task_name(TaskHandle_t task)
{
	return task != NULL ? pcTaskGetTaskName(task) : "none";
}

static void airplay_mutex_metric_wait(airplay_mutex_metric_t *metric,
	uint32_t wait_us)
{
	metric->calls++;
	metric->wait_total_us += wait_us;
	if (wait_us > metric->wait_max_us) {
		metric->wait_max_us = wait_us;
	}
	metric->wait_ge_10us += wait_us >= 10U;
	metric->wait_ge_100us += wait_us >= 100U;
	metric->wait_ge_1ms += wait_us >= 1000U;
	metric->wait_ge_5ms += wait_us >= 5000U;
	metric->wait_ge_10ms += wait_us >= 10000U;
}

static void airplay_mutex_metric_hold(airplay_mutex_metric_t *metric,
	uint32_t hold_us)
{
	metric->hold_total_us += hold_us;
	if (hold_us > metric->hold_max_us) {
		metric->hold_max_us = hold_us;
	}
	metric->hold_ge_1ms += hold_us >= 1000U;
	metric->hold_ge_5ms += hold_us >= 5000U;
	metric->hold_ge_10ms += hold_us >= 10000U;
}

void airplay_mutex_profiler_touch_enter(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();

	taskENTER_CRITICAL();
	if ((airplay_mutex_state.touch_depth == 0U) ||
	    (airplay_mutex_state.touch_task == current)) {
		airplay_mutex_state.touch_task = current;
		airplay_mutex_state.touch_depth++;
	}
	if (airplay_mutex_state.target == NULL) {
		airplay_mutex_state.discovery_task = current;
		airplay_mutex_state.discovery_depth++;
	}
	taskEXIT_CRITICAL();
}

void airplay_mutex_profiler_touch_leave(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();

	taskENTER_CRITICAL();
	if ((airplay_mutex_state.touch_task == current) &&
	    (airplay_mutex_state.touch_depth != 0U)) {
		airplay_mutex_state.touch_depth--;
		if (airplay_mutex_state.touch_depth == 0U) {
			airplay_mutex_state.touch_task = NULL;
		}
	}
	if ((airplay_mutex_state.discovery_task == current) &&
	    (airplay_mutex_state.discovery_depth != 0U)) {
		airplay_mutex_state.discovery_depth--;
		if (airplay_mutex_state.discovery_depth == 0U) {
			airplay_mutex_state.discovery_task = NULL;
		}
	}
	taskEXIT_CRITICAL();
}

/* The vehicle-event path enters the vendor archive through this raw HID API,
 * not lib_carplay_touch().  Its first operation is lock_airplay(), making it
 * the reliable discovery bracket for the private airplay_mutex. */
void __wrap_lib_carplay_hid_report(
	uint32_t uid, const void *report, uint32_t length)
{
	airplay_mutex_profiler_touch_enter();
	__real_lib_carplay_hid_report(uid, report, length);
	airplay_mutex_profiler_touch_leave();
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	TaskHandle_t owner_at_request = NULL;
	uint8_t owner_path_at_request = AIRPLAY_MUTEX_PATH_OTHER;
	uint8_t path;
	uint32_t start_us;
	uint32_t wait_us;
	int result;

	/* Before the first touch this wrapper sees every POSIX mutex in the
	 * process. Keep that path to two loads unless discovery is armed. */
	if ((airplay_mutex_state.target == NULL) &&
	    (airplay_mutex_state.discovery_depth != 0U) &&
	    (airplay_mutex_state.discovery_task == current)) {
		taskENTER_CRITICAL();
		if ((airplay_mutex_state.target == NULL) &&
		    (airplay_mutex_state.discovery_depth != 0U) &&
		    (airplay_mutex_state.discovery_task == current)) {
			airplay_mutex_state.target = mutex;
			airplay_mutex_state.discovered++;
		}
		taskEXIT_CRITICAL();
	}

	if (mutex != airplay_mutex_state.target) {
		return __real_pthread_mutex_lock(mutex);
	}

	start_us = hal_read_curtime_us();
	taskENTER_CRITICAL();
	path = airplay_mutex_current_path(current);
	owner_at_request = airplay_mutex_state.owner_task;
	owner_path_at_request = airplay_mutex_state.owner_path;
	taskEXIT_CRITICAL();

	result = __real_pthread_mutex_lock(mutex);
	wait_us = hal_read_curtime_us() - start_us;

	taskENTER_CRITICAL();
	if (result == 0) {
		airplay_mutex_metric_wait(&airplay_mutex_metrics[path], wait_us);
		airplay_mutex_state.owner_task = current;
		airplay_mutex_state.owner_path = path;
		airplay_mutex_state.owner_acquired_us = hal_read_curtime_us();
		if (wait_us > airplay_mutex_state.max_wait_us) {
			airplay_mutex_state.max_wait_us = wait_us;
			airplay_mutex_state.max_waiter_task = current;
			airplay_mutex_state.max_wait_owner_task = owner_at_request;
			airplay_mutex_state.max_waiter_path = path;
			airplay_mutex_state.max_wait_owner_path =
				owner_path_at_request;
		}
		if ((wait_us >= 100U) && (owner_at_request == NULL)) {
			airplay_mutex_state.owner_unknown++;
		}
	} else {
		airplay_mutex_metrics[path].errors++;
	}
	taskEXIT_CRITICAL();

	return result;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t now_us;
	uint32_t hold_us;
	uint8_t path;

	if (mutex != airplay_mutex_state.target) {
		return __real_pthread_mutex_unlock(mutex);
	}

	now_us = hal_read_curtime_us();
	taskENTER_CRITICAL();
	if (airplay_mutex_state.owner_task == current) {
		path = airplay_mutex_state.owner_path;
		hold_us = now_us - airplay_mutex_state.owner_acquired_us;
		airplay_mutex_metric_hold(&airplay_mutex_metrics[path], hold_us);
		/* Clear before releasing. A newly awakened owner must not be erased by
		 * this task after the real unlock returns. */
		airplay_mutex_state.owner_task = NULL;
		airplay_mutex_state.owner_acquired_us = 0U;
	} else {
		airplay_mutex_state.unlock_mismatch++;
	}
	taskEXIT_CRITICAL();

	return __real_pthread_mutex_unlock(mutex);
}

void airplay_mutex_profiler_report(uint32_t sequence)
{
	airplay_mutex_metric_t metric[AIRPLAY_MUTEX_PATH_COUNT];
	airplay_mutex_state_t state;
	unsigned i;

	taskENTER_CRITICAL();
	memcpy(metric, airplay_mutex_metrics, sizeof(metric));
	memset(airplay_mutex_metrics, 0, sizeof(airplay_mutex_metrics));
	state = airplay_mutex_state;
	airplay_mutex_state.max_wait_us = 0U;
	airplay_mutex_state.max_waiter_task = NULL;
	airplay_mutex_state.max_wait_owner_task = NULL;
	airplay_mutex_state.owner_unknown = 0U;
	airplay_mutex_state.unlock_mismatch = 0U;
	taskEXIT_CRITICAL();

	rt_printf("[AIRPLAYMUTEX][%lu] target=%p discovered=%lu owner=%s/%s "
		"max_wait=%luus waiter=%s/%s blocked_by=%s/%s unknown=%lu mismatch=%lu\n",
		(unsigned long)sequence, state.target,
		(unsigned long)state.discovered,
		airplay_mutex_task_name(state.owner_task),
		airplay_mutex_path_name(state.owner_path),
		(unsigned long)state.max_wait_us,
		airplay_mutex_task_name(state.max_waiter_task),
		airplay_mutex_path_name(state.max_waiter_path),
		airplay_mutex_task_name(state.max_wait_owner_task),
		airplay_mutex_path_name(state.max_wait_owner_path),
		(unsigned long)state.owner_unknown,
		(unsigned long)state.unlock_mismatch);

	for (i = 0U; i < AIRPLAY_MUTEX_PATH_COUNT; ++i) {
		uint32_t calls = metric[i].calls;
		rt_printf("[AIRPLAYMUTEX][%lu][%s] calls/error=%lu/%lu "
			"wait_us avg/max=%lu/%lu ge10/100us/1/5/10ms=%lu/%lu/%lu/%lu/%lu "
			"hold_us avg/max=%lu/%lu ge1/5/10ms=%lu/%lu/%lu\n",
			(unsigned long)sequence, airplay_mutex_path_name((uint8_t)i),
			(unsigned long)calls, (unsigned long)metric[i].errors,
			(unsigned long)(calls ? metric[i].wait_total_us / calls : 0U),
			(unsigned long)metric[i].wait_max_us,
			(unsigned long)metric[i].wait_ge_10us,
			(unsigned long)metric[i].wait_ge_100us,
			(unsigned long)metric[i].wait_ge_1ms,
			(unsigned long)metric[i].wait_ge_5ms,
			(unsigned long)metric[i].wait_ge_10ms,
			(unsigned long)(calls ? metric[i].hold_total_us / calls : 0U),
			(unsigned long)metric[i].hold_max_us,
			(unsigned long)metric[i].hold_ge_1ms,
			(unsigned long)metric[i].hold_ge_5ms,
			(unsigned long)metric[i].hold_ge_10ms);
	}
}

#else

void airplay_mutex_profiler_touch_enter(void) {}
void airplay_mutex_profiler_touch_leave(void) {}
void airplay_mutex_profiler_report(uint32_t sequence) { (void)sequence; }

#endif
