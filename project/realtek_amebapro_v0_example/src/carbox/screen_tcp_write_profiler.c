#include "screen_tcp_write_profiler.h"

#if defined(CONFIG_SCREEN_TCP_WRITE_PHASE_PROFILE) && \
	CONFIG_SCREEN_TCP_WRITE_PHASE_PROFILE

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"

#include <string.h>

typedef struct screen_tcp_write_phase_stats_s {
	uint32_t calls;
	uint32_t errors;
	uint32_t invalid;
	uint64_t bytes;
	uint64_t total_us;
	uint32_t total_max_us;
	uint64_t queue_us;
	uint32_t queue_max_us;
	uint64_t tcp_write_us;
	uint32_t tcp_write_max_us;
	uint64_t tcp_output_us;
	uint32_t tcp_output_max_us;
	uint64_t core_other_us;
	uint32_t core_other_max_us;
	uint64_t wake_us;
	uint32_t wake_max_us;
	uint64_t tcp_write_calls;
	uint32_t tcp_write_calls_max;
	uint64_t tcp_output_calls;
	uint32_t tcp_output_calls_max;
} screen_tcp_write_phase_stats_t;

static screen_tcp_write_phase_stats_t screen_tcp_write_stats
	__attribute__((section(".lpddr.bss.screen_tcp_write_phase_stats")));

static void screen_tcp_write_update_max(uint32_t *maximum, uint32_t value)
{
	if (value > *maximum) {
		*maximum = value;
	}
}

void carbox_screen_tcp_write_profile_record(size_t bytes, int error,
	uint32_t submit_us, uint32_t tcpip_start_us,
	uint32_t tcp_write_us, uint32_t tcp_output_us,
	uint32_t signal_us, uint32_t return_us,
	uint16_t tcp_write_calls, uint16_t tcp_output_calls)
{
	uint32_t total_us;
	uint32_t queue_us;
	uint32_t core_us;
	uint32_t core_other_us;
	uint32_t wake_us;

	if ((submit_us == 0U) || (tcpip_start_us == 0U) ||
	    (signal_us == 0U) || (return_us == 0U)) {
		taskENTER_CRITICAL();
		screen_tcp_write_stats.invalid++;
		taskEXIT_CRITICAL();
		return;
	}

	total_us = return_us - submit_us;
	queue_us = tcpip_start_us - submit_us;
	core_us = signal_us - tcpip_start_us;
	wake_us = return_us - signal_us;
	if ((tcp_write_us + tcp_output_us) > core_us) {
		taskENTER_CRITICAL();
		screen_tcp_write_stats.invalid++;
		taskEXIT_CRITICAL();
		return;
	}
	core_other_us = core_us - tcp_write_us - tcp_output_us;

	taskENTER_CRITICAL();
	screen_tcp_write_stats.calls++;
	if (error != 0) {
		screen_tcp_write_stats.errors++;
	}
	screen_tcp_write_stats.bytes += bytes;
	screen_tcp_write_stats.total_us += total_us;
	screen_tcp_write_update_max(&screen_tcp_write_stats.total_max_us, total_us);
	screen_tcp_write_stats.queue_us += queue_us;
	screen_tcp_write_update_max(&screen_tcp_write_stats.queue_max_us, queue_us);
	screen_tcp_write_stats.tcp_write_us += tcp_write_us;
	screen_tcp_write_update_max(&screen_tcp_write_stats.tcp_write_max_us,
				    tcp_write_us);
	screen_tcp_write_stats.tcp_output_us += tcp_output_us;
	screen_tcp_write_update_max(&screen_tcp_write_stats.tcp_output_max_us,
				    tcp_output_us);
	screen_tcp_write_stats.core_other_us += core_other_us;
	screen_tcp_write_update_max(&screen_tcp_write_stats.core_other_max_us,
				    core_other_us);
	screen_tcp_write_stats.wake_us += wake_us;
	screen_tcp_write_update_max(&screen_tcp_write_stats.wake_max_us, wake_us);
	screen_tcp_write_stats.tcp_write_calls += tcp_write_calls;
	screen_tcp_write_update_max(&screen_tcp_write_stats.tcp_write_calls_max,
				    tcp_write_calls);
	screen_tcp_write_stats.tcp_output_calls += tcp_output_calls;
	screen_tcp_write_update_max(&screen_tcp_write_stats.tcp_output_calls_max,
				    tcp_output_calls);
	taskEXIT_CRITICAL();
}

void carbox_screen_tcp_write_profile_report(uint32_t sequence)
{
	screen_tcp_write_phase_stats_t stats;
	uint32_t calls;

	taskENTER_CRITICAL();
	stats = screen_tcp_write_stats;
	memset(&screen_tcp_write_stats, 0, sizeof(screen_tcp_write_stats));
	taskEXIT_CRITICAL();

	if ((stats.calls == 0U) && (stats.invalid == 0U)) {
		return;
	}
	calls = stats.calls;
	rt_printf("[TCPWRITEPHASE][%lu] calls/error/invalid=%lu/%lu/%lu bytes=%llu "
		  "total_us avg/max=%llu/%lu queue_to_tcpip=%llu/%lu "
		  "tcp_write_owned=%llu/%lu tcp_output=%llu/%lu "
		  "core_other=%llu/%lu wake_to_caller=%llu/%lu "
		  "inner_calls write/output avg:max=%llu:%lu/%llu:%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.calls,
		  (unsigned long)stats.errors,
		  (unsigned long)stats.invalid,
		  (unsigned long long)stats.bytes,
		  (unsigned long long)(calls != 0U ? stats.total_us / calls : 0U),
		  (unsigned long)stats.total_max_us,
		  (unsigned long long)(calls != 0U ? stats.queue_us / calls : 0U),
		  (unsigned long)stats.queue_max_us,
		  (unsigned long long)(calls != 0U ? stats.tcp_write_us / calls : 0U),
		  (unsigned long)stats.tcp_write_max_us,
		  (unsigned long long)(calls != 0U ? stats.tcp_output_us / calls : 0U),
		  (unsigned long)stats.tcp_output_max_us,
		  (unsigned long long)(calls != 0U ? stats.core_other_us / calls : 0U),
		  (unsigned long)stats.core_other_max_us,
		  (unsigned long long)(calls != 0U ? stats.wake_us / calls : 0U),
		  (unsigned long)stats.wake_max_us,
		  (unsigned long long)(calls != 0U ? stats.tcp_write_calls / calls : 0U),
		  (unsigned long)stats.tcp_write_calls_max,
		  (unsigned long long)(calls != 0U ? stats.tcp_output_calls / calls : 0U),
		  (unsigned long)stats.tcp_output_calls_max);
}

#endif
