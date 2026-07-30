/*
 * Customer example: silent internal ATSS use with a fixed BSS buffer.
 *
 * This file is an integration example and is not added to the SDK build.
 * Call customer_atss_start() once, then call customer_atss_get_latest()
 * periodically after ATSS_SAMPLE_PERIOD_MS. Call customer_atss_stop() when
 * statistics are no longer needed.
 */

#include "atcmd_sys.h"

#define CUSTOMER_ATSS_MAX_TASKS 64U

static atss_task_stat_t customer_atss_stats[CUSTOMER_ATSS_MAX_TASKS];

int customer_atss_start(void)
{
	return atss_stats_start();
}

int customer_atss_get_latest(const atss_task_stat_t **stats,
			     size_t *task_count,
			     uint32_t *sequence)
{
	uint32_t sample_period_ms;
	int status;

	if (stats == NULL || task_count == NULL) {
		return ATSS_INVALID_ARGUMENT;
	}

	status = atss_stats_get(customer_atss_stats,
				CUSTOMER_ATSS_MAX_TASKS,
				task_count,
				sequence,
				&sample_period_ms);
	if (status != ATSS_OK) {
		*stats = NULL;
		return status;
	}

	/*
	 * All entries are returned by this one call:
	 * customer_atss_stats[0] through
	 * customer_atss_stats[*task_count - 1].
	 *
	 * Consume or copy them before calling this wrapper again.
	 */
	*stats = customer_atss_stats;
	return ATSS_OK;
}

int customer_atss_stop(void)
{
	return atss_stats_stop();
}

