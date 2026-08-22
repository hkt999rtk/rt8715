#ifndef CARBOX_TOUCH_FRAME_PROFILER_H
#define CARBOX_TOUCH_FRAME_PROFILER_H

#include <stdint.h>

uint32_t carbox_touch_frame_dispatch_begin(void);
void carbox_touch_frame_dispatch_callback_begin(uint32_t token);
void carbox_touch_frame_dispatch_callback_end(uint32_t token);
void carbox_touch_frame_dispatch_complete(uint32_t token,
					  uint32_t submit_us,
					  uint32_t callback_start_us,
					  uint32_t callback_end_us,
					  uint32_t return_us,
					  int callback_called);
void carbox_touch_frame_source_frame(uint32_t source_delta_us,
				      int delta_valid);
void carbox_touch_frame_profiler_report(uint32_t sequence);

#endif /* CARBOX_TOUCH_FRAME_PROFILER_H */
