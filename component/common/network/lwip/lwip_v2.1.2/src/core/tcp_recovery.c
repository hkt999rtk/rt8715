/* RFC 3042 Limited Transmit, RFC 6582 NewReno and RFC 6298 RTO timing.
 * All operations run in the TCP core context. No allocations or UART output
 * on the packet path. Legacy pcb offsets and maintenance timer units remain.
 */
#include "lwip/opt.h"
#if LWIP_TCP && LWIP_TCP_RECOVERY
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/tcp_recovery.h"
#include "lwip/sys.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include <string.h>

static struct tcp_recovery_stats recovery_stats;

/* RFC 6582 section 4.1. Only advancing ACKs update last_ack_advance;
 * resetting it at RTO prevents reusing pre-timeout evidence. Duplicate ACK
 * classification and the threshold of three remain in tcp_receive(). */
static u8_t ack_heuristic_reject(const struct tcp_pcb *pcb)
{
  u8_t reason = 0;
  if (!pcb->recovery.last_ack_advance) reason |= TCP_HEURISTIC_NO_PROGRESS;
  if (pcb->cwnd <= pcb->mss) reason |= TCP_HEURISTIC_SMALL_CWND;
  if (pcb->recovery.last_ack_advance > 4U * pcb->mss) reason |= TCP_HEURISTIC_LARGE_ACK;
  if (pcb->state != ESTABLISHED) reason |= TCP_HEURISTIC_NOT_ESTABLISHED;
  if (!pcb->snd_wnd) reason |= TCP_HEURISTIC_ZERO_WINDOW;
  return reason;
}

#if CONFIG_TCP_RECOVERY_TRACE
struct trace_ring {
  struct tcp_recovery_event entries[TCP_RECOVERY_TRACE_SLOTS];
  u32_t overwritten;
  u8_t head, count;
};
static struct trace_ring trace_rings[2];
static u32_t trace_id, trace_connection;

/* TCP-core-only live history: no PCB pointer survives close/reuse.
 * Bounded lookup, no allocation, no packet list traversal. Latest FULL anchors
 * a history; packet counters below cover only the interval AFTER that FULL. */
#define TRACE_HISTORY_SLOTS 8U
static struct tcp_recovery_cause cause_history[TRACE_HISTORY_SLOTS];
static struct tcp_recovery_cause cause_frozen[TCP_RECOVERY_CAUSE_SLOTS];
static struct tcp_recovery_event cause_due;
static u32_t cause_dropped, history_evicted;
static u8_t history_next, cause_head, cause_count;

static struct tcp_recovery_cause *cause_find(u32_t connection)
{
  unsigned i;
  for (i=0; i<TRACE_HISTORY_SLOTS; ++i)
    if (cause_history[i].full.connection == connection) return &cause_history[i];
  return NULL;
}

static void cause_event(const struct tcp_recovery_event *e)
{
  struct tcp_recovery_cause *h = cause_find(e->connection);
  SYS_ARCH_DECL_PROTECT(level);
  if (e->kind == TCP_TRACE_FULL) {
    if (!h) {
      h = &cause_history[history_next];
      history_next = (history_next + 1U) % TRACE_HISTORY_SLOTS;
      if (h->full.id) {
        SYS_ARCH_PROTECT(level); ++history_evicted; SYS_ARCH_UNPROTECT(level);
      }
    }
    memset(h, 0, sizeof(*h)); h->full = *e;
  } else if (e->kind == TCP_TRACE_FAST_SKIP && h) {
    h->skip = *e; ++h->skips; h->skip_mask |= e->skip_reason;
  } else if (e->kind == TCP_TRACE_RTO_DUE) {
    cause_due = *e;
  } else if (e->kind == TCP_TRACE_RTO_ARM && cause_due.connection == e->connection) {
    /* Freeze only after prepare succeeds. Busy retry attempts cannot fill the
     * queue. Reporter copies under this same short critical section. */
    SYS_ARCH_PROTECT(level);
    if (cause_count == TCP_RECOVERY_CAUSE_SLOTS) ++cause_dropped;
    else {
      struct tcp_recovery_cause *f = &cause_frozen[(cause_head+cause_count)%TCP_RECOVERY_CAUSE_SLOTS];
      if (h) *f = *h;
      else memset(f, 0, sizeof(*f));
      f->due = cause_due; ++cause_count;
    }
    SYS_ARCH_UNPROTECT(level);
  }
}

int tcp_recovery_cause_pop(struct tcp_recovery_cause *out, u32_t *dropped, u32_t *evicted)
{
  int found;
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  *dropped = cause_dropped; *evicted = history_evicted;
  found = cause_count != 0;
  if (found) {
    *out = cause_frozen[cause_head];
    cause_head = (cause_head+1U)%TCP_RECOVERY_CAUSE_SLOTS; --cause_count;
  }
  SYS_ARCH_UNPROTECT(level);
  return found;
}


static void trace_init(struct tcp_pcb *pcb)
{
  if (!pcb->recovery.trace.connection) {
    if (++trace_connection == 0) ++trace_connection;
    pcb->recovery.trace.connection = trace_connection;
  }
}

static void trace_record(struct tcp_pcb *pcb, u8_t kind, u8_t urgent)
{
  struct tcp_recovery_event e;
  struct tcp_recovery_state *r = &pcb->recovery;
  struct trace_ring *q = &trace_rings[urgent != 0];
  SYS_ARCH_DECL_PROTECT(level);
  trace_init(pcb);
  memset(&e, 0, sizeof(e));
  e.id = ++trace_id; e.connection = r->trace.connection; e.ms = sys_now();
  e.kind = kind; e.state = pcb->state; e.flags = pcb->flags;
  e.local_port = pcb->local_port; e.remote_port = pcb->remote_port;
  e.lastack = pcb->lastack; e.snd_nxt = pcb->snd_nxt; e.recover_end = r->recover_end;
  e.ack_advance = r->last_ack_advance; e.rto_end = pcb->rto_end;
  e.heuristic_age_ms = r->heuristic_wait_ack ? e.ms-r->heuristic_started_ms : 0xffffffffU;
  e.heuristic_reject = ack_heuristic_reject(pcb);
  e.cwnd = pcb->cwnd; e.ssthresh = pcb->ssthresh; e.wnd = pcb->snd_wnd;
  e.sndbuf = pcb->snd_buf; e.qlen = pcb->snd_queuelen;
  e.dupacks = pcb->dupacks; e.nrtx = pcb->nrtx;
  e.rto_ms = r->rto_ms; e.srtt_ms = r->srtt_x8 / 8U;
  e.timer_age_ms = pcb->rtime < 0 ? 0xffffffffU : e.ms - r->timer_started_ms;
  e.ack_age_ms = r->trace.ack_seen ? e.ms - r->trace.ack_ms : 0xffffffffU;
  e.progress_age_ms = r->trace.progress_seen ? e.ms - r->trace.progress_ms : 0xffffffffU;
  e.ack_no = r->trace.ack_no; e.ack_wnd = r->trace.ack_wnd;
  e.ack_gap_ms = r->trace.ack_gap_ms;
  e.tx_seq = r->trace.tx_seq; e.tx_end = r->trace.tx_end;
  e.tx_age_ms = r->trace.tx_seen ? e.ms - r->trace.tx_ms : 0xffffffffU;
  if (kind == TCP_TRACE_FAST_SKIP) {
    if (!pcb->unacked) e.skip_reason |= TCP_SKIP_NO_UNACKED;
    if (pcb->flags & TF_INFR) e.skip_reason |= TCP_SKIP_IN_RECOVERY;
    if (pcb->flags & TF_RTO) e.skip_reason |= TCP_SKIP_IN_RTO;
    if (r->recover_valid && TCP_SEQ_LEQ(pcb->lastack, r->recover_end))
      e.skip_reason |= TCP_SKIP_BOUNDARY;
  }
  e.partial_count = r->trace.partial_count; e.recover_valid = r->recover_valid;
  e.pending = r->fast_pending | (r->partial_pending << 1) | (r->output_pending << 2);
  if (pcb->unacked) {
    e.heads |= 1; e.unacked_seq = lwip_ntohl(pcb->unacked->tcphdr->seqno);
    e.unacked_len = TCP_TCPLEN(pcb->unacked);
    e.unacked_ref = pcb->unacked->p->ref;
  }
  if (pcb->unsent) {
    e.heads |= 2; e.unsent_seq = lwip_ntohl(pcb->unsent->tcphdr->seqno);
    e.unsent_len = TCP_TCPLEN(pcb->unsent);
    e.unsent_ref = pcb->unsent->p->ref;
  }
  cause_event(&e);
  /* Only the TCP core produces records; reporter protects its bounded copy.
   * All formatting is deferred. Overwrite oldest within this class only. */
  SYS_ARCH_PROTECT(level);
  if (q->count == TCP_RECOVERY_TRACE_SLOTS) {
    q->head = (q->head + 1U) % TCP_RECOVERY_TRACE_SLOTS;
    --q->count; ++q->overwritten;
  }
  q->entries[(q->head + q->count) % TCP_RECOVERY_TRACE_SLOTS] = e;
  ++q->count;
  SYS_ARCH_UNPROTECT(level);
}

int tcp_recovery_trace_pop(u8_t urgent, struct tcp_recovery_event *out, u32_t *overwritten)
{
  struct trace_ring *q = &trace_rings[urgent != 0];
  int found;
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  *overwritten = q->overwritten;
  found = q->count != 0;
  if (found) {
    *out = q->entries[q->head];
    q->head = (q->head + 1U) % TCP_RECOVERY_TRACE_SLOTS;
    --q->count;
  }
  SYS_ARCH_UNPROTECT(level);
  return found;
}

void tcp_recovery_trace_ack(struct tcp_pcb *pcb, u32_t ackno, u32_t advertised_wnd)
{
  struct tcp_recovery_state *r = &pcb->recovery;
  u32_t now = sys_now();
  trace_init(pcb);
  {
    struct tcp_recovery_cause *h = cause_find(r->trace.connection);
    if (h) ++h->ack_packets;
  }
  r->trace.ack_gap_ms = r->trace.ack_seen ? now - r->trace.ack_ms : 0;
  r->trace.ack_seen = 1; r->trace.ack_ms = now;
  r->trace.ack_no = ackno; r->trace.ack_wnd = advertised_wnd;
  /* Ignore idle gaps: only snapshot a gap while data remains outstanding. */
  if (r->trace.ack_gap_ms >= 100U && pcb->lastack != pcb->snd_nxt)
    trace_record(pcb, TCP_TRACE_ACK_GAP, 0);
}
#define TRACE(pcb, kind, urgent) trace_record(pcb, kind, urgent)
#else
#define TRACE(pcb, kind, urgent) ((void)0)
#endif

static void
note(const struct tcp_pcb *pcb, u8_t event)
{
  recovery_stats.last_event = event;
  recovery_stats.last_event_ms = sys_now();
  recovery_stats.local_port = pcb->local_port;
  recovery_stats.remote_port = pcb->remote_port;
  recovery_stats.last_seq = pcb->snd_nxt;
  recovery_stats.last_ack = pcb->lastack;
  recovery_stats.last_cwnd = pcb->cwnd;
  recovery_stats.last_flight = pcb->snd_nxt - pcb->lastack;
  recovery_stats.last_rto_ms = pcb->recovery.rto_ms;
}

void
tcp_recovery_snapshot(struct tcp_recovery_stats *result)
{
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  *result = recovery_stats;
  memset(&recovery_stats, 0, sizeof(recovery_stats));
  SYS_ARCH_UNPROTECT(level);
}

void tcp_recovery_busy(struct tcp_pcb *pcb) { ++recovery_stats.busy; note(pcb, 5); TRACE(pcb, TCP_TRACE_BUSY, 0); }
void tcp_recovery_output_error(struct tcp_pcb *pcb)
{
  ++recovery_stats.output_error;
  pcb->recovery.output_pending = 1;
  note(pcb, 7);
  TRACE(pcb, TCP_TRACE_OUTPUT_ERROR, 0);
}

static u32_t
recovery_rto_estimate(const struct tcp_recovery_state *r)
{
  u32_t floor = r->usb_screen_rto ? TCP_RECOVERY_USB_SCREEN_RTO_MIN : TCP_RECOVERY_RTO_MIN;
  return LWIP_MIN(TCP_RECOVERY_RTO_MAX, LWIP_MAX(floor,
      r->srtt_x8 / 8U + LWIP_MAX(TCP_RECOVERY_INTERVAL, r->rttvar_x4)));
}

int
tcp_recovery_usb_screen(struct tcp_pcb *pcb)
{
  struct netif *out;
  struct tcp_recovery_state *r;
  LWIP_ASSERT_CORE_LOCKED();
  if (pcb == NULL || pcb->state != ESTABLISHED) return 0;
  r = &pcb->recovery;
  if (r->usb_screen_rto) return 0;
  /* Match tcp_output's bound-interface/route choice. en is the USB NCM netif
   * in this firmware. Caller identifies screen TX; ports are not guessed.
   * This opt-in is retained for this PCB lifetime, not for a recycled fd. */
  out = pcb->netif_idx != NETIF_NO_INDEX ? netif_get_by_index(pcb->netif_idx) :
        ip_route(&pcb->local_ip, &pcb->remote_ip);
  if (out == NULL || out->name[0] != 'e' || out->name[1] != 'n') return 0;
  r->usb_screen_rto = 1;
  /* An already-running RTO/Karn recovery must keep its backed-off deadline.
   * A later clean RTT sample will apply the USB floor. Do not restart the
   * clock: enabling this during EAGAIN must not extend a stalled flight. */
  if (!pcb->nrtx && !(pcb->flags & TF_RTO) && !r->karn_blocked) {
    r->rto_ms = r->srtt_x8 ? recovery_rto_estimate(r) : TCP_RECOVERY_USB_SCREEN_RTO_MIN;
    pcb->rto = (s16_t)((r->rto_ms + TCP_SLOW_INTERVAL - 1) / TCP_SLOW_INTERVAL);
  }
  return 1;
}

int
tcp_recovery_fine_timer(const struct tcp_pcb *pcb)
{
  return pcb->state >= ESTABLISHED && pcb->state < TIME_WAIT;
}

void
tcp_recovery_dupack(struct tcp_pcb *pcb)
{
  if (pcb->state == ESTABLISHED && !(pcb->flags & (TF_INFR | TF_RTO)) &&
      pcb->dupacks <= 2) {
    /* One opportunity per duplicate ACK; cwnd itself is never inflated. */
    if (pcb->recovery.lt_credit < 2) {
      ++pcb->recovery.lt_credit;
    }
    ++recovery_stats.lt_opportunities;
  } else {
    pcb->recovery.lt_credit = 0;
  }
}

u32_t
tcp_recovery_output_window(const struct tcp_pcb *pcb)
{
  u32_t wnd = LWIP_MIN(pcb->snd_wnd, pcb->cwnd);
  const struct tcp_seg *seg = pcb->unsent;
  if (pcb->recovery.lt_credit && pcb->dupacks <= 2 &&
      !(pcb->flags & (TF_INFR | TF_RTO)) && seg && seg->len &&
      seg->len <= pcb->mss && TCP_SEQ_GEQ(lwip_ntohl(seg->tcphdr->seqno), pcb->snd_nxt)) {
    u32_t end = lwip_ntohl(seg->tcphdr->seqno) - pcb->lastack + TCP_TCPLEN(seg);
    u32_t limit = pcb->cwnd + 2U * pcb->mss;
    if (end <= pcb->snd_wnd && end <= limit) {
      /* Extend only to this segment; next iteration must recheck credits. */
      wnd = LWIP_MAX(wnd, end);
    }
  }
  return wnd;
}

void
tcp_recovery_sent(struct tcp_pcb *pcb, const struct tcp_seg *seg)
{
  struct tcp_recovery_state *r = &pcb->recovery;
  u32_t seq = lwip_ntohl(seg->tcphdr->seqno);
#if CONFIG_TCP_RECOVERY_TRACE
  trace_init(pcb);
  r->trace.tx_ms = sys_now(); r->trace.tx_seq = seq;
  r->trace.tx_end = seq + TCP_TCPLEN(seg); r->trace.tx_seen = 1;
  if (TCP_SEQ_LT(seq, pcb->snd_nxt) && TCP_TCPLEN(seg)) {
    TRACE(pcb, r->trace.rto_wait_tx ? TCP_TRACE_RTO_TX : TCP_TRACE_RETX,
          r->trace.rto_wait_tx);
    r->trace.rto_wait_tx = 0;
  }
#endif
  /* Called only after successful output, before snd_nxt is advanced. */
  if (TCP_SEQ_GEQ(seq, pcb->snd_nxt) && TCP_TCPLEN(seg)) {
    if (r->lt_credit && pcb->dupacks <= 2 && !(pcb->flags & (TF_INFR | TF_RTO))) {
      --r->lt_credit;
      r->lt_bytes += seg->len;
      ++recovery_stats.lt_sent;
      note(pcb, 1);
    }
    if (tcp_recovery_fine_timer(pcb) && !r->sample_valid && !r->karn_blocked &&
        !(pcb->flags & (TF_INFR | TF_RTO))) {
      r->sample_started_ms = sys_now();
      r->sample_end = seq + TCP_TCPLEN(seg);
      r->sample_valid = 1; /* separate validity permits sys_now()==0 */
    }
  }
}

void
tcp_recovery_retransmitted(struct tcp_pcb *pcb)
{
  /* Karn: a cumulative ACK covering retransmitted and fresh data is ambiguous.
   * Do not restart sampling until the entire old flight has been acknowledged. */
  pcb->recovery.sample_valid = 0;
  pcb->recovery.karn_blocked = 1;
  pcb->recovery.karn_end = pcb->snd_nxt;
}

void
tcp_recovery_fast(struct tcp_pcb *pcb)
{
  struct tcp_recovery_state *r = &pcb->recovery;
  u32_t flight;
  const u8_t guarded = (pcb->flags & TF_RTO) ||
      (r->recover_valid && TCP_SEQ_LEQ(pcb->lastack, r->recover_end));
  if (!pcb->unacked || (pcb->flags & TF_INFR) ||
      (guarded && ack_heuristic_reject(pcb))) {
    if (pcb->dupacks == 3) {
      if (guarded && pcb->unacked && !(pcb->flags & TF_INFR))
        ++recovery_stats.heuristic_reject;
      TRACE(pcb, TCP_TRACE_FAST_SKIP, 0);
    }
    r->fast_pending = 0;
    return;
  }
  /* RFC 3042: exclude bytes sent using Limited Transmit from FlightSize. */
  flight = pcb->snd_nxt - pcb->lastack;
  flight = flight > r->lt_bytes ? flight - r->lt_bytes : 0;
  if (tcp_rexmit(pcb) != ERR_OK) {
    r->fast_pending = 1;
    tcp_recovery_busy(pcb);
    return;
  }
  /* Requeue succeeded: now commit the RTO -> fast recovery transition.
   * A busy driver pbuf above leaves cwnd, flags and timer untouched.
   * This event snapshots the OLD boundary/cwnd/TF_RTO after requeue, before
   * the state change; actual output and ACK progress have separate events. */
  if (guarded) {
    ++recovery_stats.heuristic_enter;
    if (pcb->flags & TF_RTO) ++recovery_stats.heuristic_from_rto;
    r->heuristic_started_ms = sys_now(); r->heuristic_wait_ack = 1;
    TRACE(pcb, TCP_TRACE_HEURISTIC, 1);
  }
  tcp_clear_flags(pcb, TF_RTO);
  r->fast_pending = 0;
  r->recover_end = pcb->snd_nxt; /* exclusive high sequence at entry */
  r->recover_valid = 1;
  r->partial_seen = r->partial_pending = r->lt_credit = 0;
  r->recovery_started_ms = r->timer_started_ms = sys_now();
  pcb->ssthresh = (tcpwnd_size_t)LWIP_MAX(flight / 2U, 2U * pcb->mss);
  pcb->cwnd = pcb->ssthresh;
  TCP_WND_INC(pcb->cwnd, 3U * pcb->mss);
  pcb->bytes_acked = 0;
  tcp_set_flags(pcb, TF_INFR);
  pcb->rtime = 0;
  ++recovery_stats.fast;
  note(pcb, 2);
#if CONFIG_TCP_RECOVERY_TRACE
  r->trace.partial_count = 0;
#endif
  TRACE(pcb, TCP_TRACE_FAST, 0);
}

static void
partial_retransmit(struct tcp_pcb *pcb)
{
  /* A previous output failure may already have queued the lowest hole.
   * Do not requeue a later segment ahead of it. tcp_output will retry it. */
  if (pcb->unsent && TCP_SEQ_LT(lwip_ntohl(pcb->unsent->tcphdr->seqno), pcb->snd_nxt) &&
      (!pcb->unacked || TCP_SEQ_LEQ(lwip_ntohl(pcb->unsent->tcphdr->seqno),
                                    lwip_ntohl(pcb->unacked->tcphdr->seqno)))) {
    pcb->recovery.partial_pending = 0;
  } else if (pcb->unacked && tcp_rexmit(pcb) == ERR_OK) {
    pcb->recovery.partial_pending = 0;
  } else {
    pcb->recovery.partial_pending = 1;
    tcp_recovery_busy(pcb);
  }
}

void
tcp_recovery_ack(struct tcp_pcb *pcb, tcpwnd_size_t acked, u8_t was_in_recovery)
{
  struct tcp_recovery_state *r = &pcb->recovery;
  u32_t now = sys_now();
  r->last_ack_advance = acked;
  if (r->heuristic_wait_ack) {
    ++recovery_stats.heuristic_progress;
    recovery_stats.heuristic_progress_max_ms = LWIP_MAX(
        recovery_stats.heuristic_progress_max_ms, now-r->heuristic_started_ms);
    TRACE(pcb, TCP_TRACE_HEURISTIC_ACK, 1);
    r->heuristic_wait_ack = 0;
  }
#if CONFIG_TCP_RECOVERY_TRACE
  {
    struct tcp_recovery_cause *h = cause_find(r->trace.connection);
    if (h) {
      ++h->progress_calls; h->progress_bytes += acked;
      h->progress_ms = now; h->progress_ack = pcb->lastack;
    }
  }
  r->trace.progress_seen = 1; r->trace.progress_ms = now;
  if (r->trace.rto_wait_ack) {
    TRACE(pcb, TCP_TRACE_RTO_ACK, 1);
    r->trace.rto_wait_ack = r->trace.rto_wait_tx = 0;
  }
#endif
  r->lt_credit = r->fast_pending = 0;
  r->lt_bytes = 0;
  /* ACK progress refreshes the abort budget, including partial ACKs. The RTO
   * backoff is independent and remains until a non-ambiguous RTT sample. */
  pcb->nrtx = 0;
  if (was_in_recovery && TCP_SEQ_LT(pcb->lastack, r->recover_end)) {
    /* RFC 6582 partial ACK: retain recovery; deflate by ACKed bytes, add one
     * MSS only when at least one MSS was ACKed. Retransmit next hole now. */
    pcb->cwnd = pcb->cwnd > acked ? pcb->cwnd - acked : 0;
    if (acked >= pcb->mss) TCP_WND_INC(pcb->cwnd, pcb->mss);
    pcb->cwnd = LWIP_MAX(pcb->cwnd, pcb->mss);
    if (!r->partial_seen) r->timer_started_ms = now;
    r->partial_seen = 1;
    partial_retransmit(pcb);
    ++recovery_stats.partial;
    note(pcb, 3);
#if CONFIG_TCP_RECOVERY_TRACE
    if (++r->trace.partial_count == 1) TRACE(pcb, TCP_TRACE_PARTIAL, 0);
#endif
  } else {
    if (was_in_recovery) {
      u32_t duration = now - r->recovery_started_ms;
      u32_t flight = pcb->snd_nxt - pcb->lastack;
      /* RFC 6582 step 5: deflate and avoid a burst after recovery. */
      pcb->cwnd = (tcpwnd_size_t)LWIP_MIN(pcb->ssthresh,
                                          LWIP_MAX(flight, pcb->mss) + pcb->mss);
      pcb->bytes_acked = 0;
      tcp_clear_flags(pcb, TF_INFR);
      r->partial_pending = 0;
      ++recovery_stats.full;
      recovery_stats.recovery_max_ms = LWIP_MAX(recovery_stats.recovery_max_ms, duration);
      note(pcb, 4);
      TRACE(pcb, TCP_TRACE_FULL, 0);
    }
    if (r->recover_valid && TCP_SEQ_GT(pcb->lastack, r->recover_end)) r->recover_valid = 0;
    r->timer_started_ms = now;
  }
  /* If retransmissions are queued on unsent, the timer must remain running. */
  if (pcb->unacked || (pcb->unsent &&
      TCP_SEQ_LT(lwip_ntohl(pcb->unsent->tcphdr->seqno), pcb->snd_nxt))) pcb->rtime = 0;
}

u32_t
tcp_recovery_rtt_ack(struct tcp_pcb *pcb, u32_t ackno)
{
  struct tcp_recovery_state *r = &pcb->recovery;
  u32_t sample = 0;
  if (TCP_SEQ_GT(ackno, pcb->snd_nxt)) return 0;
  if (r->sample_valid && TCP_SEQ_GEQ(ackno, r->sample_end)) {
    sample = LWIP_MAX(1U, (u32_t)(sys_now() - r->sample_started_ms));
    /* Bound arithmetic even if a peer/clock stalls for a very long time. */
    sample = LWIP_MIN(sample, TCP_RECOVERY_RTO_MAX);
    r->sample_valid = 0;
    if (!r->srtt_x8) {
      r->srtt_x8 = sample * 8U;
      r->rttvar_x4 = sample * 2U;
    } else {
      s32_t delta = (s32_t)sample - (s32_t)(r->srtt_x8 / 8U);
      u32_t absdelta = delta < 0 ? (u32_t)-delta : (u32_t)delta;
      r->rttvar_x4 = r->rttvar_x4 - r->rttvar_x4 / 4U + absdelta;
      r->srtt_x8 = (u32_t)((s32_t)r->srtt_x8 + delta);
    }
    r->rto_ms = recovery_rto_estimate(r);
    ++recovery_stats.rtt_samples;
    recovery_stats.rtt_sum_ms += sample;
    recovery_stats.rtt_max_ms = LWIP_MAX(recovery_stats.rtt_max_ms, sample);
    recovery_stats.rto_current_ms = r->rto_ms;
    recovery_stats.srtt_current_ms = r->srtt_x8 / 8U;
    /* Keep legacy diagnostics and OOSEQ expiry expressed in 500ms ticks. */
    pcb->rto = (s16_t)((r->rto_ms + TCP_SLOW_INTERVAL - 1) / TCP_SLOW_INTERVAL);
  }
  if (r->karn_blocked && TCP_SEQ_GEQ(ackno, r->karn_end)) r->karn_blocked = 0;
  return sample;
}

void
tcp_recovery_tmr(void)
{
  struct tcp_pcb *pcb;
  u32_t now;
  LWIP_ASSERT_CORE_LOCKED();
  for (pcb = tcp_active_pcbs; pcb; pcb = pcb->next) {
    struct tcp_recovery_state *r = &pcb->recovery;
    if (!tcp_recovery_fine_timer(pcb) || pcb->persist_backoff || pcb->nrtx >= TCP_MAXRTX) continue;
    if (r->output_pending) {
      r->output_pending = 0;
      tcp_output(pcb); /* a failed enqueue sets output_pending again */
    }
    if (r->fast_pending && pcb->dupacks >= 3) {
      tcp_recovery_fast(pcb);
      if (!r->fast_pending) tcp_output(pcb);
    }
    if (r->partial_pending && (pcb->flags & TF_INFR)) {
      partial_retransmit(pcb);
      if (!r->partial_pending) tcp_output(pcb);
    }
    /* Deferred output/recovery can start a new timer in this iteration.
     * Read after those mutations, or a tick crossing would underflow elapsed. */
    now = sys_now();
    if (pcb->rtime >= 0 && (u32_t)(now - r->timer_started_ms) >= r->rto_ms) {
      TRACE(pcb, TCP_TRACE_RTO_DUE, 1);
      if (tcp_rexmit_rto_prepare(pcb) != ERR_OK &&
          !(pcb->unacked == NULL && pcb->unsent != NULL)) {
        tcp_recovery_busy(pcb);
        continue;
      }
      recovery_stats.deadline_late_max_ms = LWIP_MAX(recovery_stats.deadline_late_max_ms,
                            (u32_t)(now - r->timer_started_ms) - r->rto_ms);
      ++recovery_stats.rto;
      if (r->heuristic_wait_ack) {
        ++recovery_stats.heuristic_timeout;
        r->heuristic_wait_ack = 0;
      }
      r->last_ack_advance = 0;
      r->recover_end = pcb->snd_nxt;
      r->recover_valid = 1;
      r->fast_pending = r->partial_pending = r->lt_credit = 0;
      r->lt_bytes = 0;
      tcp_clear_flags(pcb, TF_INFR);
      /* RFC 6298: retain exponential backoff until a valid RTT sample, and
       * restart a full deadline. Busy buffers do not consume retry attempts. */
      r->rto_ms = LWIP_MIN(r->rto_ms * 2U, TCP_RECOVERY_RTO_MAX);
      pcb->rto = (s16_t)((r->rto_ms + TCP_SLOW_INTERVAL - 1) / TCP_SLOW_INTERVAL);
      r->timer_started_ms = now;
      pcb->rtime = 0;
      pcb->ssthresh = (tcpwnd_size_t)LWIP_MAX((pcb->snd_nxt - pcb->lastack) / 2U, 2U * pcb->mss);
      pcb->cwnd = pcb->mss;
      pcb->bytes_acked = 0;
      note(pcb, 6);
#if CONFIG_TCP_RECOVERY_TRACE
      r->trace.rto_wait_tx = r->trace.rto_wait_ack = 1;
#endif
      TRACE(pcb, TCP_TRACE_RTO_ARM, 1);
      tcp_rexmit_rto_commit(pcb);
    }
  }
}
#endif
