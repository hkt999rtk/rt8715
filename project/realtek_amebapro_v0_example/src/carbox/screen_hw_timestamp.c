#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"

#include "screen_queue_profiler.h"

#ifndef CONFIG_SCREEN_HW_TIMESTAMP
#define CONFIG_SCREEN_HW_TIMESTAMP 0
#endif

#define SCREEN_HW_US_WRAP       (1ULL << 32)
#define SCREEN_HW_US_HALF_WRAP  (1ULL << 31)

extern uint64_t __real_UpTicksToNTP(uint64_t ticks);

#if CONFIG_SCREEN_HW_TIMESTAMP
static uint64_t screen_hw_epoch_offset_us;
static uint64_t screen_hw_last_us;
static uint8_t screen_hw_epoch_valid;

static uint64_t screen_rtos_time_us(void)
{
	TimeOut_t state;
	uint64_t ticks;

	/*
	 * vTaskSetTimeOutState() snapshots both the 32-bit tick and its overflow
	 * count atomically.  The RTOS clock is only the coarse epoch oracle here;
	 * frame-to-frame elapsed time comes from the hardware system timer.
	 */
	vTaskSetTimeOutState(&state);
	ticks = ((uint64_t)(uint32_t)state.xOverflowCount << 32) |
		(uint64_t)state.xTimeOnEntering;

	return (ticks / configTICK_RATE_HZ) * 1000000ULL +
		((ticks % configTICK_RATE_HZ) * 1000000ULL) /
		configTICK_RATE_HZ;
}

static uint64_t screen_extend_hw_time_us(uint32_t hw_low, uint64_t coarse_us)
{
	uint64_t candidate = (coarse_us & ~(SCREEN_HW_US_WRAP - 1ULL)) |
		(uint64_t)hw_low;

	/* Select the hardware epoch nearest the 64-bit RTOS uptime. */
	if ((candidate < coarse_us) &&
	    ((coarse_us - candidate) > SCREEN_HW_US_HALF_WRAP)) {
		candidate += SCREEN_HW_US_WRAP;
	} else if ((candidate > coarse_us) &&
		((candidate - coarse_us) > SCREEN_HW_US_HALF_WRAP) &&
		(candidate >= SCREEN_HW_US_WRAP)) {
		candidate -= SCREEN_HW_US_WRAP;
	}

	return candidate;
}

static uint64_t screen_hw_time_us(uint32_t *sample_us)
{
	uint64_t before_us = 0U;
	uint64_t after_us = 0U;
	uint64_t coarse_us;
	uint64_t raw_us;
	uint64_t precise_us;
	uint32_t hw_low = 0U;
	uint32_t attempt;

	/*
	 * Bracket the hardware latch with overflow-aware RTOS snapshots.  Retry a
	 * boundary crossing so the low hardware word and coarse epoch describe the
	 * same instant. The final midpoint is also safe if an unusually slow latch
	 * crosses a tick on every attempt.
	 */
	for (attempt = 0U; attempt < 3U; attempt++) {
		before_us = screen_rtos_time_us();
		hw_low = hal_read_curtime_us();
		after_us = screen_rtos_time_us();
		if (before_us == after_us) {
			break;
		}
	}
	coarse_us = before_us + ((after_us - before_us) / 2U);
	raw_us = screen_extend_hw_time_us(hw_low, coarse_us);

	taskENTER_CRITICAL();
	if (!screen_hw_epoch_valid) {
		/* Preserve the old uptime epoch while gaining hardware resolution. */
		screen_hw_epoch_offset_us = coarse_us - raw_us;
		screen_hw_epoch_valid = 1U;
	}
	precise_us = raw_us + screen_hw_epoch_offset_us;

	/* Protect the wire timestamp from an unexpected timer reset or race. */
	if (precise_us < screen_hw_last_us) {
		precise_us = screen_hw_last_us;
	} else {
		screen_hw_last_us = precise_us;
	}
	taskEXIT_CRITICAL();

	*sample_us = hw_low;
	return precise_us;
}

static uint64_t screen_us_to_ntp(uint64_t time_us)
{
	uint64_t seconds = time_us / 1000000ULL;
	uint64_t remainder = time_us % 1000000ULL;
	uint64_t fraction = (remainder << 32) / 1000000ULL;

	return (seconds << 32) | (uint32_t)fraction;
}
#endif

uint64_t __wrap_UpTicksToNTP(uint64_t ticks)
{
	uint32_t sample_us;
	uint64_t ntp;

#if CONFIG_SCREEN_HW_TIMESTAMP
	ntp = screen_us_to_ntp(screen_hw_time_us(&sample_us));
#else
	sample_us = hal_read_curtime_us();
	ntp = __real_UpTicksToNTP(ticks);
#endif

	carbox_screen_timestamp_profile_sample(ticks, ntp, sample_us);
	return ntp;
}
