#ifndef CARBOX_AUDIO_NAV_PACING_H
#define CARBOX_AUDIO_NAV_PACING_H
#include <stdint.h>
/* Single output owner; caller protects updates/snapshot against the reporter.
 * All times are wrapping hal microseconds. Duration comparisons must be <2^31 us.
 * State is allocated once per stream, never per packet. */
struct nav_pacer {
    uint32_t bytes_per_sec, capacity, last_control_us, deadline_us, fraction;
    uint32_t q_us, filtered_us, q_min_us, q_max_us, observations;
    uint32_t boost_ppm, boost_max_ppm, interval_us, deadline_rebases, late_max_us;
    uint32_t phase_us, debt_us, debt_max_us, catchup_waits, min_gap_us;
    uint32_t pressure_samples, started, empty_open, empty_since_us;
    uint32_t startup_empty_us, empty_resume_n, empty_resume_max_us, terminal_empty_us;
    uint64_t empty_resume_total_us;
    uint32_t sends, returns, last_entry_us, last_return_us;
    uint32_t send_gap_max_us, return_gap_max_us, gap_gt10ms, gap_gt20ms, gap_gt40ms;
    uint32_t gap_seq, gap_ts, gap_at_us;
    uint64_t send_gap_total_us;
    /* First second from first send entry, not from first audible syllable. */
    uint32_t init_us, first_us, first_q_us, onset_n, onset_gap_min, onset_gap_max;
    uint32_t onset_closed, onset_send_active;
    uint32_t onset_q_min, onset_q_max, onset_boost_max, onset_debt_max;
    uint32_t onset_send_max, onset_empty_n, onset_fast_n, onset_slow_n;
    uint64_t onset_gap_total;

};
void nav_pace_init(struct nav_pacer *, uint32_t, uint32_t, uint32_t);
void nav_pace_observe(struct nav_pacer *, uint32_t);
void nav_pace_pop(struct nav_pacer *, uint32_t, uint32_t, uint32_t, int);
void nav_pace_schedule(struct nav_pacer *, uint32_t, uint32_t, uint32_t);
void nav_pace_send_enter(struct nav_pacer *, uint32_t, uint32_t, uint32_t);
void nav_pace_send_exit(struct nav_pacer *, uint32_t);
void nav_pace_finish(struct nav_pacer *, uint32_t);
#endif
