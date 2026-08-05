#include "gcd_sync_profiler.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "hal_timer.h"

#ifndef CONFIG_GCD_SYNC_PROFILE
#define CONFIG_GCD_SYNC_PROFILE 0
#endif

#ifndef CONFIG_GCD_WORK_PRIORITY
#define CONFIG_GCD_WORK_PRIORITY (-1)
#endif

#if CONFIG_GCD_WORK_PRIORITY >= 0

extern BaseType_t __real_xTaskCreate(TaskFunction_t task_code,
				     const char *task_name,
				     configSTACK_DEPTH_TYPE stack_depth,
				     void *parameters,
				     UBaseType_t priority,
				     TaskHandle_t *created_task);

static int gcdprof_is_worker_name(const char *name)
{
	static const char expected[] = "gcd-work";
	uint32_t i;

	if (name == NULL) {
		return 0;
	}
	for (i = 0U; i < sizeof(expected); i++) {
		if (name[i] != expected[i]) {
			return 0;
		}
	}
	return 1;
}

/*
 * DispatchLite creates its workers through FreeRTOS-Plus-POSIX and requests
 * priority 2.  Its binary has no source, so alter only tasks named gcd-work at
 * the final FreeRTOS creation boundary.  Every other xTaskCreate call is passed
 * through unchanged.  This is intentionally a build-time A/B test switch.
 */
BaseType_t __wrap_xTaskCreate(TaskFunction_t task_code,
			      const char *task_name,
			      configSTACK_DEPTH_TYPE stack_depth,
			      void *parameters,
			      UBaseType_t priority,
			      TaskHandle_t *created_task)
{
	if (gcdprof_is_worker_name(task_name)) {
		priority = (UBaseType_t)CONFIG_GCD_WORK_PRIORITY;
		if (priority >= (UBaseType_t)configMAX_PRIORITIES) {
			priority = (UBaseType_t)configMAX_PRIORITIES - 1U;
		}
	}
	return __real_xTaskCreate(task_code, task_name, stack_depth, parameters,
				  priority, created_task);
}

#endif /* CONFIG_GCD_WORK_PRIORITY >= 0 */

#if CONFIG_GCD_SYNC_PROFILE

/*
 * DispatchLite is supplied in lib_CarPlay.a without source.  Intercepting its
 * public synchronous-dispatch entry point lets us distinguish time waiting for
 * a gcd-work thread from time spent in the submitted callback.  Do not replace
 * this timer with DWT CYCCNT: at 350 MHz its 32-bit counter wraps in about 12 s,
 * while a blocked synchronous dispatch is allowed to last longer.  Unsigned
 * subtraction of this 1-us system timer remains valid for intervals below its
 * approximately 71-minute wrap period.
 */
#define GCDPROF_MAX_CALLBACKS       32U
#define GCDPROF_REPORT_CALLBACKS     8U

typedef void (*gcdprof_function_t)(void *context);

typedef struct gcdprof_call_s {
	void *original_context;
	gcdprof_function_t original_function;
	volatile uint32_t callback_start_us;
	volatile uint32_t callback_end_us;
	volatile uint32_t callback_called;
} gcdprof_call_t;

typedef struct gcdprof_entry_s {
	uint32_t queue;
	uint32_t function;
	uint32_t calls;
	uint64_t wait_sum_us;
	uint64_t exec_sum_us;
	uint64_t total_sum_us;
	uint32_t wait_max_us;
	uint32_t exec_max_us;
	uint32_t total_max_us;
	uint32_t wait_ge_1ms;
	uint32_t wait_ge_5ms;
	uint32_t wait_ge_20ms;
	uint32_t wait_ge_100ms;
} gcdprof_entry_t;

typedef struct gcdprof_snapshot_s {
	gcdprof_entry_t callbacks[GCDPROF_MAX_CALLBACKS];
	gcdprof_entry_t total;
	uint32_t callback_count;
	uint32_t callback_overflow;
	uint32_t callback_missing;
} gcdprof_snapshot_t;

static gcdprof_snapshot_t gcdprof_live
	__attribute__((section(".lpddr.bss.gcdprof")));
static gcdprof_snapshot_t gcdprof_report_copy
	__attribute__((section(".lpddr.bss.gcdprof")));
static volatile uint32_t gcdprof_in_flight;
static volatile uint32_t gcdprof_in_flight_peak;

extern void __real_dispatch_sync_f(void *queue, void *context,
				   gcdprof_function_t function);

static void gcdprof_update_entry(gcdprof_entry_t *entry, uint32_t queue,
				 uint32_t function,
				 uint32_t wait_us, uint32_t exec_us,
				 uint32_t total_us)
{
	entry->queue = queue;
	entry->function = function;
	entry->calls++;
	entry->wait_sum_us += wait_us;
	entry->exec_sum_us += exec_us;
	entry->total_sum_us += total_us;
	if (wait_us > entry->wait_max_us) {
		entry->wait_max_us = wait_us;
	}
	if (exec_us > entry->exec_max_us) {
		entry->exec_max_us = exec_us;
	}
	if (total_us > entry->total_max_us) {
		entry->total_max_us = total_us;
	}
	entry->wait_ge_1ms += (wait_us >= 1000U);
	entry->wait_ge_5ms += (wait_us >= 5000U);
	entry->wait_ge_20ms += (wait_us >= 20000U);
	entry->wait_ge_100ms += (wait_us >= 100000U);
}

static void gcdprof_trampoline(void *context)
{
	gcdprof_call_t *call = (gcdprof_call_t *)context;

	call->callback_start_us = hal_read_curtime_us();
	call->callback_called = 1U;
	call->original_function(call->original_context);
	call->callback_end_us = hal_read_curtime_us();
}

void __wrap_dispatch_sync_f(void *queue, void *context,
			    gcdprof_function_t function)
{
	gcdprof_call_t call;
	uint32_t submit_us;
	uint32_t return_us;
	uint32_t wait_us;
	uint32_t exec_us;
	uint32_t total_us;
	uint32_t normalized_function;
	uint32_t normalized_queue;
	uint32_t i;
	gcdprof_entry_t *entry = NULL;

	/* Preserve the original API's behaviour for an invalid callback. */
	if (function == NULL) {
		__real_dispatch_sync_f(queue, context, function);
		return;
	}

	call.original_context = context;
	call.original_function = function;
	call.callback_start_us = 0U;
	call.callback_end_us = 0U;
	call.callback_called = 0U;
	taskENTER_CRITICAL();
	gcdprof_in_flight++;
	if (gcdprof_in_flight > gcdprof_in_flight_peak) {
		gcdprof_in_flight_peak = gcdprof_in_flight;
	}
	taskEXIT_CRITICAL();
	submit_us = hal_read_curtime_us();
	__real_dispatch_sync_f(queue, &call, gcdprof_trampoline);
	return_us = hal_read_curtime_us();

	if (call.callback_called == 0U) {
		taskENTER_CRITICAL();
		gcdprof_in_flight--;
		gcdprof_live.callback_missing++;
		taskEXIT_CRITICAL();
		return;
	}

	wait_us = call.callback_start_us - submit_us;
	exec_us = call.callback_end_us - call.callback_start_us;
	total_us = return_us - submit_us;
	normalized_function = ((uint32_t)(uintptr_t)function) & ~1U;
	normalized_queue = (uint32_t)(uintptr_t)queue;

	/* Updates are short and infrequent compared with callback execution. */
	taskENTER_CRITICAL();
	for (i = 0U; i < gcdprof_live.callback_count; i++) {
		if ((gcdprof_live.callbacks[i].function == normalized_function) &&
		    (gcdprof_live.callbacks[i].queue == normalized_queue)) {
			entry = &gcdprof_live.callbacks[i];
			break;
		}
	}
	if ((entry == NULL) &&
	    (gcdprof_live.callback_count < GCDPROF_MAX_CALLBACKS)) {
		entry = &gcdprof_live.callbacks[gcdprof_live.callback_count++];
	}
	if (entry != NULL) {
		gcdprof_update_entry(entry, normalized_queue, normalized_function,
				     wait_us, exec_us,
				     total_us);
	} else {
		gcdprof_live.callback_overflow++;
	}
	gcdprof_update_entry(&gcdprof_live.total, 0U, 0U, wait_us, exec_us,
			     total_us);
	gcdprof_in_flight--;
	taskEXIT_CRITICAL();
}

static uint32_t gcdprof_average(uint64_t total, uint32_t count)
{
	return (count != 0U) ? (uint32_t)(total / count) : 0U;
}

static int gcdprof_more_wait(const gcdprof_entry_t *left,
			     const gcdprof_entry_t *right)
{
	/* Rank callbacks by accumulated queue delay, then by worst delay. */
	if (left->wait_sum_us != right->wait_sum_us) {
		return left->wait_sum_us > right->wait_sum_us;
	}
	return left->wait_max_us > right->wait_max_us;
}

void gcd_sync_profiler_report(uint32_t sequence)
{
	gcdprof_entry_t *total;
	uint32_t print_count;
	uint32_t i;
	uint32_t j;
	uint32_t in_flight;
	uint32_t in_flight_peak;

	taskENTER_CRITICAL();
	memcpy(&gcdprof_report_copy, &gcdprof_live, sizeof(gcdprof_report_copy));
	memset(&gcdprof_live, 0, sizeof(gcdprof_live));
	in_flight = gcdprof_in_flight;
	in_flight_peak = gcdprof_in_flight_peak;
	gcdprof_in_flight_peak = in_flight;
	taskEXIT_CRITICAL();

	total = &gcdprof_report_copy.total;
	rt_printf("[GCDPROF][%lu] window_ms=10000 worker_prio=%d sync=%lu callbacks=%lu "
		  "in_flight=%lu peak=%lu bucket_overflow=%lu missing=%lu\r\n",
		  (unsigned long)sequence, CONFIG_GCD_WORK_PRIORITY,
		  (unsigned long)total->calls,
		  (unsigned long)gcdprof_report_copy.callback_count,
		  (unsigned long)in_flight, (unsigned long)in_flight_peak,
		  (unsigned long)gcdprof_report_copy.callback_overflow,
		  (unsigned long)gcdprof_report_copy.callback_missing);
	if (total->calls == 0U) {
		return;
	}

	rt_printf("[GCDPROF][%lu] wait_us avg/max=%lu/%lu exec_us avg/max=%lu/%lu "
		  "total_us avg/max=%lu/%lu wait_ge_ms 1/5/20/100=%lu/%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)gcdprof_average(total->wait_sum_us, total->calls),
		  (unsigned long)total->wait_max_us,
		  (unsigned long)gcdprof_average(total->exec_sum_us, total->calls),
		  (unsigned long)total->exec_max_us,
		  (unsigned long)gcdprof_average(total->total_sum_us, total->calls),
		  (unsigned long)total->total_max_us,
		  (unsigned long)total->wait_ge_1ms,
		  (unsigned long)total->wait_ge_5ms,
		  (unsigned long)total->wait_ge_20ms,
		  (unsigned long)total->wait_ge_100ms);

	/* A small in-place selection sort keeps the diagnostic independent of libc qsort. */
	for (i = 0U; i < gcdprof_report_copy.callback_count; i++) {
		uint32_t best = i;
		for (j = i + 1U; j < gcdprof_report_copy.callback_count; j++) {
			if (gcdprof_more_wait(&gcdprof_report_copy.callbacks[j],
					      &gcdprof_report_copy.callbacks[best])) {
				best = j;
			}
		}
		if (best != i) {
			gcdprof_entry_t temporary = gcdprof_report_copy.callbacks[i];
			gcdprof_report_copy.callbacks[i] =
				gcdprof_report_copy.callbacks[best];
			gcdprof_report_copy.callbacks[best] = temporary;
		}
	}

	print_count = gcdprof_report_copy.callback_count;
	if (print_count > GCDPROF_REPORT_CALLBACKS) {
		print_count = GCDPROF_REPORT_CALLBACKS;
	}
	for (i = 0U; i < print_count; i++) {
		gcdprof_entry_t *entry = &gcdprof_report_copy.callbacks[i];
		rt_printf("[GCDPROF][%lu][CB] #%02lu q=0x%08lx fn=0x%08lx calls=%lu "
			  "wait_avg/max=%lu/%lu exec_avg/max=%lu/%lu "
			  "total_avg/max=%lu/%lu ge_ms=%lu/%lu/%lu/%lu\r\n",
			  (unsigned long)sequence, (unsigned long)(i + 1U),
			  (unsigned long)entry->queue,
			  (unsigned long)entry->function,
			  (unsigned long)entry->calls,
			  (unsigned long)gcdprof_average(entry->wait_sum_us,
							entry->calls),
			  (unsigned long)entry->wait_max_us,
			  (unsigned long)gcdprof_average(entry->exec_sum_us,
							entry->calls),
			  (unsigned long)entry->exec_max_us,
			  (unsigned long)gcdprof_average(entry->total_sum_us,
							entry->calls),
			  (unsigned long)entry->total_max_us,
			  (unsigned long)entry->wait_ge_1ms,
			  (unsigned long)entry->wait_ge_5ms,
			  (unsigned long)entry->wait_ge_20ms,
			  (unsigned long)entry->wait_ge_100ms);
	}
}

#else

void gcd_sync_profiler_report(uint32_t sequence)
{
	(void)sequence;
}

#endif /* CONFIG_GCD_SYNC_PROFILE */
