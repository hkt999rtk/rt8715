#!/usr/bin/env python3
"""Compile the real netconn receive function against deterministic mailbox mocks.

Run directly from any directory. An optional api_lib.c path permits checking
that this regression test rejects an older source version.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[6]
SOURCE = REPO / "component/common/network/lwip/lwip_v2.1.2/src/api/api_lib.c"

MOCKS = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
typedef int err_t;
typedef uint8_t u8_t;
typedef uint16_t u16_t;
struct pbuf { u16_t tot_len; };
struct netconn {
  unsigned flags;
  int pending_err, nonblocking, recvmbox, recv_timeout, waiting, recv_avail;
};
#define LWIP_ERROR(m,c,a) do { if (!(c)) { a; } } while (0)
#define NETCONN_RECVMBOX_WAITABLE(c) 1
#define NETCONN_MBOX_WAITING_INC(c) (++(c)->waiting)
#define NETCONN_MBOX_WAITING_DEC(c) (--(c)->waiting)
#define netconn_is_nonblocking(c) ((c)->nonblocking)
#define NETCONN_DONTBLOCK 1
#define NETCONN_FLAG_MBOXCLOSED 2
#define ERR_OK 0
#define ERR_ARG -1
#define ERR_CONN -2
#define ERR_WOULDBLOCK -3
#define ERR_TIMEOUT -4
#define ERR_CLSD -5
#define SYS_ARCH_TIMEOUT UINT32_MAX
#define LWIP_TCP 1
#define LWIP_UDP 0
#define LWIP_RAW 0
#define LWIP_NETCONN_FULLDUPLEX 0
#define LWIP_SO_RCVBUF 1
#define SYS_ARCH_DEC(v,n) ((v) -= (n))
#define API_EVENT(c,e,l) do { events++; event_bytes += (l); } while (0)
#define LWIP_DEBUGF(d,m) ((void)0)
static unsigned waits, timeout_arg, events, event_bytes;
static int deliver_on_wait;
static struct pbuf packet = {190};
static err_t netconn_err(struct netconn *c) { return c->pending_err; }
static unsigned sys_arch_mbox_tryfetch(int *box, void **p)
{
  if (!*box) return SYS_ARCH_TIMEOUT;
  *box = 0; *p = &packet; return 0;
}
static unsigned sys_arch_mbox_fetch(int *box, void **p, unsigned timeout)
{
  waits++; timeout_arg = timeout;
  if (deliver_on_wait) { *box = 1; return sys_arch_mbox_tryfetch(box, p); }
  return SYS_ARCH_TIMEOUT;
}
static int lwip_netconn_is_err_msg(void *p, err_t *e)
{ (void)p; (void)e; return 0; }
'''

TESTS = r'''
static err_t receive(struct netconn *c, void **p, u8_t flags)
{
  err_t result = netconn_recv_data(c, p, flags);
  assert(c->waiting == 0);
  return result;
}
int main(void)
{
  struct netconn c = {0};
  void *p = &packet;
  /* Per-call DONTBLOCK and socket-level nonblocking both must return now. */
  assert(receive(&c, &p, NETCONN_DONTBLOCK) == ERR_WOULDBLOCK);
  assert(!p && waits == 0);
  c.nonblocking = 1; c.recv_timeout = 37;
  assert(receive(&c, &p, 0) == ERR_WOULDBLOCK && waits == 0);

  /* Read existing payload, then probe for more with DONTBLOCK (recv loop). */
  c.nonblocking = 0; c.recvmbox = 1; c.recv_avail = packet.tot_len;
  assert(receive(&c, &p, NETCONN_DONTBLOCK) == ERR_OK && p == &packet);
  assert(c.recv_avail == 0 && events == 1 && event_bytes == packet.tot_len);
  assert(receive(&c, &p, NETCONN_DONTBLOCK) == ERR_WOULDBLOCK);
  assert(!p && waits == 0);
  /* Data arriving after EAGAIN remains available for the caller's next read. */
  c.recvmbox = 1; c.recv_avail = packet.tot_len;
  assert(receive(&c, &p, NETCONN_DONTBLOCK) == ERR_OK && p == &packet);
  assert(waits == 0 && events == 2);

  /* Closed/error paths must not start a new mailbox wait either. */
  c.flags = NETCONN_FLAG_MBOXCLOSED;
  assert(receive(&c, &p, 0) == ERR_CONN && waits == 0);
  c.flags = 0; c.pending_err = ERR_ARG;
  assert(receive(&c, &p, 0) == ERR_ARG && waits == 0);
  c.pending_err = ERR_OK;

  /* Blocking timeout and blocking delivery retain their original contract. */
#if LWIP_SO_RCVTIMEO
  assert(receive(&c, &p, 0) == ERR_TIMEOUT && !p);
  assert(waits == 1 && timeout_arg == 37);
#endif
  waits = 0; deliver_on_wait = 1; c.recv_timeout = 0;
  c.recv_avail = packet.tot_len;
  assert(receive(&c, &p, 0) == ERR_OK && p == &packet);
  assert(waits == 1 && timeout_arg == 0 && c.recv_avail == 0);
  puts("PASS: nonblocking, partial-read follow-up, late arrival, errors, blocking receive");
}
'''


def main():
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else SOURCE
    contents = source.read_text()
    start = contents.index("static err_t\nnetconn_recv_data(")
    end = contents.index("\n#if LWIP_TCP\nstatic err_t", start)
    with tempfile.TemporaryDirectory(prefix="nonblocking-recv-") as directory:
        root = Path(directory)
        test = root / "test.c"
        test.write_text(MOCKS + contents[start:end] + TESTS)
        for timeout_enabled in (1, 0):
            binary = root / "test"
            subprocess.run([
                "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                f"-DLWIP_SO_RCVTIMEO={timeout_enabled}",
                str(test), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    main()
