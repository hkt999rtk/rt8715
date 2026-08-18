#include "touch_path_profiler.h"

#ifndef CONFIG_TOUCH_PATH_PROFILE
#define CONFIG_TOUCH_PATH_PROFILE 0
#endif

#if CONFIG_TOUCH_PATH_PROFILE

#include "diag.h"

extern uint32_t GetTickCount(void);

typedef struct touch_path_stats_s {
	volatile uint32_t input_calls;
	volatile uint32_t input_last_ms;
	volatile uint32_t input_action;
	volatile uint32_t input_x;
	volatile uint32_t input_y;
	volatile uint32_t hid_calls;
	volatile uint32_t hid_touch_calls;
	volatile uint32_t hid_errors;
	volatile uint32_t hid_last_ms;
	volatile uint32_t hid_elapsed_ms;
	volatile uint32_t hid_elapsed_max_ms;
} touch_path_stats_t;

static touch_path_stats_t touch_path_stats;
static uint32_t touch_path_prev_input;
static uint32_t touch_path_prev_hid;
static uint32_t touch_path_prev_hid_touch;
static uint32_t touch_path_prev_errors;

extern void __real_lib_carplay_touch(uint32_t action, uint32_t x, uint32_t y);
extern int32_t __real_AirPlayReceiverSessionSendHIDReport(
	void *session, uint32_t device_uid, const void *report, uint32_t length);

void __wrap_lib_carplay_touch(uint32_t action, uint32_t x, uint32_t y)
{
	touch_path_stats.input_calls++;
	touch_path_stats.input_last_ms = GetTickCount();
	touch_path_stats.input_action = action;
	touch_path_stats.input_x = x;
	touch_path_stats.input_y = y;
	__real_lib_carplay_touch(action, x, y);
}

int32_t __wrap_AirPlayReceiverSessionSendHIDReport(
	void *session, uint32_t device_uid, const void *report, uint32_t length)
{
	uint32_t start_ms = GetTickCount();
	int32_t result;
	uint32_t elapsed_ms;

	touch_path_stats.hid_calls++;
	if (length == 5U) {
		touch_path_stats.hid_touch_calls++;
	}
	result = __real_AirPlayReceiverSessionSendHIDReport(
		session, device_uid, report, length);
	elapsed_ms = GetTickCount() - start_ms;
	touch_path_stats.hid_last_ms = GetTickCount();
	touch_path_stats.hid_elapsed_ms += elapsed_ms;
	if (elapsed_ms > touch_path_stats.hid_elapsed_max_ms) {
		touch_path_stats.hid_elapsed_max_ms = elapsed_ms;
	}
	if (result != 0) {
		touch_path_stats.hid_errors++;
	}
	return result;
}

void carbox_touch_path_profiler_report(uint32_t sequence)
{
	uint32_t now_ms = GetTickCount();
	uint32_t input_calls = touch_path_stats.input_calls;
	uint32_t hid_calls = touch_path_stats.hid_calls;
	uint32_t hid_touch_calls = touch_path_stats.hid_touch_calls;
	uint32_t errors = touch_path_stats.hid_errors;
	uint32_t hid_delta = hid_calls - touch_path_prev_hid;
	uint32_t elapsed_ms = touch_path_stats.hid_elapsed_ms;
	uint32_t elapsed_max_ms = touch_path_stats.hid_elapsed_max_ms;

	touch_path_stats.hid_elapsed_ms = 0U;
	touch_path_stats.hid_elapsed_max_ms = 0U;
	rt_printf("[TOUCHPATH][%lu] input/hid/touch5/error=%lu/%lu/%lu/%lu "
		  "age_ms input/hid=%ld/%ld hid_time_ms avg/max=%lu/%lu "
		  "last action/x/y=%lu/%lu/%lu observation_only=1\r\n",
		  (unsigned long)sequence,
		  (unsigned long)(input_calls - touch_path_prev_input),
		  (unsigned long)hid_delta,
		  (unsigned long)(hid_touch_calls - touch_path_prev_hid_touch),
		  (unsigned long)(errors - touch_path_prev_errors),
		  touch_path_stats.input_last_ms != 0U ?
			(long)(now_ms - touch_path_stats.input_last_ms) : -1L,
		  touch_path_stats.hid_last_ms != 0U ?
			(long)(now_ms - touch_path_stats.hid_last_ms) : -1L,
		  (unsigned long)(hid_delta != 0U ? elapsed_ms / hid_delta : 0U),
		  (unsigned long)elapsed_max_ms,
		  (unsigned long)touch_path_stats.input_action,
		  (unsigned long)touch_path_stats.input_x,
		  (unsigned long)touch_path_stats.input_y);

	touch_path_prev_input = input_calls;
	touch_path_prev_hid = hid_calls;
	touch_path_prev_hid_touch = hid_touch_calls;
	touch_path_prev_errors = errors;
}

#else

void carbox_touch_path_profiler_report(uint32_t sequence)
{
	(void)sequence;
}

#endif
