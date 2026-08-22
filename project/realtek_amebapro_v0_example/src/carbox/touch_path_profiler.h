#ifndef CARBOX_TOUCH_PATH_PROFILER_H
#define CARBOX_TOUCH_PATH_PROFILER_H

#include <stdint.h>

void carbox_touch_path_profiler_report(uint32_t sequence);

/*
 * DispatchLite's implementation is closed.  The common dispatch_sync_f
 * wrapper calls these hooks so the touch profiler can follow only the HID
 * command submitted by AirPlayReceiverSessionSendHIDReport without enabling
 * the noisy system-wide GCD report.
 */
uint32_t carbox_touch_dispatch_sync_begin(void);
void carbox_touch_dispatch_sync_callback_begin(uint32_t token);
void carbox_touch_dispatch_sync_callback_end(uint32_t token);
void carbox_touch_dispatch_sync_complete(uint32_t token,
					 uint32_t submit_us,
					 uint32_t callback_start_us,
					 uint32_t callback_end_us,
					 uint32_t return_us,
					 int callback_called);

#endif /* CARBOX_TOUCH_PATH_PROFILER_H */
