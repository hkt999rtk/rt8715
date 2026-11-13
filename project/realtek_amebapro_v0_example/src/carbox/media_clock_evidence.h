#ifndef CARBOX_MEDIA_CLOCK_EVIDENCE_H
#define CARBOX_MEDIA_CLOCK_EVIDENCE_H
#include <stdint.h>
#ifndef CONFIG_MEDIA_CLOCK_DRIFT_PROFILE
#define CONFIG_MEDIA_CLOCK_DRIFT_PROFILE 0
#endif
#if CONFIG_MEDIA_CLOCK_DRIFT_PROFILE
void carbox_media_clock_evidence_tick(void);
void carbox_media_clock_evidence_pair(uint64_t *hal_us,uint64_t *rtos_us);
void carbox_media_clock_evidence_report(uint32_t sequence);
#else
static inline void carbox_media_clock_evidence_report(uint32_t sequence){(void)sequence;}
#endif
#endif
