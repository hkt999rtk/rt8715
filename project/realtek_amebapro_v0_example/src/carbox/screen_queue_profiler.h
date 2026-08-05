#ifndef CARBOX_SCREEN_QUEUE_PROFILER_H
#define CARBOX_SCREEN_QUEUE_PROFILER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the existing 10-second PC-profiler reporter task. */
void screen_queue_profiler_report(uint32_t sequence);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_SCREEN_QUEUE_PROFILER_H */
