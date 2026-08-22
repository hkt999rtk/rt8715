#ifndef CARBOX_AIRPLAY_MUTEX_PROFILER_H
#define CARBOX_AIRPLAY_MUTEX_PROFILER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bracket a known airplay_mutex user so the pthread wrapper can discover the
 * private mutex in the vendor archive without depending on its data symbol. */
void airplay_mutex_profiler_touch_enter(void);
void airplay_mutex_profiler_touch_leave(void);

void airplay_mutex_profiler_report(uint32_t sequence);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_AIRPLAY_MUTEX_PROFILER_H */
