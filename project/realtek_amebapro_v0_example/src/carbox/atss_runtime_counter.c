#include <stdint.h>

#include "FreeRTOS.h"
#include "cmsis.h"

static uint32_t atss_runtime_counter_base_cycles;

void atss_runtime_counter_init(void)
{
	/*
	 * DWT CYCCNT is shared with the network latency profiler.  Enabling it is
	 * idempotent, but resetting it here would corrupt an in-flight user of the
	 * same hardware counter, so retain the current count and use a logical base.
	 * A single CYCCNT read in the context-switch path is substantially cheaper
	 * than hal_read_curtime_us(), which latches and polls the system GTimer.
	 */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	configASSERT((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0U);
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	configASSERT((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U);
	atss_runtime_counter_base_cycles = DWT->CYCCNT;
}

uint32_t __attribute__((section(".itcm.text.atss_runtime_counter_get")))
atss_runtime_counter_get(void)
{
	/*
	 * Return raw CPU cycles. Unsigned deltas remain correct across one wrap;
	 * CYCCNT wraps in about 14.32 seconds at 300 MHz. ATSS samples every two
	 * seconds and the scheduler accounts at every task switch.
	 */
	return DWT->CYCCNT - atss_runtime_counter_base_cycles;
}
