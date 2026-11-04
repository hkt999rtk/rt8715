#ifndef LWIP_TCP_WAKE_TRACE_H
#define LWIP_TCP_WAKE_TRACE_H
#include "lwip/opt.h"
/* The customer mono archive imports these hooks even when tracing is off. */
void tcp_wake_poll_begin(int fd);
void tcp_wake_poll_end(int fd, int result, int revents);
#if CONFIG_SCREEN_WAKE_TRACE
#include "lwip/arch.h"
struct tcp_pcb;
#define TCP_WAKE_SLOTS 32U
enum { TCP_WAKE_WRITE_OK = 1, TCP_WAKE_WRITE_ERROR, TCP_WAKE_FRAME_END,
       TCP_WAKE_CLOSED, TCP_WAKE_REPLACED };
#define TCP_WAKE_SEEN_ACK    1U
#define TCP_WAKE_SEEN_READY  2U
#define TCP_WAKE_SEEN_PLUS   4U
#define TCP_WAKE_SEEN_SIGNAL 8U
#define TCP_WAKE_SEEN_POLL  16U
struct tcp_wake_record {
  u32_t id, connection, frame, start_ms, end_ms;
  u32_t ack_first_ms, ready_first_ms, plus_first_ms, signal_first_ms;
  u32_t poll_last_ms, write_start_ms;
  u32_t acks, advanced, freed, below, ready, sendplus, transitions, signals;
  u32_t eagain, polls, timeouts, poll_errors, poll_ready;
  u32_t matched_signals, signal_to_poll_max_ms, poll_to_write_max_ms, write_max_ms;
  u32_t sndbuf, qlen, wnd, cwnd, ackno;
  s32_t fd, result, error, poll_result, revents;
  u16_t local_port, remote_port, waiters;
  u8_t reason, seen;
};
/* Track/forget/ack run in TCP core context. All shared observer state is
 * protected; no PCB pointer is dereferenced by the reporter or sender hooks. */
void tcp_wake_track(struct tcp_pcb *pcb, int fd);
void tcp_wake_forget(struct tcp_pcb *pcb);
void tcp_wake_ack(struct tcp_pcb *pcb, u32_t ackno, u32_t advanced,
                  u32_t freed, u32_t input_ms);
void tcp_wake_socket_event(int fd, int plus, int already_ready, unsigned waiters);
void tcp_wake_signal(int fd);
void tcp_wake_write_begin(int fd, u32_t frame);
void tcp_wake_write_end(int fd, int result, int error);
void tcp_wake_frame_end(int fd);
int tcp_wake_pop(struct tcp_wake_record *out, u32_t *overwritten);
int tcp_wake_active(struct tcp_wake_record *out);
#endif
#endif
