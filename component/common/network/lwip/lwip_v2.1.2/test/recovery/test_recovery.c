/* Packet injection tests use the production TCP input/output/timer code. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/tcp_recovery.h"
#include "lwip/priv/tcp_wake_trace.h"
#include <errno.h>
#include "lwip/ip.h"
#include "lwip/timeouts.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"

static u32_t now_ms = 10;
static int clock_runs;
u32_t sys_now(void) { u32_t t = now_ms; if (clock_runs) ++now_ms; return t; }
static struct netif nif;
struct netif eth_netif[4];
static u32_t sent[512];
static unsigned nsend;
static int reject_output;
static const char payload[24000] = {1};

static err_t output(struct netif *netif, struct pbuf *p, const ip4_addr_t *dst)
{
  unsigned char h[60];
  struct tcp_hdr *t;
  if (reject_output) return ERR_MEM;
  assert(pbuf_copy_partial(p, h, sizeof(h), 0) >= 40);
  t = (struct tcp_hdr *)(h + (h[0] & 15) * 4);
  if (p->tot_len > (h[0] & 15) * 4 + TCPH_HDRLEN_BYTES(t)) {
    assert(nsend < 512);
    sent[nsend++] = lwip_ntohl(t->seqno);
  }
  return ERR_OK;
}
static err_t netinit(struct netif *n) { n->output = output; n->mtu = 1500; return ERR_OK; }
static struct tcp_pcb *connection(unsigned window, unsigned segments, u32_t base)
{
  struct tcp_pcb *p = tcp_new();
  assert(p);
  p->state = ESTABLISHED;
  p->local_ip = *netif_ip_addr4(&nif);
  IP_ADDR4(&p->remote_ip, 192,168,1,2);
  p->local_port = 40000; p->remote_port = 50000;
  p->snd_nxt = p->snd_lbb = p->lastack = p->snd_wl2 = base;
  p->rcv_nxt = p->snd_wl1 = 100;
  p->mss = 1000; p->cwnd = window * 1000; p->ssthresh = 32000;
  p->snd_wnd = p->snd_wnd_max = 32000;
  tcp_nagle_disable(p);
  TCP_REG_ACTIVE(p);
  nsend = 0;
  if (segments) {
    assert(tcp_write(p, payload, segments * 1000, TCP_WRITE_FLAG_COPY) == ERR_OK);
    assert(tcp_output(p) == ERR_OK);
  }
  return p;
}
static void ack_flags(struct tcp_pcb *pcb, u32_t seq, u16_t wnd, u8_t flags)
{
  struct pbuf *b = pbuf_alloc(PBUF_RAW, 40, PBUF_RAM);
  struct ip_hdr *ip = b->payload;
  struct tcp_hdr *t = (void *)((char *)b->payload + 20);
  assert(b); memset(b->payload, 0, 40);
  IPH_VHL_SET(ip, 4, 5); IPH_LEN_SET(ip, lwip_htons(40));
  IPH_PROTO_SET(ip, IP_PROTO_TCP); IPH_TTL_SET(ip, 64);
  ip->src.addr = ip_2_ip4(&pcb->remote_ip)->addr; ip->dest.addr = ip_2_ip4(&pcb->local_ip)->addr;
  t->src = lwip_htons(pcb->remote_port); t->dest = lwip_htons(pcb->local_port);
  t->seqno = lwip_htonl(pcb->rcv_nxt); t->ackno = lwip_htonl(seq);
  TCPH_HDRLEN_FLAGS_SET(t, 5, flags); t->wnd = lwip_htons(wnd);
  assert(ip4_input(b, &nif) == ERR_OK);
}
static void ack(struct tcp_pcb *p, u32_t seq, u16_t wnd) { ack_flags(p, seq, wnd, TCP_ACK); }
static void advance(unsigned ms)
{
  while (ms) { unsigned step = ms < 10 ? ms : 10; now_ms += step; ms -= step; sys_check_timeouts(); }
}
static void limited(void)
{
  struct tcp_pcb *p = connection(2, 8, 1000);
  assert(nsend == 2);
  ack(p, 1000, 32000);
  assert(nsend == 3 && sent[2] == 3000 && p->cwnd == 2000);
  ack(p, 1000, 32000);
  assert(nsend == 4 && sent[3] == 4000 && p->cwnd == 2000);
  ack(p, 1000, 32000);
  assert(sent[4] == 1000 && (p->flags & TF_INFR));
  assert(p->ssthresh == 2000); /* LT bytes excluded. */
  ack(p, p->snd_nxt, 32000);
  assert(!(p->flags & TF_INFR));
  tcp_abort(p);
}
static void partial(void)
{
  struct tcp_pcb *p = connection(6, 6, 1000);
  ack(p, 1000, 32000); ack(p, 1000, 32000); ack(p, 1000, 32000);
  assert(nsend == 7 && sent[6] == 1000);
  ack(p, 2000, 32000);
  assert((p->flags & TF_INFR) && nsend == 8 && sent[7] == 2000);
  ack(p, 4000, 32000);
  assert((p->flags & TF_INFR) && nsend == 9 && sent[8] == 4000);
  assert(p->ssthresh == 3000);
  ack(p, 7000, 32000);
  assert(!(p->flags & TF_INFR) && p->cwnd <= p->ssthresh);
  tcp_abort(p);
}
static void rto(void)
{
  struct tcp_pcb *p = connection(2, 1, 1000);
  advance(5); ack(p, 2000, 32000); /* millisecond RTT sample */
  assert(tcp_write(p, payload, 1000, TCP_WRITE_FLAG_COPY) == ERR_OK);
  assert(tcp_output(p) == ERR_OK); assert(nsend == 2);
  advance(999); assert(nsend == 2);
  advance(51); assert(nsend == 3 && sent[2] == 2000);
  advance(1900); assert(nsend == 3);
  advance(150); assert(nsend == 4 && sent[3] == 2000);
  tcp_abort(p);
}
static void busy(void)
{
  struct tcp_pcb *p = connection(6, 6, 1000);
  struct pbuf *held = p->unacked->p;
  pbuf_ref(held);
  ack(p, 1000, 32000); ack(p, 1000, 32000); ack(p, 1000, 32000);
  assert(nsend == 6 && !(p->flags & TF_INFR));
  pbuf_free(held);
  advance(60);
  assert(nsend == 7 && sent[6] == 1000 && (p->flags & TF_INFR));
  tcp_abort(p);
}
static void many_partial(void)
{
  struct tcp_pcb *p = connection(20, 20, 1000);
  unsigned i;
  ack(p, 1000, 32000); ack(p, 1000, 32000); ack(p, 1000, 32000);
  for (i = 1; i < 20; ++i) {
    ack(p, 1000 + i * 1000, 32000);
    assert(p->flags & TF_INFR);
    assert(p->nrtx <= 1); /* progress must not consume retry-abort budget */
  }
  ack(p, 21000, 32000); assert(!(p->flags & TF_INFR)); tcp_abort(p);
}
static void queued_busy(void)
{
  struct tcp_pcb *p = connection(2, 0, 1000);
  struct pbuf *held;
  assert(tcp_write(p, payload, 1000, TCP_WRITE_FLAG_COPY) == ERR_OK);
  held = p->unsent->p; pbuf_ref(held);
  assert(tcp_output(p) == ERR_MEM);
  assert(nsend == 0 && p->unsent && !p->unacked && p->snd_nxt == 1000);
  pbuf_free(held); advance(60);
  assert(nsend == 1 && sent[0] == 1000); tcp_abort(p);
}
static void window_reorder(void)
{
  struct tcp_pcb *p = connection(2, 8, 1000);
  p->snd_wnd = 2000; /* peer window limits flight to the two segments */
  ack(p, 1000, 2000); ack(p, 1000, 2000);
  assert(nsend == 2 && p->cwnd == 2000 && !(p->flags & TF_INFR));
  ack(p, 3000, 32000); /* reordering resolved, no retransmission needed */
  assert(!(p->flags & TF_INFR) && p->ssthresh == 32000);
  tcp_abort(p);
}
static void partial_busy(void)
{
  struct tcp_pcb *p = connection(6, 6, 1000);
  struct pbuf *held;
  ack(p, 1000, 32000); ack(p, 1000, 32000); ack(p, 1000, 32000);
  held = p->unacked->next->p; pbuf_ref(held);
  ack(p, 2000, 32000);
  assert((p->flags & TF_INFR) && nsend == 7);
  pbuf_free(held); advance(60);
  assert(nsend == 8 && sent[7] == 2000);
  tcp_abort(p);
}
static void wrap(void)
{
  u32_t base = 0xfffff000U;
  struct tcp_pcb *p = connection(6, 6, base);
  ack(p, base, 32000); ack(p, base, 32000); ack(p, base, 32000);
  ack(p, base + 5000U, 32000);
  assert(nsend == 8 && sent[7] == base + 5000U && (p->flags & TF_INFR));
  ack(p, base + 6000U, 32000); assert(!(p->flags & TF_INFR)); tcp_abort(p);
  rto(); /* also cross sys_now() wrap with a running retransmission timer */
}
static void handshake_idle(void)
{
  struct tcp_pcb *p = tcp_new();
  ip_addr_t remote;
  IP_ADDR4(&remote,192,168,1,2); nsend = 0;
  assert(tcp_bind(p, netif_ip_addr4(&nif), 40000) == ERR_OK);
  assert(tcp_connect(p, &remote, 50000, NULL) == ERR_OK);
  advance(10); ack_flags(p, p->snd_nxt, 32000, TCP_SYN | TCP_ACK);
  assert(p->state == ESTABLISHED);
  advance(10000);
  assert(tcp_write(p, payload, 500, TCP_WRITE_FLAG_COPY) == ERR_OK);
  assert(tcp_output(p) == ERR_OK); advance(5); ack(p, p->snd_nxt, 32000);
#if LWIP_TCP_RECOVERY
  assert(p->recovery.rto_ms == 1000); /* idle time is not an RTT sample */
#endif
  tcp_abort(p);
}
static void boundary(void)
{
  struct tcp_pcb *p = connection(6, 12, 1000);
  u32_t high = p->snd_nxt;
  ack(p, 1000, 32000); ack(p, 1000, 32000); ack(p, 1000, 32000);
  /* LT may have extended the recovery boundary. */
#if LWIP_TCP_RECOVERY
  high = p->recovery.recover_end;
#endif
  ack(p, high, 32000); assert(!(p->flags & TF_INFR));
  ack(p, high, 32000); ack(p, high, 32000); ack(p, high, 32000);
  assert(!(p->flags & TF_INFR)); /* old duplicated ACKs at the prior boundary */
  ack(p, high + 1000, 32000);
  ack(p, high + 1000, 32000); ack(p, high + 1000, 32000); ack(p, high + 1000, 32000);
  assert(p->flags & TF_INFR); /* progress beyond boundary permits new recovery */
  tcp_abort(p);
}
static void moving_clock(void)
{
  struct tcp_pcb *p = connection(6, 6, 1000);
  struct pbuf *held = p->unacked->p;
  pbuf_ref(held);
  ack(p, 1000, 32000); ack(p, 1000, 32000); ack(p, 1000, 32000);
  pbuf_free(held);
  clock_runs = 1; advance(60); clock_runs = 0;
  assert((p->flags & TF_INFR) && p->cwnd > p->mss);
#if LWIP_TCP_RECOVERY
  { struct tcp_recovery_stats stats; tcp_recovery_snapshot(&stats); assert(stats.rto == 0); }
#endif
  tcp_abort(p);
}
static void output_failure(void)
{
  struct tcp_pcb *p = connection(6, 6, 1000);
  ack(p, 1000, 32000); ack(p, 1000, 32000);
  reject_output = 1; ack(p, 1000, 32000);
  assert(nsend == 6 && p->unsent && (p->flags & TF_INFR));
  reject_output = 0; advance(60);
  assert(nsend == 7 && sent[6] == 1000);
  tcp_abort(p);
}
static void karn(void)
{
  struct tcp_pcb *p = connection(6, 6, 1000);
  ack(p, 1000, 32000); ack(p, 1000, 32000); ack(p, 1000, 32000);
  advance(250); ack(p, 7000, 32000);
#if LWIP_TCP_RECOVERY
  assert(p->recovery.rto_ms == 3000 && p->recovery.srtt_x8 == 0);
#endif
  assert(tcp_write(p, payload, 1000, TCP_WRITE_FLAG_COPY) == ERR_OK);
  assert(tcp_output(p) == ERR_OK); advance(5); ack(p, 8000, 32000);
#if LWIP_TCP_RECOVERY
  assert(p->recovery.rto_ms == 1000 && p->recovery.srtt_x8 == 40);
#endif
  tcp_abort(p);
}
static void timer_cadence(void)
{
  struct tcp_pcb *p = connection(2, 0, 1000);
  u32_t ticks = tcp_ticks;
  advance(1000); assert(tcp_ticks - ticks == 2); tcp_abort(p);
}
static void zero_window(void)
{
  struct tcp_pcb *p = connection(2, 0, 1000);
  p->snd_wnd = 0;
  assert(tcp_write(p,payload,4000,TCP_WRITE_FLAG_COPY) == ERR_OK);
  assert(tcp_output(p) == ERR_OK); assert(nsend == 0 && p->persist_backoff);
  advance(1000); assert(nsend == 0); /* first persist slot is longer */
  ack(p,1000,32000); assert(nsend == 2 && !p->persist_backoff);
  tcp_abort(p);
}
static void partial_dupack(void)
{
  struct tcp_pcb *p = connection(6, 6, 1000);
  tcpwnd_size_t cwnd;
  ack(p,1000,32000); ack(p,1000,32000); ack(p,1000,32000);
  ack(p,2000,32000); cwnd = p->cwnd;
  ack(p,2000,32000);
  assert((p->flags & TF_INFR) && p->cwnd == cwnd + p->mss);
  tcp_abort(p);
}

#if CONFIG_TCP_RECOVERY_TRACE
static void trace_fast(void)
{
  struct tcp_pcb *p = connection(6, 6, 1000);
  struct tcp_recovery_event e;
  u32_t lost;
  unsigned fast = 0, partial_event = 0, full = 0, retx = 0;
  ack(p, 1000, 32000); ack(p, 1000, 32000); ack(p, 1000, 32000);
  ack(p, 2000, 32000); ack(p, 3000, 32000); ack(p, 7000, 32000);
  while (tcp_recovery_trace_pop(0, &e, &lost)) {
    assert(!lost && e.connection == p->recovery.trace.connection);
    if (e.kind == TCP_TRACE_FAST) {
      ++fast; assert((e.flags & TF_INFR) && e.recover_end == 7000);
    }
    if (e.kind == TCP_TRACE_PARTIAL) {
      ++partial_event; assert(e.partial_count == 1 && e.lastack == 2000);
    }
    if (e.kind == TCP_TRACE_FULL) {
      ++full; assert(e.partial_count == 2 && !(e.flags & TF_INFR));
    }
    if (e.kind == TCP_TRACE_RETX) {
      ++retx; assert(e.tx_seq == retx * 1000 && e.tx_age_ms == 0);
    }
  }
  assert(fast == 1 && partial_event == 1 && full == 1 && retx == 3);
  assert(!tcp_recovery_trace_pop(1, &e, &lost));
  tcp_abort(p);
}

/* Freeze evidence across normal-ring overflow and later PCB reuse. */
static void trace_cause(void)
{
  struct tcp_pcb *p = connection(6, 6, 1000);
  struct tcp_recovery_cause c;
  u32_t dropped, evicted, cid = p->recovery.trace.connection;
  ack(p,1000,32000); ack(p,1000,32000); ack(p,1000,32000);
  ack(p,7000,32000); /* full recovery at exclusive boundary */
  assert(!(p->flags & TF_INFR) && p->recovery.recover_end == 7000);
  assert(tcp_write(p,payload,2000,TCP_WRITE_FLAG_COPY) == ERR_OK);
  assert(tcp_output(p) == ERR_OK);
  ack(p,7000,32000); ack(p,7000,32000); ack(p,7000,32000);
  assert(!(p->flags & TF_INFR));
  for (unsigned i=0;i<100;++i) tcp_recovery_output_error(p);
  p->recovery.output_pending = 0;
  advance(p->recovery.rto_ms + 50);
  ack(p,8000,32000); /* must not mutate already-frozen evidence */
  tcp_abort(p);
  assert(tcp_recovery_cause_pop(&c,&dropped,&evicted));
  assert(!dropped && !evicted && c.full.connection == cid);
  assert(c.full.kind == TCP_TRACE_FULL && c.full.lastack == 7000);
  assert(c.skip.skip_reason == TCP_SKIP_BOUNDARY && c.skip.lastack == 7000);
  assert(c.skips == 1 && c.ack_packets == 3 && c.progress_calls == 0);
  assert(c.due.kind == TCP_TRACE_RTO_DUE && c.due.lastack == 7000);
  assert(c.due.timer_age_ms >= c.due.rto_ms);
  assert(!tcp_recovery_cause_pop(&c,&dropped,&evicted));
  /* ACK progress after FULL and before RTO must survive later mutations. */
  p=connection(6,6,1000);
  ack(p,1000,32000); ack(p,1000,32000); ack(p,1000,32000);
  ack(p,7000,32000);
  assert(tcp_write(p,payload,4000,TCP_WRITE_FLAG_COPY) == ERR_OK);
  assert(tcp_output(p) == ERR_OK);
  ack(p,8000,32000);
  advance(p->recovery.rto_ms+50);
  assert(tcp_recovery_cause_pop(&c,&dropped,&evicted));
  assert(c.full.lastack == 7000 && c.due.lastack == 8000);
  assert(c.ack_packets == 1 && c.progress_calls == 1 && c.progress_bytes == 1000);
  assert(c.progress_ack == 8000 && c.skip.id == 0);
  tcp_abort(p);
  /* No previous full recovery must be represented explicitly by full.id=0.
   * Freeze FIFO preserves earliest entries and counts dropped NEW entries. */
  for (unsigned i=0;i<TCP_RECOVERY_CAUSE_SLOTS+2;++i) {
    p=connection(1,1,1000); advance(p->recovery.rto_ms+50); tcp_abort(p);
  }
  for (unsigned i=0;i<TCP_RECOVERY_CAUSE_SLOTS;++i) {
    assert(tcp_recovery_cause_pop(&c,&dropped,&evicted));
    assert(c.full.id == 0 && c.skip.id == 0 && c.due.connection != cid);
    assert(dropped == 2);
  }
  assert(!tcp_recovery_cause_pop(&c,&dropped,&evicted));
}

static void trace_evidence(void)
{
  struct tcp_pcb *p = connection(2, 2, 1000);
  struct tcp_recovery_event e;
  u32_t lost, cid = p->recovery.trace.connection;
  unsigned due = 0, arm = 0, tx = 0, progress = 0, events = 0;
  assert(cid != 0);
  /* Equal ACKs stay below fast-recovery threshold. Preserve arrival evidence. */
  ack(p, 1000, 32000);
  advance(3000);
  ack(p, 2000, 32000);
  while (tcp_recovery_trace_pop(1, &e, &lost)) {
    assert(!lost && e.connection == cid);
    assert(e.local_port == 40000 && e.remote_port == 50000);
    if (e.kind == TCP_TRACE_RTO_DUE) {
      due++;
      assert(e.rto_ms == 3000 && e.timer_age_ms >= 3000);
      assert(e.lastack == 1000 && e.unacked_seq == 1000);
      assert(e.ack_no == 1000 && e.ack_age_ms >= 3000);
      assert(e.dupacks == 1);
    } else if (e.kind == TCP_TRACE_RTO_ARM) {
      arm++;
      assert(e.rto_ms == 6000 && e.cwnd == 1000);
    } else if (e.kind == TCP_TRACE_RTO_TX) {
      tx++;
      assert(e.tx_seq == 1000 && e.tx_end == 2000 && e.tx_age_ms == 0);
    } else if (e.kind == TCP_TRACE_RTO_ACK) {
      progress++;
      assert(e.lastack == 2000 && e.ack_no == 2000 && e.ack_age_ms == 0);
    }
    events++;
  }
  assert(due == 1 && arm == 1 && tx == 1 && progress == 1 && events == 4);
  /* Noise may overwrite the normal ring, but must not displace RTO evidence. */
  advance(6000);
  for (unsigned i = 0; i < TCP_RECOVERY_TRACE_SLOTS + 9; ++i)
    tcp_recovery_output_error(p);
  assert(tcp_recovery_trace_pop(1, &e, &lost) && !lost);
  assert(e.kind == TCP_TRACE_RTO_DUE && e.connection == cid);
  events = 0;
  while (tcp_recovery_trace_pop(0, &e, &lost)) ++events;
  assert(events == TCP_RECOVERY_TRACE_SLOTS && lost >= 9);
  tcp_abort(p);
  /* A reused PCB address/port tuple must receive a new identity. */
  p = connection(1, 1, 1000);
  assert(p->recovery.trace.connection != cid);
  tcp_abort(p);
}
#endif

#if CONFIG_SCREEN_WAKE_TRACE
static void wake_ack_path(void)
{
  struct tcp_pcb *p = connection(6, 6, 1000);
  struct tcp_wake_record r;
  u32_t lost;
  tcp_wake_track(p, 7);
  tcp_wake_write_begin(7, 99);
  tcp_wake_socket_event(7, 0, 0, 0);
  tcp_wake_write_end(7, -1, EWOULDBLOCK);
  ack(p, 1000, 32000); /* Duplicate ACK frees nothing. */
  advance(5); ack(p, 2000, 32000);
  assert(tcp_wake_active(&r));
  assert(r.acks == 2 && r.advanced == 1000 && r.freed == 1000);
  assert(r.sndbuf == p->snd_buf && r.qlen == p->snd_queuelen);
  assert(r.ackno == 2000 && r.frame == 99);
  tcp_abort(p); /* Real PCB destruction invalidates the tracked connection. */
  assert(!tcp_wake_active(&r));
  assert(tcp_wake_pop(&r, &lost) && !lost && r.reason == TCP_WAKE_CLOSED);
}
#endif


#if LWIP_TCP_RECOVERY
/* Real ACK injection: RTO retransmits a window, then a second hole produces
 * duplicate ACKs below recover_end. Retaining either blanket guard breaks
 * these tests by leaving the hole waiting for the backed-off timer. */
static struct tcp_pcb *heuristic_connection(u32_t base)
{
  struct tcp_pcb *p=connection(10,1,base-1000U);
  advance(5); ack(p,base,32000);
  assert(p->recovery.rto_ms==1000);
  assert(tcp_write(p,payload,7237,TCP_WRITE_FLAG_COPY)==ERR_OK);
  assert(tcp_output(p)==ERR_OK);
  advance(1050);
  assert(p->flags&TF_RTO);
  return p;
}
static void three_acks(struct tcp_pcb *p, u32_t seq)
{
  ack(p,seq,32000); ack(p,seq,32000); ack(p,seq,32000);
}
static void heuristic_rto(u32_t base)
{
  struct tcp_pcb *p=heuristic_connection(base);
  unsigned before, i, copies;
  ack(p,base+1000U,32000); ack(p,base+2000U,32000);
  before=nsend;
  three_acks(p,base+2000U);
  assert((p->flags&TF_INFR) && !(p->flags&TF_RTO));
  assert(nsend>before && sent[before]==base+2000U);
  /* Extra duplicate ACKs must not retransmit this same hole again. */
  for(i=0;i<10;++i) ack(p,base+2000U,32000);
  copies=0; for(i=before;i<nsend;++i) if(sent[i]==base+2000U) ++copies;
  assert(copies==1);
  before=nsend;
  ack(p,base+3000U,32000);
  assert((p->flags&TF_INFR) && nsend>before && sent[before]==base+3000U);
  ack(p,base+7237U,32000);
  assert(!(p->flags&TF_INFR));
  tcp_abort(p);
}
static void heuristic_tail(void)
{
  struct tcp_pcb *p=heuristic_connection(1000);
  unsigned before,i;
  ack(p,2000,32000); ack(p,3000,32000);
  advance(2050); /* second RTO; rto_end now covers only this retransmit batch */
  for(i=4000;i<=8000;i+=1000) ack(p,i,32000);
  assert(!(p->flags&TF_RTO) && p->unacked->len==237);
  assert(tcp_write(p,payload,3000,TCP_WRITE_FLAG_COPY)==ERR_OK);
  assert(tcp_output(p)==ERR_OK);
  before=nsend;
  three_acks(p,8000);
  assert((p->flags&TF_INFR) && nsend>before && sent[before]==8000);
  ack(p,8237,32000);
  assert(p->flags&TF_INFR); /* further holes use normal NewReno partial ACKs */
  ack(p,p->snd_nxt,32000);
  assert(!(p->flags&TF_INFR));
  tcp_abort(p);
}
static void heuristic_busy(void)
{
  struct tcp_pcb *p=heuristic_connection(1000);
  struct pbuf *held;
  u32_t cwnd,ssthresh,timer;
  unsigned before;
  ack(p,2000,32000); ack(p,3000,32000);
  held=p->unacked->p; pbuf_ref(held);
  cwnd=p->cwnd; ssthresh=p->ssthresh; timer=p->recovery.timer_started_ms;
  before=nsend; three_acks(p,3000);
  assert(!(p->flags&TF_INFR) && (p->flags&TF_RTO));
  assert(p->cwnd==cwnd && p->ssthresh==ssthresh);
  assert(p->recovery.timer_started_ms==timer && nsend==before);
  pbuf_free(held); advance(60);
  assert((p->flags&TF_INFR) && !(p->flags&TF_RTO));
  assert(nsend>before && sent[before]==3000);
  tcp_abort(p);
}
static void heuristic_output(void)
{
  struct tcp_pcb *p=heuristic_connection(1000);
  unsigned before;
  ack(p,2000,32000); ack(p,3000,32000);
  before=nsend; reject_output=1; three_acks(p,3000);
  assert(p->flags&TF_INFR); assert(nsend==before);
  reject_output=0; advance(60);
  assert(nsend>before && sent[before]==3000);
  ack(p,4000,32000); ack(p,8237,32000);
  assert(!(p->flags&TF_INFR));
  tcp_abort(p);
}
static void heuristic_guards(void)
{
  struct tcp_pcb *p=heuristic_connection(1000);
  unsigned before=nsend;
  three_acks(p,1000); /* no progress after RTO, cwnd=one MSS */
  assert(!(p->flags&TF_INFR) && nsend==before);
  ack(p,6000,32000); /* five MSS: likely unnecessary retransmissions */
  before=nsend; three_acks(p,6000);
  assert(!(p->flags&TF_INFR) && nsend==before);
  tcp_abort(p);
  p=heuristic_connection(1000);
  ack(p,5000,32000); /* exactly four MSS is allowed by RFC 6582 4.1 */
  before=nsend; three_acks(p,5000);
  assert((p->flags&TF_INFR) && nsend>before && sent[before]==5000);
  tcp_abort(p);
}
static void heuristic_timeout(void)
{
  struct tcp_pcb *p=heuristic_connection(1000);
  unsigned before;
  ack(p,2000,32000); ack(p,3000,32000); three_acks(p,3000);
  assert(p->flags&TF_INFR);
  before=nsend; advance(2050); /* heuristic retransmission lost too */
  assert(!(p->flags&TF_INFR) && (p->flags&TF_RTO));
  assert(nsend>before && sent[before]==3000 && p->recovery.rto_ms==4000);
  before=nsend; three_acks(p,3000);
  assert(!(p->flags&TF_INFR) && nsend==before);
  tcp_abort(p);
}
/* USB screen is explicitly opted in; other TCP keeps its conservative floor. */
static void usb_screen_rto(void)
{
  struct tcp_pcb *p = connection(2, 1, 1000);
  unsigned before;
  u32_t start;
  nif.name[0]='e'; nif.name[1]='n';
  assert(tcp_recovery_usb_screen(p)==1);
  assert(p->recovery.rto_ms==50); /* USB initial RTO, even before first sample */
  advance(5); ack(p,2000,32000);
  assert(p->recovery.rto_ms==50);
  assert(tcp_write(p,payload,1000,TCP_WRITE_FLAG_COPY)==ERR_OK);
  assert(tcp_output(p)==ERR_OK);
  before=nsend; start=now_ms;
  advance(49); assert(nsend==before);
  advance(11); assert(nsend==before+1 && now_ms-start==60);
  assert(p->recovery.rto_ms==100 && p->cwnd==p->mss);
  /* Repeated poll calls must not erase backoff or restart the deadline. */
  start=p->recovery.timer_started_ms;
  assert(tcp_recovery_usb_screen(p)==0);
  assert(p->recovery.rto_ms==100 && p->recovery.timer_started_ms==start);
  before=nsend;
  advance(89); assert(nsend==before);
  advance(11); assert(nsend==before+1 && p->recovery.rto_ms==200);
  ack(p,3000,32000); assert(p->recovery.rto_ms==200); /* Karn */
  assert(tcp_write(p,payload,1000,TCP_WRITE_FLAG_COPY)==ERR_OK);
  assert(tcp_output(p)==ERR_OK); advance(5); ack(p,4000,32000);
  assert(p->recovery.rto_ms==50);
  tcp_abort(p);
  /* Reused/new USB PCB is not implicitly a screen connection. */
  p=connection(2,1,1000); advance(5); ack(p,2000,32000);
  assert(p->recovery.rto_ms==1000); tcp_abort(p);
}
static void usb_screen_scope(void)
{
  struct tcp_pcb *p;
  nif.name[0]='w'; nif.name[1]='l';
  p=connection(2,1,1000);
  assert(tcp_recovery_usb_screen(p)==0);
  advance(5); ack(p,2000,32000);
  assert(p->recovery.rto_ms==1000); tcp_abort(p);
  nif.name[0]='e'; nif.name[1]='n';
  p=connection(2,1,1000); advance(5); ack(p,2000,32000);
  assert(p->recovery.rto_ms==1000);
  assert(tcp_recovery_usb_screen(p)==1); /* warm socket, no extra sample needed */
  assert(p->recovery.rto_ms==50); tcp_abort(p);
  p=connection(2,0,1000); p->state=SYN_SENT;
  assert(tcp_recovery_usb_screen(p)==0); tcp_abort(p);
  p=connection(2,0,1000); p->netif_idx=254;
  assert(tcp_recovery_usb_screen(p)==0); tcp_abort(p);
}
static void usb_screen_initial(void)
{
  struct tcp_pcb *p=connection(2,1,1000);
  unsigned before=nsend;
  nif.name[0]='e'; nif.name[1]='n';
  assert(tcp_recovery_usb_screen(p)==1);
  assert(p->recovery.srtt_x8==0 && p->recovery.rto_ms==50);
  advance(49); assert(nsend==before);
  advance(11); assert(nsend==before+1 && sent[before]==1000);
  assert(p->recovery.rto_ms==100);
  tcp_abort(p);
  /* Enabling after a timeout cannot erase an existing 6-second backoff. */
  p=connection(2,1,1000); advance(3010);
  assert(p->recovery.rto_ms==6000 && (p->flags&TF_RTO));
  assert(tcp_recovery_usb_screen(p)==1);
  assert(p->recovery.rto_ms==6000);
  tcp_abort(p);
  p=connection(2,0,1000); assert(tcp_recovery_usb_screen(p)==1);
  before=nsend; advance(300); assert(nsend==before); tcp_abort(p);
}
static void usb_screen_jitter(void)
{
  struct tcp_pcb *p=connection(2,1,1000);
  nif.name[0]='e'; nif.name[1]='n';
  advance(120); ack(p,2000,32000); /* sample before opt-in */
  assert(tcp_recovery_usb_screen(p)==1);
  assert(p->recovery.rto_ms==360); /* SRTT + 4*RTTVAR exceeds 50ms */
  tcp_abort(p);
}

#if CONFIG_TCP_RECOVERY_TRACE
static void heuristic_evidence(void)
{
  struct tcp_pcb *p=heuristic_connection(1000);
  struct tcp_recovery_event e;
  struct tcp_recovery_stats stats;
  u32_t lost;
  unsigned enter=0, progress=0;
  ack(p,2000,32000); ack(p,3000,32000);
  while(tcp_recovery_trace_pop(1,&e,&lost)) {}
  tcp_recovery_snapshot(&stats);
  three_acks(p,3000);
  advance(7); ack(p,4000,32000);
  ack(p,5000,32000); /* only first forward ACK is correlated */
  while(tcp_recovery_trace_pop(1,&e,&lost)) {
    if(e.kind==TCP_TRACE_HEURISTIC) {
      ++enter;
      assert(e.lastack==3000 && e.ack_advance==1000 && e.recover_end==8237);
      assert((e.flags&TF_RTO) && !(e.flags&TF_INFR));
      assert(e.rto_end==8237 && e.heuristic_reject==0);
      assert(e.heuristic_age_ms==0);
    } else if(e.kind==TCP_TRACE_HEURISTIC_ACK) {
      ++progress;
      assert(e.lastack==4000 && e.heuristic_age_ms==7);
    }
  }
  assert(enter==1 && progress==1 && !lost);
  tcp_recovery_snapshot(&stats);
  assert(stats.heuristic_enter==1 && stats.heuristic_from_rto==1);
  assert(stats.heuristic_progress==1 && stats.heuristic_progress_max_ms==7);
  assert(!stats.heuristic_timeout);
  tcp_abort(p);
  /* Reused PCB must not inherit ACK eligibility or a pending correlation. */
  p=connection(1,1,1000);
  assert(!p->recovery.last_ack_advance && !p->recovery.heuristic_wait_ack);
  tcp_abort(p);
  heuristic_timeout();
  tcp_recovery_snapshot(&stats);
  assert(stats.heuristic_enter==1 && stats.heuristic_timeout==1);
  assert(!stats.heuristic_progress);
}
#endif

#endif

int main(int argc, char **argv)
{
  ip4_addr_t ip, mask, gw;
  if (argc == 2 && (!strcmp(argv[1],"wrap") || !strcmp(argv[1],"trace_wrap") || !strcmp(argv[1],"cause_wrap") || !strcmp(argv[1],"heuristic_wrap") || !strcmp(argv[1],"usb_screen_wrap"))) now_ms = 0xfffffff0U;
  lwip_init(); IP4_ADDR(&ip,192,168,1,1); IP4_ADDR(&mask,255,255,255,0); IP4_ADDR(&gw,0,0,0,0);
  assert(netif_add(&nif,&ip,&mask,&gw,NULL,netinit,ip4_input));
  netif_set_default(&nif); netif_set_up(&nif); netif_set_link_up(&nif);
  assert(argc == 2);
#if CONFIG_SCREEN_WAKE_TRACE
  if (!strcmp(argv[1],"wake_ack_path")) { wake_ack_path(); assert(!tcp_active_pcbs); puts("PASS wake_ack_path"); return 0; }
#endif
#if CONFIG_TCP_RECOVERY_TRACE
  if (!strcmp(argv[1],"trace_evidence") || !strcmp(argv[1],"trace_wrap")) { trace_evidence(); assert(!tcp_active_pcbs); printf("PASS %s\n", argv[1]); return 0; }
  if (!strcmp(argv[1],"trace_cause") || !strcmp(argv[1],"cause_wrap")) { trace_cause(); assert(!tcp_active_pcbs); puts("PASS trace_cause"); return 0; }
  if (!strcmp(argv[1],"trace_fast")) { trace_fast(); assert(!tcp_active_pcbs); puts("PASS trace_fast"); return 0; }
#endif
#if LWIP_TCP_RECOVERY
#if CONFIG_TCP_RECOVERY_TRACE
  if (!strcmp(argv[1],"heuristic_evidence")) heuristic_evidence();
  else
#endif
  if (!strcmp(argv[1],"usb_screen_rto")) usb_screen_rto();
  else if (!strcmp(argv[1],"usb_screen_wrap")) usb_screen_rto();
  else if (!strcmp(argv[1],"usb_screen_initial")) usb_screen_initial();
  else if (!strcmp(argv[1],"usb_screen_scope")) usb_screen_scope();
  else if (!strcmp(argv[1],"usb_screen_jitter")) usb_screen_jitter();
  else if (!strcmp(argv[1],"heuristic_rto")) heuristic_rto(1000);
  else if (!strcmp(argv[1],"heuristic_wrap")) heuristic_rto(0xfffff000U);
  else if (!strcmp(argv[1],"heuristic_tail")) heuristic_tail();
  else if (!strcmp(argv[1],"heuristic_busy")) heuristic_busy();
  else if (!strcmp(argv[1],"heuristic_output")) heuristic_output();
  else if (!strcmp(argv[1],"heuristic_guards")) heuristic_guards();
  else if (!strcmp(argv[1],"heuristic_timeout")) heuristic_timeout();
  else
#endif
  if (!strcmp(argv[1],"limited")) limited();
  else if (!strcmp(argv[1],"partial")) partial();
  else if (!strcmp(argv[1],"rto")) rto();
  else if (!strcmp(argv[1],"busy")) busy();
  else if (!strcmp(argv[1],"many_partial")) many_partial();
  else if (!strcmp(argv[1],"queued_busy")) queued_busy();
  else if (!strcmp(argv[1],"window_reorder")) window_reorder();
  else if (!strcmp(argv[1],"partial_busy")) partial_busy();
  else if (!strcmp(argv[1],"wrap")) wrap();
  else if (!strcmp(argv[1],"handshake_idle")) handshake_idle();
  else if (!strcmp(argv[1],"boundary")) boundary();
  else if (!strcmp(argv[1],"moving_clock")) moving_clock();
  else if (!strcmp(argv[1],"output_failure")) output_failure();
  else if (!strcmp(argv[1],"karn")) karn();
  else if (!strcmp(argv[1],"timer_cadence")) timer_cadence();
  else if (!strcmp(argv[1],"zero_window")) zero_window();
  else if (!strcmp(argv[1],"partial_dupack")) partial_dupack();
  else return 2;
  assert(!tcp_active_pcbs); printf("PASS %s\n", argv[1]); return 0;
}
