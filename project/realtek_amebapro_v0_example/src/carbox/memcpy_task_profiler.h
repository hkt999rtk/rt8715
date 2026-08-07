#ifndef CARBOX_MEMCPY_TASK_PROFILER_H
#define CARBOX_MEMCPY_TASK_PROFILER_H

#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CARBOX_MEMCPY_PROFILE_TARGETS 2U

enum carbox_memop_kind {
	CARBOX_MEMOP_MEMCPY = 0,
	CARBOX_MEMOP_MEMMOVE,
	CARBOX_MEMOP_MEMSET,
	CARBOX_MEMOP_COUNT
};

/* Read directly by the ITCM memcpy wrapper to keep the disabled/non-target
 * path to two pointer comparisons. NULL targets are never recorded. */
extern void * volatile
	carbox_memcpy_profile_targets[CARBOX_MEMCPY_PROFILE_TARGETS];

void carbox_memcpy_task_profiler_record(void *task, uintptr_t caller,
					const void *dst, const void *src,
					size_t len, unsigned int operation,
					uint32_t cycles, int cycles_valid);
void carbox_memcpy_task_profiler_set_targets(TaskHandle_t screen,
					     TaskHandle_t receiver);
void carbox_memcpy_task_profiler_report(uint32_t sequence,
					uint32_t window_ms);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_MEMCPY_TASK_PROFILER_H */
