/* Bounded navigation transmission pacing. This never changes PCM or RTP time. */
#include "audio_nav_pacing.h"
#include <string.h>

void nav_pace_init(struct nav_pacer *p, uint32_t bytes_per_sec,
                   uint32_t capacity, uint32_t now)
{
    memset(p, 0, sizeof(*p));
    p->bytes_per_sec = bytes_per_sec;
    p->capacity = capacity;
    p->last_control_us = p->deadline_us = p->phase_us = now;
    p->q_min_us = UINT32_MAX;
    p->init_us=now; p->onset_gap_min=UINT32_MAX; p->onset_q_min=UINT32_MAX;
}
void nav_pace_observe(struct nav_pacer *p, uint32_t bytes)
{
    if (!p->bytes_per_sec) return;
    p->q_us = (uint32_t)((uint64_t)bytes * 1000000U / p->bytes_per_sec);
    if (p->q_us < p->q_min_us) p->q_min_us = p->q_us;
    if (p->q_us > p->q_max_us) p->q_max_us = p->q_us;
    ++p->observations;
}
void nav_pace_pop(struct nav_pacer *p, uint32_t now, uint32_t remaining,
                  uint32_t packet_bytes, int ok)
{
    (void)packet_bytes;
    nav_pace_observe(p, remaining);
    if (!ok) {
        if (!p->empty_open) { p->empty_open = 1; p->empty_since_us = now; }
        return;
    }
    if (p->empty_open) {
        uint32_t duration = now - p->empty_since_us;
        if (!p->started) p->startup_empty_us += duration;
        else {
            ++p->empty_resume_n;
            if(p->sends && !p->onset_closed && now-p->first_us<1000000U)++p->onset_empty_n;
            p->empty_resume_total_us += duration;
            if (duration > p->empty_resume_max_us) p->empty_resume_max_us = duration;
        }
        p->empty_open = 0;
        /* No debt from time spent waiting for a producer; no added prefill. */
        p->deadline_us = p->phase_us = now; p->debt_us = 0;
        p->fraction = 0; p->boost_ppm = 0;
        /* An empty ring invalidates the previous backlog estimate as well. */
        p->filtered_us = p->q_us; p->last_control_us = now;
    }
    if (!p->started) {
        p->started = 1; p->deadline_us = p->phase_us = now; p->last_control_us = now;
        p->filtered_us = p->q_us;
    }
}
void nav_pace_schedule(struct nav_pacer *p, uint32_t now,
                       uint32_t remaining, uint32_t packet_bytes)
{
    uint32_t dt = now - p->last_control_us, target, step;
    uint64_t nominal_q16, interval_q16;
    nav_pace_observe(p, remaining);
    if (!p->bytes_per_sec || !packet_bytes) return;
    /* Long producer gaps should not make the controller jump to full boost. */
    if (dt > 100000U) dt = 100000U;
    p->last_control_us = now;
    p->filtered_us = (uint32_t)((int64_t)p->filtered_us +
        ((int64_t)p->q_us - p->filtered_us) * dt / (80000U + dt));
    target = p->filtered_us > 50000U ? p->filtered_us - 50000U : 0;
    if (target > 50000U) target = 50000U;
    step = dt / 10U; /* at most 100,000 ppm per second */
    if (p->boost_ppm < target) {
        uint32_t change = target - p->boost_ppm;
        p->boost_ppm += change < step ? change : step;
    } else {
        uint32_t change = p->boost_ppm - target;
        p->boost_ppm -= change < step ? change : step;
    }
    /* Never accelerate into an actually depleted ring just because EWMA is high. */
    if (p->q_us < 15000U) p->boost_ppm = 0;
    if (p->boost_ppm > p->boost_max_ppm) p->boost_max_ppm = p->boost_ppm;
    if ((uint64_t)remaining * 5U >= (uint64_t)p->capacity * 4U)
        ++p->pressure_samples;
    nominal_q16 = ((uint64_t)packet_bytes * 1000000U << 16) / p->bytes_per_sec;
    interval_q16 = nominal_q16 * 1000000U / (1000000U + p->boost_ppm);
    interval_q16 += p->fraction;
    p->fraction = (uint32_t)interval_q16 & 65535U;
    p->interval_us = (uint32_t)(interval_q16 >> 16);
    /* Keep the media schedule separate from the actual wake deadline. A late
     * send already spent its interval; do not charge another interval here.
     * Retain at most one 40 ms producer batch of timing debt. Older debt remains
     * in the ring and is drained by the queue controller, never by a burst. */
    p->phase_us += p->interval_us;
    p->debt_us = 0;
    if ((int32_t)(now - p->phase_us) > 0) {
        uint32_t late = now - p->phase_us;
        if (late > p->late_max_us) p->late_max_us = late;
        if (late > 40000U) {
            ++p->deadline_rebases;
            p->phase_us = now - 40000U;
            late = 40000U;
        }
        p->debt_us = late;
        if (late > p->debt_max_us) p->debt_max_us = late;
    }
    p->deadline_us = p->phase_us;
    /* Bound catch-up to 1.5x nominal packet rate (5 ms for 1440 bytes),
     * measured from send entry. Time spent building/sending counts toward it.
     * This bounds each gap even while the phase is overdue across many sends. */
    p->min_gap_us = (uint32_t)((nominal_q16 * 2U / 3U + 65535U) >> 16);
    if (p->sends) {
        uint32_t earliest = p->last_entry_us + p->min_gap_us;
        if ((int32_t)(earliest - p->deadline_us) > 0) {
            p->deadline_us = earliest;
            ++p->catchup_waits;
        }
    }
    if ((int32_t)(now - p->deadline_us) > 0) p->deadline_us = now;
}
void nav_pace_send_enter(struct nav_pacer *p, uint32_t now, uint32_t seq, uint32_t ts)
{
    if (!p->sends) {p->first_us=now;p->first_q_us=p->q_us;}
    if (now-p->first_us>=1000000U)p->onset_closed=1;
    p->onset_send_active=!p->onset_closed;
    if (!p->onset_closed) {
        ++p->onset_n;
        if(p->q_us<p->onset_q_min)p->onset_q_min=p->q_us;
        if(p->q_us>p->onset_q_max)p->onset_q_max=p->q_us;
        if(p->boost_ppm>p->onset_boost_max)p->onset_boost_max=p->boost_ppm;
        if(p->debt_us>p->onset_debt_max)p->onset_debt_max=p->debt_us;
        if(p->sends) {
            uint32_t gap=now-p->last_entry_us;
            p->onset_gap_total+=gap;
            if(gap<p->onset_gap_min)p->onset_gap_min=gap;
            if(gap>p->onset_gap_max)p->onset_gap_max=gap;
            if(gap<6000U)++p->onset_fast_n;
            if(gap>10000U)++p->onset_slow_n;
        }
    }
    if (p->sends) {
        uint32_t gap = now - p->last_entry_us;
        p->send_gap_total_us += gap;
        if (gap > p->send_gap_max_us) {
            p->send_gap_max_us = gap; p->gap_seq = seq; p->gap_ts = ts; p->gap_at_us = now;
        }
        if (gap > 10000U) ++p->gap_gt10ms;
        if (gap > 20000U) ++p->gap_gt20ms;
        if (gap > 40000U) ++p->gap_gt40ms;
    }
    ++p->sends; p->last_entry_us = now;
}
void nav_pace_send_exit(struct nav_pacer *p, uint32_t now)
{
    if(p->sends && p->onset_send_active &&
       now-p->last_entry_us>p->onset_send_max)p->onset_send_max=now-p->last_entry_us;
    if (p->returns && now - p->last_return_us > p->return_gap_max_us)
        p->return_gap_max_us = now - p->last_return_us;
    ++p->returns; p->last_return_us = now;
}
void nav_pace_finish(struct nav_pacer *p, uint32_t now)
{
    if (p->empty_open) {
        if (p->started) p->terminal_empty_us += now - p->empty_since_us;
        else p->startup_empty_us += now - p->empty_since_us;
        p->empty_open = 0;
    }
}
