#ifndef CARBOX_SCREEN_TCP_WRITE_PROFILER_H
#define CARBOX_SCREEN_TCP_WRITE_PROFILER_H

#include <stddef.h>
#include <stdint.h>

#if defined(CONFIG_SCREEN_TCP_WRITE_PHASE_PROFILE) && \
	CONFIG_SCREEN_TCP_WRITE_PHASE_PROFILE
#include "hal_timer.h"

static inline uint32_t carbox_screen_tcp_write_now_us(void)
{
	return hal_read_curtime_us();
}

void carbox_screen_tcp_write_profile_record(size_t bytes, int error,
	uint32_t submit_us, uint32_t tcpip_start_us,
	uint32_t tcp_write_us, uint32_t tcp_output_us,
	uint32_t signal_us, uint32_t return_us,
	uint16_t tcp_write_calls, uint16_t tcp_output_calls);
void carbox_screen_tcp_write_profile_report(uint32_t sequence);
#else
static inline void carbox_screen_tcp_write_profile_report(uint32_t sequence)
{
	(void)sequence;
}
#endif

#endif /* CARBOX_SCREEN_TCP_WRITE_PROFILER_H */
