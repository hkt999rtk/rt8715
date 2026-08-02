#ifndef __ATCMD_SYS_H__
#define __ATCMD_SYS_H__

#include <stddef.h>
#include <stdint.h>

/*
 * ATSS keeps the longer task name registered by the local FreeRTOS debug
 * registry. Keep this public type independent of FreeRTOS headers.
 */
#define ATSS_TASK_NAME_LEN       32U
#define ATSS_SAMPLE_PERIOD_MS    2000U

typedef struct atss_task_stat {
	char task_name[ATSS_TASK_NAME_LEN];
	uint32_t priority;
	union {
		uint32_t runtime_us;
		uint32_t runtime_ticks; /* Deprecated source-compatible alias. */
	};
	uint32_t cpu_utilization_x10;
	uint32_t stack_size_bytes;
	uint32_t stack_used_bytes;
	uint32_t stack_peak_bytes;
} atss_task_stat_t;

typedef enum atss_status {
	ATSS_OK = 0,
	ATSS_NOT_READY = 1,
	ATSS_BUFFER_TOO_SMALL = 2,
	ATSS_NOT_RUNNING = 3,
	ATSS_NO_MEMORY = 4,
	ATSS_INVALID_ARGUMENT = 5,
	ATSS_UNAVAILABLE = 6
} atss_status_t;

/*
 * Silent task-context API; do not call it from an ISR. The first sample becomes
 * available one sample period after start. atss_stats_get() always reports the
 * required entry count in task_count; pass stats == NULL/capacity == 0 to query
 * that count.
 */
int atss_stats_start(void);
int atss_stats_get(atss_task_stat_t *stats,
		   size_t capacity,
		   size_t *task_count,
		   uint32_t *sequence,
		   uint32_t *sample_period_ms);
int atss_stats_stop(void);

#endif
