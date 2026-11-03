#include <assert.h>
#include <stdio.h>
#include "wcam_stats.h"

int main(void)
{
    struct wcam_stats s;
    struct wcam_rates r;
    struct wcam_trace q = {0};
    struct wcam_event event;
    /* Hand-counted two-second window: 2 encoded, 1 delivered, 1 discarded.
     * Catches counting encoded frames as throughput and tick/ms confusion. */
    wcam_stats_reset(&s, 100);
    wcam_stats_encoded(&s, 1000, 2, WCAM_NAL_IDR | WCAM_NAL_SPS | WCAM_NAL_PPS);
    wcam_stats_encoded(&s, 3000, 4, WCAM_NAL_SLICE);
    wcam_stats_sent(&s, 1000, 5);
    s.dropped++;
    r = wcam_stats_rates(&s, 300, 100);
    assert(r.elapsed_ms == 2000 && r.fps10 == 5 && r.kbps == 4);
    assert(r.enc_avg_ms == 30 && r.enc_max_ms == 40);
    assert(r.usb_avg_ms == 50 && r.usb_max_ms == 50);
    assert(s.encoded == 2 && s.sent == 1 && s.dropped == 1);
    assert(s.frame_min == 1000 && s.frame_max == 3000);
    assert(s.idr == 1 && s.nonidr == 1 && s.sps == 1 && s.pps == 1 && s.no_vcl == 0);
    wcam_stats_encoded(&s, 40, 1, WCAM_NAL_SPS);
    assert(s.no_vcl == 1);
    /* Empty/zero-duration windows and wrapping RTOS ticks must be safe. */
    wcam_stats_reset(&s, UINT32_MAX - 499);
    r = wcam_stats_rates(&s, s.start, 1000);
    assert(r.elapsed_ms == 0 && r.fps10 == 0 && r.enc_avg_ms == 0);
    wcam_stats_encoded(&s, 125000, 5, WCAM_NAL_IDR);
    wcam_stats_sent(&s, 125000, 10);
    r = wcam_stats_rates(&s, 500, 1000);
    assert(r.elapsed_ms == 1000 && r.fps10 == 10 && r.kbps == 1000);
    /* Mixed 3/4-byte Annex B starts, duplicate slices and emulation prevention.
     * Do not label non-IDR slices as P without parsing slice headers. */
    {
        const uint8_t au[] = {0,0,0,1,0x67,0x42, 0,0,1,0x68,0x12,
                             0,0,0,1,0x65,0x80, 0,0,1,0x65,0x80};
        const uint8_t p[] = {0,0,1,0x41,0,0,3,1,0x67};
        const uint8_t tail[] = {0,0,0,1};
        assert(wcam_nal_flags(au, sizeof(au)) == 14);
        assert(wcam_nal_flags(p, sizeof(p)) == 1);
        assert(wcam_nal_flags(tail, sizeof(tail)) == 0);
        assert(wcam_nal_flags(NULL, 0) == 0);
        for (size_t n = 0; n < sizeof(au); ++n) (void)wcam_nal_flags(au, n);
    }
    /* Full trace must stay bounded, report losses and preserve FIFO order. */
    for (unsigned i = 0; i < WCAM_TRACE_CAPACITY + 3; ++i)
        wcam_trace_push(&q, (struct wcam_event){1, i, 0, 0, 0});
    assert(q.lost == 3);
    for (unsigned i = 0; i < WCAM_TRACE_CAPACITY; ++i) {
        assert(wcam_trace_pop(&q, &event));
        assert(event.a == i);
    }
    assert(!wcam_trace_pop(&q, &event));
    wcam_trace_push(&q, (struct wcam_event){2, 99, 0, 0, 0});
    assert(wcam_trace_pop(&q, &event) && event.a == 99);
    puts("stats: delivery rates, tick wrap, NAL bounds and bounded trace passed");
}
