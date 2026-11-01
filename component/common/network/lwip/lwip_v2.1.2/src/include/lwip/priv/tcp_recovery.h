#ifndef LWIP_HDR_TCP_RECOVERY_H
#define LWIP_HDR_TCP_RECOVERY_H
#include "lwip/tcp.h"
#if LWIP_TCP && LWIP_TCP_RECOVERY
#define TCP_RECOVERY_INTERVAL 50U
#define TCP_RECOVERY_RTO_MIN 1000U
#define TCP_RECOVERY_RTO_MAX 60000U

/* Only TCP core writes these counters; the statistics task snapshots them. */
struct tcp_recovery_stats {
  u32_t lt_opportunities, lt_sent, fast, partial, full, busy, rto, output_error;
  u32_t rtt_samples, rtt_sum_ms, rtt_max_ms;
  u32_t recovery_max_ms, deadline_late_max_ms;
  u32_t last_seq, last_ack, last_cwnd, last_flight, last_rto_ms;
  u32_t last_event_ms, rto_current_ms, srtt_current_ms;
  u8_t last_event; /* 1=LT 2=fast 3=partial 4=full 5=busy 6=RTO 7=output error */
  u16_t local_port, remote_port;
};
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
