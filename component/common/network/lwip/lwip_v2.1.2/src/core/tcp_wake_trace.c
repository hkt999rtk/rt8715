/* Bounded screen ACK -> writable notification -> sender observation.
 * No allocation, printing, callbacks or TCP state changes. Times use sys_now
 * throughout, including across wrap. One screen sender/connection at a time.
 */
#include "lwip/priv/tcp_wake_trace.h"
#if CONFIG_SCREEN_WAKE_TRACE
#include "lwip/tcp.h"
#include "lwip/sys.h"
#include <errno.h>
#include <string.h>

static struct tcp_pcb *tracked;
static int tracked_fd = -1;
static u32_t connection, next_id, frame, write_start;
static u16_t local_port, remote_port;
static struct tcp_wake_record current, records[TCP_WAKE_SLOTS];
static u32_t overwritten, poll_signal_ms;
static u8_t active, head, count, poll_waiting, poll_signalled, after_poll;

/* All helpers below are called with SYS_ARCH_PROTECT held. */
static void start(void)
{
  if (active || !frame) return;
  memset(&current, 0, sizeof(current));
  current.id = ++next_id; current.connection = connection;
  current.fd = tracked_fd; current.frame = frame;
  current.local_port = local_port; current.remote_port = remote_port;
  current.start_ms = sys_now();
  active = 1; poll_waiting = poll_signalled = after_poll = 0;
}
static void finish(u8_t reason)
{
  if (!active) return;
  current.reason = reason; current.end_ms = sys_now();
  /* SENDMINUS also occurs on successful partial writes. Keep only episodes
   * for which the sender actually observed EAGAIN, avoiding normal-path logs. */
  if (current.eagain) {
    if (count == TCP_WAKE_SLOTS) {
      head = (head + 1U) % TCP_WAKE_SLOTS; --count; ++overwritten;
    }
    records[(head + count) % TCP_WAKE_SLOTS] = current;
    ++count;
  }
  active = poll_waiting = poll_signalled = after_poll = 0;
}
static void maximum(u32_t *dest, u32_t value) { if (value > *dest) *dest = value; }

void tcp_wake_track(struct tcp_pcb *pcb, int fd)
{
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  if (pcb != tracked || fd != tracked_fd) {
    finish(TCP_WAKE_REPLACED);
    tracked = pcb; tracked_fd = fd; frame = 0;
    if (++connection == 0) ++connection;
    local_port = pcb->local_port; remote_port = pcb->remote_port;
  }
  SYS_ARCH_UNPROTECT(level);
}
void tcp_wake_forget(struct tcp_pcb *pcb)
{
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  if (pcb == tracked) {
    finish(TCP_WAKE_CLOSED); tracked = NULL; tracked_fd = -1; frame = 0;
  }
  SYS_ARCH_UNPROTECT(level);
}
void tcp_wake_ack(struct tcp_pcb *pcb, u32_t ackno, u32_t advanced,
                 u32_t freed, u32_t input_ms)
{
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  if (active && pcb == tracked) {
    ++current.acks; current.advanced += advanced; current.freed += freed;
    if (!(current.seen & TCP_WAKE_SEEN_ACK)) {
      current.ack_first_ms = input_ms; current.seen |= TCP_WAKE_SEEN_ACK;
    }
    current.sndbuf = pcb->snd_buf; current.qlen = pcb->snd_queuelen;
    current.wnd = pcb->snd_wnd; current.cwnd = pcb->cwnd; current.ackno = ackno;
    if (tcp_sndbuf(pcb) > TCP_SNDLOWAT && pcb->snd_queuelen < TCP_SNDQUEUELOWAT) {
      ++current.ready;
      if (!(current.seen & TCP_WAKE_SEEN_READY)) {
        current.ready_first_ms = sys_now(); current.seen |= TCP_WAKE_SEEN_READY;
      }
    } else ++current.below;
  }
  SYS_ARCH_UNPROTECT(level);
}
void tcp_wake_socket_event(int fd, int plus, int already_ready, unsigned waiters)
{
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  if (fd == tracked_fd && tracked != NULL) {
    if (!plus) start();
    if (active && plus) {
      ++current.sendplus; current.transitions += !already_ready;
      current.waiters = (u16_t)waiters;
      if (!(current.seen & TCP_WAKE_SEEN_PLUS)) {
        current.plus_first_ms = sys_now(); current.seen |= TCP_WAKE_SEEN_PLUS;
      }
    }
  }
  SYS_ARCH_UNPROTECT(level);
}
void tcp_wake_signal(int fd)
{
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  if (active && fd == tracked_fd) {
    u32_t now = sys_now();
    ++current.signals;
    if (!(current.seen & TCP_WAKE_SEEN_SIGNAL)) {
      current.signal_first_ms = now; current.seen |= TCP_WAKE_SEEN_SIGNAL;
    }
    if (poll_waiting && !poll_signalled) { poll_signal_ms = now; poll_signalled = 1; }
  }
  SYS_ARCH_UNPROTECT(level);
}
void tcp_wake_poll_begin(int fd)
{
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  if (active && fd == tracked_fd) { poll_waiting = 1; poll_signalled = 0; }
  SYS_ARCH_UNPROTECT(level);
}
void tcp_wake_poll_end(int fd, int result, int revents)
{
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  if (active && fd == tracked_fd) {
    current.poll_last_ms = sys_now(); current.seen |= TCP_WAKE_SEEN_POLL;
    current.poll_result = result; current.revents = revents;
    ++current.polls; current.timeouts += result == 0;
    current.poll_errors += result < 0 || (revents & 0x20c) != 0;
    current.poll_ready += result > 0 && (revents & 2) != 0; /* lwIP POLLOUT */
    if (poll_waiting && poll_signalled) {
      ++current.matched_signals;
      maximum(&current.signal_to_poll_max_ms, current.poll_last_ms - poll_signal_ms);
    }
    poll_waiting = 0; after_poll = 1;
  }
  SYS_ARCH_UNPROTECT(level);
}
void tcp_wake_write_begin(int fd, u32_t frame_id)
{
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  if (fd == tracked_fd && tracked != NULL) {
    frame = frame_id; write_start = sys_now();
    if (active) {
      current.write_start_ms = write_start;
      if (after_poll) maximum(&current.poll_to_write_max_ms,
                             write_start - current.poll_last_ms);
      after_poll = 0;
    }
  }
  SYS_ARCH_UNPROTECT(level);
}
void tcp_wake_write_end(int fd, int result, int error)
{
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  if (fd == tracked_fd && tracked != NULL) {
    if (result < 0 && (error == EAGAIN || error == EWOULDBLOCK)) {
      start(); if (active) ++current.eagain;
    }
    if (active) {
      current.result = result; current.error = error;
      maximum(&current.write_max_ms, sys_now() - write_start);
      if (result > 0) finish(TCP_WAKE_WRITE_OK);
      else if (result == 0 || (error != EAGAIN && error != EWOULDBLOCK))
        finish(TCP_WAKE_WRITE_ERROR);
    }
  }
  SYS_ARCH_UNPROTECT(level);
}
void tcp_wake_frame_end(int fd)
{
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  if (fd == tracked_fd) { finish(TCP_WAKE_FRAME_END); frame = 0; }
  SYS_ARCH_UNPROTECT(level);
}
int tcp_wake_pop(struct tcp_wake_record *out, u32_t *lost)
{
  int found;
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  *lost = overwritten; found = count != 0;
  if (found) { *out = records[head]; head = (head + 1U) % TCP_WAKE_SLOTS; --count; }
  SYS_ARCH_UNPROTECT(level);
  return found;
}
int tcp_wake_active(struct tcp_wake_record *out)
{
  int found;
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  found = active && current.eagain;
  if (found) { *out = current; out->end_ms = sys_now(); }
  SYS_ARCH_UNPROTECT(level);
  return found;
}
#else
/* ABI required by the customer mono archive. Keep production diagnostics
 * disabled: these calls allocate nothing and do not touch TCP state. */
void tcp_wake_poll_begin(int fd)
{
  (void)fd;
}
void tcp_wake_poll_end(int fd, int result, int revents)
{
  (void)fd;
  (void)result;
  (void)revents;
}
#endif
