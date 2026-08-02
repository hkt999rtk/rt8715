#include <stdint.h>

#include <hal_timer.h>

static uint32_t atss_runtime_counter_base_us;

void atss_runtime_counter_init(void)
{
	/*
	 * The platform system GTimer is already configured as a 1 us free-running
	 * counter before the scheduler starts. Keep a logical zero point so boot
	 * time is not charged to the first FreeRTOS task.
	 */
	atss_runtime_counter_base_us = hal_read_curtime_us();
}

uint32_t atss_runtime_counter_get(void)
{
	/*
	 * Unsigned subtraction also works when the raw 32-bit microsecond counter
	 * wraps. The returned value still wraps every ~71.6 minutes; FreeRTOS and
	 * ATSS consume it using the same modulo arithmetic.
	 */
	return hal_read_curtime_us() - atss_runtime_counter_base_us;
}
