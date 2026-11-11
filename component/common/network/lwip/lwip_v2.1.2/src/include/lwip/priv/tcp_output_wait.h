#ifndef LWIP_TCP_OUTPUT_WAIT_H
#define LWIP_TCP_OUTPUT_WAIT_H
#include "lwip/opt.h"

/* 仅测试版 USB screen 观测；关闭时不产生调用，也不改变 PCB 布局。 */
#if LWIP_TCP && LWIP_TCP_RECOVERY && CONFIG_TCP_RECOVERY_TRACE && CONFIG_MOVE_LAG_PROFILE
#define TCP_WAIT_ROWS 4U
#define TCP_WAIT_EVENTS 16U
#define TCP_WAIT_CWND 1U
#define TCP_WAIT_RWND 2U
#define TCP_WAIT_NAGLE 4U
#define TCP_WAIT_SENT 1U
#define TCP_WAIT_EMPTY 2U
#define TCP_WAIT_CLOSED 3U
#define TCP_WAIT_SKIPPED 4U
struct tcp_pcb;
struct tcp_seg;
struct tcp_wait_event {
  u32_t connection, episode, start_ms, end_ms, duration_ms, rto_id;
  u32_t seq, length, flight, cwnd, wnd, effective_wnd, need;
  u32_t first_ack_ms, last_ack_ms, ack_count, resume_seq, resume_len;
  u16_t local_port, remote_port, flags;
  u8_t reason_mask, result;
};
struct tcp_wait_row {
  u32_t connection, started, resumed, cancelled, checks;
  u32_t cwnd_checks, rwnd_checks, nagle_checks, max_ms;
  u32_t last_rto_id, first_ack_at, last_ack_at, snapshot_ms;
  u8_t active, closed;
  struct tcp_wait_event current;
};
void tcp_wait_block(const struct tcp_pcb *, const struct tcp_seg *, u8_t reason);
void tcp_wait_window(const struct tcp_pcb *, const struct tcp_seg *, u32_t effective_wnd);
void tcp_wait_sent(const struct tcp_pcb *, const struct tcp_seg *);
void tcp_wait_ack(const struct tcp_pcb *);
void tcp_wait_rto(const struct tcp_pcb *, u32_t id);
void tcp_wait_cancel(const struct tcp_pcb *, u8_t result);
int tcp_wait_snapshot(unsigned index, struct tcp_wait_row *, u32_t *evicted);
int tcp_wait_pop(struct tcp_wait_event *, u32_t *dropped);
#else
#define tcp_wait_block(p,s,r) ((void)0)
#define tcp_wait_window(p,s,w) ((void)0)
#define tcp_wait_sent(p,s) ((void)0)
#define tcp_wait_ack(p) ((void)0)
#define tcp_wait_rto(p,id) ((void)0)
#define tcp_wait_cancel(p,r) ((void)0)
#endif
#endif
