#ifndef WCAM_PACER_H
#define WCAM_PACER_H
#include <stdint.h>
#include "wcam_uvc_control.h"

struct wcam_pacer {
    uint32_t deadline;
    unsigned fraction;
};

static inline uint32_t wcam_pacer_delay(struct wcam_pacer *pacer,
                                       uint32_t now, unsigned tick_hz)
{
    unsigned period;
    pacer->fraction += tick_hz;
    period = pacer->fraction / WCAM_FPS;
    pacer->fraction %= WCAM_FPS;
    pacer->deadline += period;
    if ((int32_t)(now - pacer->deadline) >= 0) {
        pacer->deadline = now;
        return 0;
    }
    return pacer->deadline - now;
}
#endif
