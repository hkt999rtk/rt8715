/* RFC 3042 Limited Transmit, RFC 6582 NewReno and RFC 6298 RTO timing.
 * All operations run in the TCP core context. No allocations or UART output
 * on the packet path. Legacy pcb offsets and maintenance timer units remain.
 */
#include "lwip/opt.h"
#if LWIP_TCP && LWIP_TCP_RECOVERY
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/tcp_recovery.h"
#include "lwip/sys.h"
#include <string.h>

static struct tcp_recovery_stats recovery_stats;

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

void tcp_recovery_busy(struct tcp_pcb *pcb) { ++recovery_stats.busy; note(pcb, 5); }
void tcp_recovery_output_error(struct tcp_pcb *pcb)
{
  ++recovery_stats.output_error;
  pcb->recovery.output_pending = 1;
  note(pcb, 7);
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
  if (!pcb->unacked || (pcb->flags & (TF_INFR | TF_RTO)) ||
      (r->recover_valid && TCP_SEQ_LEQ(pcb->lastack, r->recover_end))) {
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
    r->rto_ms = LWIP_MIN(TCP_RECOVERY_RTO_MAX, LWIP_MAX(TCP_RECOVERY_RTO_MIN,
                         r->srtt_x8 / 8U + LWIP_MAX(TCP_RECOVERY_INTERVAL, r->rttvar_x4)));
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
      if (tcp_rexmit_rto_prepare(pcb) != ERR_OK &&
          !(pcb->unacked == NULL && pcb->unsent != NULL)) {
        tcp_recovery_busy(pcb);
        continue;
      }
      recovery_stats.deadline_late_max_ms = LWIP_MAX(recovery_stats.deadline_late_max_ms,
                            (u32_t)(now - r->timer_started_ms) - r->rto_ms);
      ++recovery_stats.rto;
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
      tcp_rexmit_rto_commit(pcb);
    }
  }
}
#endif
