#include "wcam_stats.h"
#include <string.h>

void wcam_stats_reset(struct wcam_stats *s, uint32_t now)
{
    memset(s, 0, sizeof(*s));
    s->start = now;
}

unsigned wcam_nal_flags(const uint8_t *data, size_t length)
{
    unsigned flags = 0, zeros = 0;
    size_t i;
    for (i = 0; i < length; ++i) {
        if (data[i] == 1 && zeros >= 2 && i + 1 < length) {
            switch (data[i + 1] & 31) {
            case 1: flags |= WCAM_NAL_SLICE; break;
            case 5: flags |= WCAM_NAL_IDR; break;
            case 7: flags |= WCAM_NAL_SPS; break;
            case 8: flags |= WCAM_NAL_PPS; break;
            }
        }
        zeros = data[i] == 0 ? (zeros < 3 ? zeros + 1 : 3) : 0;
    }
    return flags;
}

void wcam_stats_encoded(struct wcam_stats *s, uint32_t bytes,
                        uint32_t ticks, unsigned flags)
{
    if (!s->encoded || bytes < s->frame_min) s->frame_min = bytes;
    if (bytes > s->frame_max) s->frame_max = bytes;
    ++s->encoded;
    s->enc_ticks += ticks;
    if (ticks > s->enc_max) s->enc_max = ticks;
    s->idr += !!(flags & WCAM_NAL_IDR);
    s->nonidr += !!(flags & WCAM_NAL_SLICE);
    s->sps += !!(flags & WCAM_NAL_SPS);
    s->pps += !!(flags & WCAM_NAL_PPS);
    s->no_vcl += !(flags & (WCAM_NAL_IDR | WCAM_NAL_SLICE));
}

void wcam_stats_sent(struct wcam_stats *s, uint32_t bytes, uint32_t ticks)
{
    ++s->sent;
    s->bytes += bytes;
    s->usb_ticks += ticks;
    if (ticks > s->usb_max) s->usb_max = ticks;
}

struct wcam_rates wcam_stats_rates(const struct wcam_stats *s,
                                   uint32_t now, uint32_t hz)
{
    struct wcam_rates r = {0};
    uint32_t elapsed = now - s->start;
    if (!hz) return r;
    r.elapsed_ms = (uint64_t)elapsed * 1000 / hz;
    if (elapsed) {
        r.fps10 = (uint64_t)s->sent * 10 * hz / elapsed;
        r.kbps = s->bytes * 8 * hz / elapsed / 1000;
    }
    if (s->encoded) r.enc_avg_ms = s->enc_ticks * 1000 / hz / s->encoded;
    if (s->sent) r.usb_avg_ms = s->usb_ticks * 1000 / hz / s->sent;
    r.enc_max_ms = (uint64_t)s->enc_max * 1000 / hz;
    r.usb_max_ms = (uint64_t)s->usb_max * 1000 / hz;
    return r;
}

void wcam_trace_push(struct wcam_trace *q, struct wcam_event event)
{
    if (q->count == WCAM_TRACE_CAPACITY) {
        ++q->lost;
        return;
    }
    q->events[(q->head + q->count) % WCAM_TRACE_CAPACITY] = event;
    ++q->count;
}

int wcam_trace_pop(struct wcam_trace *q, struct wcam_event *event)
{
    if (!q->count) return 0;
    *event = q->events[q->head];
    q->head = (q->head + 1) % WCAM_TRACE_CAPACITY;
    --q->count;
    return 1;
}
