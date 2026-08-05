#ifndef CARBOX_GCD_SYNC_PROFILER_H
#define CARBOX_GCD_SYNC_PROFILER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the existing 10-second PC-profiler reporter task. */
void gcd_sync_profiler_report(uint32_t sequence);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_GCD_SYNC_PROFILER_H */
