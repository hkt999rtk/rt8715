#ifndef WCAM_STATS_H
#define WCAM_STATS_H
#include <stddef.h>
#include <stdint.h>

#define WCAM_NAL_SLICE 1u
#define WCAM_NAL_IDR 2u
#define WCAM_NAL_SPS 4u
#define WCAM_NAL_PPS 8u

struct wcam_stats {
    uint32_t start, encoded, sent, dropped, overruns;
    uint32_t frame_min, frame_max, enc_max, usb_max;
    uint32_t idr, nonidr, sps, pps, no_vcl;
    uint64_t bytes, enc_ticks, usb_ticks;
};
struct wcam_rates {
    uint32_t elapsed_ms, fps10, kbps;
    uint32_t enc_avg_ms, enc_max_ms, usb_avg_ms, usb_max_ms;
};
void wcam_stats_reset(struct wcam_stats *s, uint32_t now);
unsigned wcam_nal_flags(const uint8_t *data, size_t length);
void wcam_stats_encoded(struct wcam_stats *s, uint32_t bytes,
                        uint32_t ticks, unsigned nal_flags);
void wcam_stats_sent(struct wcam_stats *s, uint32_t bytes, uint32_t ticks);
struct wcam_rates wcam_stats_rates(const struct wcam_stats *s,
                                   uint32_t now, uint32_t tick_hz);

/* Callers serialize push/pop. Never print from a USB callback. */
#define WCAM_TRACE_CAPACITY 32u
struct wcam_event { uint32_t kind, a, b, c, d; };
struct wcam_trace {
    struct wcam_event events[WCAM_TRACE_CAPACITY];
    unsigned head, count;
    uint32_t lost;
};
void wcam_trace_push(struct wcam_trace *q, struct wcam_event event);
int wcam_trace_pop(struct wcam_trace *q, struct wcam_event *event);
#endif
