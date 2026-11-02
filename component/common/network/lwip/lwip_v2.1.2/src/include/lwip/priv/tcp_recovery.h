#ifndef LWIP_HDR_TCP_RECOVERY_H
#define LWIP_HDR_TCP_RECOVERY_H
#include "lwip/tcp.h"
#if LWIP_TCP && LWIP_TCP_RECOVERY
#define TCP_RECOVERY_INTERVAL 10U
#define TCP_RECOVERY_RTO_MIN 1000U
#define TCP_RECOVERY_USB_SCREEN_RTO_MIN 50U
#define TCP_RECOVERY_RTO_MAX 60000U

/* Only TCP core writes these counters; the statistics task snapshots them. */
struct tcp_recovery_stats {
  u32_t lt_opportunities, lt_sent, fast, partial, full, busy, rto, output_error;
  u32_t rtt_samples, rtt_sum_ms, rtt_max_ms;
  u32_t heuristic_enter, heuristic_from_rto, heuristic_reject;
  u32_t heuristic_progress, heuristic_timeout, heuristic_progress_max_ms;
  u32_t recovery_max_ms, deadline_late_max_ms;
  u32_t last_seq, last_ack, last_cwnd, last_flight, last_rto_ms;
  u32_t last_event_ms, rto_current_ms, srtt_current_ms;
  u8_t last_event; /* 1=LT 2=fast 3=partial 4=full 5=busy 6=RTO 7=output error */
  u16_t local_port, remote_port;
};
/* Rejection bits for the optional RFC 6582 ACK heuristic. */
#define TCP_HEURISTIC_NO_PROGRESS 1U
#define TCP_HEURISTIC_SMALL_CWND 2U
#define TCP_HEURISTIC_LARGE_ACK 4U
#define TCP_HEURISTIC_NOT_ESTABLISHED 8U
#define TCP_HEURISTIC_ZERO_WINDOW 16U
#if CONFIG_TCP_RECOVERY_TRACE
#define TCP_RECOVERY_TRACE_SLOTS 32U /* separate normal and RTO rings */
enum tcp_recovery_event_kind {
  TCP_TRACE_FAST = 1, TCP_TRACE_PARTIAL, TCP_TRACE_FULL, TCP_TRACE_FAST_SKIP,
  TCP_TRACE_RTO_DUE, TCP_TRACE_RTO_ARM, TCP_TRACE_RTO_TX, TCP_TRACE_RTO_ACK,
  TCP_TRACE_RETX, TCP_TRACE_OUTPUT_ERROR, TCP_TRACE_BUSY, TCP_TRACE_ACK_GAP,
  TCP_TRACE_HEURISTIC, TCP_TRACE_HEURISTIC_ACK
};
struct tcp_recovery_event {
  u32_t id, connection, ms;
  u32_t lastack, snd_nxt, recover_end, cwnd, ssthresh, wnd, sndbuf;
  u32_t timer_age_ms, rto_ms, srtt_ms, ack_age_ms, progress_age_ms;
  u32_t ack_no, ack_wnd, ack_gap_ms, tx_seq, tx_end, tx_age_ms;
  u32_t unacked_seq, unsent_seq, partial_count;
  u32_t ack_advance, rto_end, heuristic_age_ms;
  u16_t local_port, remote_port, flags, qlen, unacked_len, unsent_len;
  u16_t unacked_ref, unsent_ref;
  u8_t kind, state, dupacks, nrtx, heads, pending, recover_valid, skip_reason;
  u8_t heuristic_reject;
};
#define TCP_RECOVERY_CAUSE_SLOTS 8U
#define TCP_SKIP_NO_UNACKED 1U
#define TCP_SKIP_IN_RECOVERY 2U
#define TCP_SKIP_IN_RTO 4U
#define TCP_SKIP_BOUNDARY 8U
struct tcp_recovery_cause {
  struct tcp_recovery_event full, skip, due;
  u32_t ack_packets, progress_calls, progress_bytes, progress_ms, progress_ack;
  u32_t skips, skip_mask;
};
/* Frozen FIFO drops NEW entries when full. full.id=0 means no retained history.
 * dropped/evicted are cumulative, including calls returning no record. */
int tcp_recovery_cause_pop(struct tcp_recovery_cause *out, u32_t *dropped, u32_t *evicted);
/* Copy one immutable record under a short lock. No PCB pointer escapes. */
int tcp_recovery_trace_pop(u8_t urgent, struct tcp_recovery_event *out, u32_t *overwritten);
void tcp_recovery_trace_ack(struct tcp_pcb *pcb, u32_t ackno, u32_t advertised_wnd);
#endif
/* Called only for the screen sender, under the TCP core lock. */
int tcp_recovery_usb_screen(struct tcp_pcb *pcb);
int tcp_recovery_fine_timer(const struct tcp_pcb *pcb);
void tcp_recovery_dupack(struct tcp_pcb *pcb);
u32_t tcp_recovery_output_window(const struct tcp_pcb *pcb);
void tcp_recovery_sent(struct tcp_pcb *pcb, const struct tcp_seg *seg);
void tcp_recovery_retransmitted(struct tcp_pcb *pcb);
void tcp_recovery_fast(struct tcp_pcb *pcb);
void tcp_recovery_ack(struct tcp_pcb *pcb, tcpwnd_size_t acked, u8_t was_in_recovery);
u32_t tcp_recovery_rtt_ack(struct tcp_pcb *pcb, u32_t ackno);
void tcp_recovery_tmr(void);
void tcp_recovery_busy(struct tcp_pcb *pcb);
void tcp_recovery_output_error(struct tcp_pcb *pcb);
void tcp_recovery_snapshot(struct tcp_recovery_stats *result);
#endif
#endif
