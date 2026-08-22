#ifndef CARBOX_SCREEN_QUEUE_PROFILER_H
#define CARBOX_SCREEN_QUEUE_PROFILER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the existing 10-second PC-profiler reporter task. */
void screen_queue_profiler_report(uint32_t sequence);

/* Called by the isolated screen timestamp generator/wrapper. */
void carbox_screen_timestamp_profile_sample(uint64_t original_ticks,
					    uint64_t emitted_ntp,
					    uint32_t sample_us);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_SCREEN_QUEUE_PROFILER_H */
