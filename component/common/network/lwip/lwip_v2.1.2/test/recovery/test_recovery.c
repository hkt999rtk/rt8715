/* Packet injection tests use the production TCP input/output/timer code. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/tcp_recovery.h"
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
int main(int argc, char **argv)
{
  ip4_addr_t ip, mask, gw;
  if (argc == 2 && !strcmp(argv[1],"wrap")) now_ms = 0xfffffff0U;
  lwip_init(); IP4_ADDR(&ip,192,168,1,1); IP4_ADDR(&mask,255,255,255,0); IP4_ADDR(&gw,0,0,0,0);
  assert(netif_add(&nif,&ip,&mask,&gw,NULL,netinit,ip4_input));
  netif_set_default(&nif); netif_set_up(&nif); netif_set_link_up(&nif);
  assert(argc == 2);
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
