/* Verify observer correlation and wrap/lifetime handling, not TCP policy. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lwip/priv/tcp_wake_trace.h"
#include "lwip/tcp.h"
#include <errno.h>
static u32_t now;
u32_t sys_now(void) { return now; }
static struct tcp_pcb pcb;
static void begin(u32_t start) {
  now = start;
  tcp_wake_track(&pcb, 7);
  tcp_wake_write_begin(7, 42);
  tcp_wake_socket_event(7, 0, 0, 0);
  tcp_wake_write_end(7, -1, EWOULDBLOCK);
}
static void episode(u32_t start) {
  struct tcp_wake_record r; u32_t lost;
  begin(start);
  now = start + 2; tcp_wake_poll_begin(7);
  now = start + 5; pcb.snd_buf = 1000; pcb.snd_queuelen = 20;
  tcp_wake_ack(&pcb, 2000, 1000, 1000, now);
  now = start + 12; pcb.snd_buf = TCP_SND_BUF; pcb.snd_queuelen = 0;
  tcp_wake_ack(&pcb, 3000, 1000, 1000, now);
  tcp_wake_socket_event(7, 1, 0, 1);
  now = start + 13; tcp_wake_signal(7);
  now = start + 20; tcp_wake_poll_end(7, 1, 2);
  now = start + 21; tcp_wake_write_begin(7, 42);
  now = start + 23; tcp_wake_write_end(7, 500, 0);
  assert(tcp_wake_pop(&r, &lost) && !lost);
  assert(r.reason == TCP_WAKE_WRITE_OK && r.frame == 42 && r.fd == 7);
  assert(r.eagain == 1 && r.acks == 2 && r.freed == 2000 && r.below == 1);
  assert(r.ack_first_ms == start + 5 && r.ready_first_ms == start + 12);
  assert(r.signal_to_poll_max_ms == 7 && r.poll_to_write_max_ms == 1);
  assert(r.write_max_ms == 2 && r.end_ms - r.start_ms == 23);
  assert(r.polls == 1 && r.signals == 1 && r.sendplus == 1);
  assert(!tcp_wake_pop(&r, &lost));
  assert(!tcp_wake_active(&r));
  tcp_wake_forget(&pcb);
}
int main(void) {
  struct tcp_wake_record r; u32_t lost, conn; unsigned i;
  memset(&pcb, 0, sizeof(pcb));
  pcb.local_port = 40000; pcb.remote_port = 50000;
  episode(100); episode(0xfffffff0U);
  begin(1000); tcp_wake_active(&r); conn = r.connection;
  now = 1002; tcp_wake_poll_begin(7);
  now = 1252; tcp_wake_poll_end(7, 0, 0);
  assert(tcp_wake_active(&r) && r.timeouts == 1 && r.end_ms == 1252);
  assert(!(r.seen & TCP_WAKE_SEEN_SIGNAL));
  tcp_wake_forget(&pcb);
  assert(tcp_wake_pop(&r, &lost) && r.reason == TCP_WAKE_CLOSED);
  /* Same fd/address gets a fresh connection ID, no stale signal/ACK. */
  begin(1300); assert(tcp_wake_active(&r) && r.connection != conn);
  tcp_wake_socket_event(8, 1, 0, 1); tcp_wake_signal(8);
  now = 1305; tcp_wake_frame_end(7);
  assert(tcp_wake_pop(&r, &lost) && !r.sendplus && !r.signals);
  /* Normal partial write without EAGAIN must not flood the record ring. */
  tcp_wake_write_begin(7, 43); tcp_wake_socket_event(7, 0, 0, 0);
  tcp_wake_write_end(7, 10, 0); assert(!tcp_wake_pop(&r, &lost));
  for(i=0;i<TCP_WAKE_SLOTS+3;i++) {
    begin(2000+i*10); tcp_wake_write_end(7, -1, EIO);
  }
  assert(tcp_wake_pop(&r, &lost) && lost == 3 && r.reason == TCP_WAKE_WRITE_ERROR);
  puts("PASS wake trace: ACK threshold, signal/poll/write, timeout, wrap, reuse, overflow");
  return 0;
}
