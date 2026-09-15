#include <cassert>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <initializer_list>
#include "cmsis.h"

MockDwt mock_dwt;
MockCoreDebug mock_debug;
uint32_t SystemCoreClock;
static uint32_t counter, step, reads, freeze_after, last_count;
static uint32_t fallback_calls, fallback_us;

uint32_t mock_cycle_read(void)
{
	assert(reads < 100000U); // Also detect an unbounded stall in the DUT.
	last_count = counter;
	if (++reads < freeze_after)
		counter += step;
	return last_count;
}

void hal_delay_us(uint32_t us)
{
	++fallback_calls;
	fallback_us = us;
}

// Compile the production implementation against simulated CMSIS registers.
#include "../../libusb_ref_compat/libusb_ref_compat_delay.c"

static void reset(uint32_t hz = 300000000U)
{
	SystemCoreClock = hz;
	mock_dwt.CTRL = DWT_CTRL_CYCCNTENA_Msk;
	mock_debug.DEMCR = CoreDebug_DEMCR_TRCENA_Msk;
	counter = reads = last_count = fallback_calls = fallback_us = 0;
	step = 1U;
	freeze_after = UINT32_MAX;
}

static void expect_fallback(uint32_t us)
{
	DelayUs(us);
	assert(fallback_calls == 1 && fallback_us == us);
}

int main(void)
{
	reset(); DelayUs(0); assert(reads == 0 && fallback_calls == 0);
	for (uint32_t hz : {300000000U, 400000000U, 300000001U, UINT32_MAX}) {
		for (uint32_t us : {1U, 10U}) {
			reset(hz);
			counter = UINT32_MAX - 20U; // All cases cross rollover.
			uint32_t start = counter;
			uint32_t expected = (uint64_t(hz) * us + 999999ULL) / 1000000ULL;
			DelayUs(us);
			assert(uint32_t(last_count - start) == expected);
			assert(fallback_calls == 0);
		}
	}
	reset(); step = 30000U; DelayUs(1); // Simulate time spent preempted.
	assert(reads == 2 && fallback_calls == 0);
	reset(); expect_fallback(11); assert(reads == 0);
	reset(); expect_fallback(UINT32_MAX); assert(reads == 0);
	reset(0); expect_fallback(1);
	reset(); mock_debug.DEMCR = 0; expect_fallback(1);
	reset(); mock_dwt.CTRL = 0; expect_fallback(1);
	reset(); mock_dwt.CTRL |= DWT_CTRL_NOCYCCNT_Msk; expect_fallback(1);
	reset(); step = 0; expect_fallback(1); assert(reads == 33);
	reset(); freeze_after = 20; expect_fallback(1); // Stops mid-delay.
	assert(reads == 52);
	puts("DelayUs host tests passed (18 cases)");
}
