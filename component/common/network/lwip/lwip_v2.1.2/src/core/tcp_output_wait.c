#include "lwip/opt.h"
#if LWIP_TCP && LWIP_TCP_RECOVERY && CONFIG_TCP_RECOVERY_TRACE && CONFIG_MOVE_LAG_PROFILE
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/tcp_output_wait.h"
#include "lwip/sys.h"
#include <string.h>

/* TCP core 写入，统计线程复制后打印。固定内存，不保存 PCB/segment 指针。
 * connection 使用现有 TCPREC ID，避免 socket/PCB 地址复用混入旧连接。
 * 短临界区仅保护观测数据；不遍历发送队列，不读写业务字段。 */
static struct tcp_wait_row wait_rows[TCP_WAIT_ROWS];
static struct tcp_wait_event wait_events[TCP_WAIT_EVENTS];
static u32_t wait_evicted, wait_dropped;
static unsigned wait_head, wait_count, wait_replace;

static struct tcp_wait_row *wait_find(const struct tcp_pcb *pcb, int create)
{
  unsigned i, empty = TCP_WAIT_ROWS;
  u32_t id = pcb->recovery.trace.connection;
  struct tcp_wait_row *r;
  if (!pcb->recovery.usb_screen_rto || !id) return NULL;
  for (i = 0; i < TCP_WAIT_ROWS; ++i) {
    if (wait_rows[i].connection == id) return &wait_rows[i];
    if (!wait_rows[i].connection && empty == TCP_WAIT_ROWS) empty = i;
  }
  if (!create) return NULL;
  if (empty == TCP_WAIT_ROWS) {
    empty = wait_replace;
    wait_replace = (wait_replace + 1U) % TCP_WAIT_ROWS;
    ++wait_evicted; /* 包括被替换的未结束观测；不能当作成功恢复。 */
  }
  r = &wait_rows[empty];
  memset(r, 0, sizeof(*r)); r->connection = id;
  r->current.connection = id;
  r->current.local_port = pcb->local_port; r->current.remote_port = pcb->remote_port;
  return r;
}

static void wait_finish(struct tcp_wait_row *r, u32_t now, u8_t result,
                        u32_t seq, u32_t len)
{
  struct tcp_wait_event *e = &r->current;
  if (!r->active) return;
  e->end_ms = now; e->duration_ms = now - e->start_ms; e->result = result;
  e->resume_seq = seq; e->resume_len = len;
  e->first_ack_ms = e->ack_count ? now - r->first_ack_at : 0xffffffffU;
  e->last_ack_ms = e->ack_count ? now - r->last_ack_at : 0xffffffffU;
  r->max_ms = LWIP_MAX(r->max_ms, e->duration_ms);
  if (result == TCP_WAIT_SENT) ++r->resumed; else ++r->cancelled;
  r->active = 0;
  /* 所有等待进入累计计数；明细只留 >=20ms，避免正常短等待刷屏。
   * 满环丢新记录，dropped 会打印，不能把缺失明细当作没有等待。 */
  if (e->duration_ms >= 20U) {
    if (wait_count == TCP_WAIT_EVENTS) ++wait_dropped;
    else {
      wait_events[(wait_head + wait_count) % TCP_WAIT_EVENTS] = *e;
      ++wait_count;
    }
  }
}

static void wait_block_locked(const struct tcp_pcb *pcb, const struct tcp_seg *seg,
                              u8_t reason, u32_t effective_wnd, u32_t now)
{
  struct tcp_wait_row *r = wait_find(pcb, 1);
  struct tcp_wait_event *e;
  if (!r) return;
  e = &r->current;
  if (!r->active) {
    memset(e, 0, sizeof(*e));
    e->connection = r->connection; e->episode = ++r->started;
    e->start_ms = now; e->rto_id = r->last_rto_id;
    e->seq = lwip_ntohl(seg->tcphdr->seqno); e->length = TCP_TCPLEN(seg);
    e->flight = pcb->snd_nxt - pcb->lastack;
    e->need = e->seq - pcb->lastack + e->length;
    e->cwnd = pcb->cwnd; e->wnd = pcb->snd_wnd; e->effective_wnd = effective_wnd;
    e->flags = pcb->flags; e->local_port = pcb->local_port; e->remote_port = pcb->remote_port;
    r->active = 1;
  }
  e->reason_mask |= reason; ++r->checks;
  r->cwnd_checks += !!(reason & TCP_WAIT_CWND);
  r->rwnd_checks += !!(reason & TCP_WAIT_RWND);
  r->nagle_checks += !!(reason & TCP_WAIT_NAGLE);
}

void tcp_wait_block(const struct tcp_pcb *pcb, const struct tcp_seg *seg, u8_t reason)
{
  u32_t now;
  SYS_ARCH_DECL_PROTECT(level);
  if (!pcb->recovery.usb_screen_rto || !seg) return;
  now = sys_now();
  SYS_ARCH_PROTECT(level);
  wait_block_locked(pcb, seg, reason,
      pcb->cwnd < pcb->snd_wnd ? pcb->cwnd : pcb->snd_wnd, now);
  SYS_ARCH_UNPROTECT(level);
}

void tcp_wait_window(const struct tcp_pcb *pcb, const struct tcp_seg *seg, u32_t effective_wnd)
{
  u32_t need, now;
  u8_t reason = 0;
  SYS_ARCH_DECL_PROTECT(level);
  if (!pcb->recovery.usb_screen_rto || !seg) return;
  /* 与 tcp_output 当前窗口判定使用相同的 seq/len，不忽略 Limited Transmit。
   * 只记录已经被实际有效窗口挡住的 segment。 */
  need = lwip_ntohl(seg->tcphdr->seqno) - pcb->lastack + seg->len;
  if (need <= effective_wnd) return;
  if (need > pcb->cwnd) reason |= TCP_WAIT_CWND;
  if (need > pcb->snd_wnd) reason |= TCP_WAIT_RWND;
  now = sys_now();
  SYS_ARCH_PROTECT(level);
  wait_block_locked(pcb, seg, reason, effective_wnd, now);
  SYS_ARCH_UNPROTECT(level);
}

void tcp_wait_sent(const struct tcp_pcb *pcb, const struct tcp_seg *seg)
{
  struct tcp_wait_row *r;
  u32_t seq, len, now;
  SYS_ARCH_DECL_PROTECT(level);
  if (!pcb->recovery.usb_screen_rto) return;
  seq = lwip_ntohl(seg->tcphdr->seqno); len = TCP_TCPLEN(seg); now = sys_now();
  SYS_ARCH_PROTECT(level);
  r = wait_find(pcb, 0);
  if (r && r->active && len) {
    /* 只有成功提交覆盖原等待 seq 的数据才算恢复。重传更早的 segment
     * 不算恢复；允许窗口拆包后仅提交原等待 segment 的第一部分。 */
    if (TCP_SEQ_LEQ(seq, r->current.seq) && TCP_SEQ_GT(seq + len, r->current.seq))
      wait_finish(r, now, TCP_WAIT_SENT, seq, len);
    else if (TCP_SEQ_GT(seq, r->current.seq))
      wait_finish(r, now, TCP_WAIT_SKIPPED, seq, len);
  }
  SYS_ARCH_UNPROTECT(level);
}

void tcp_wait_ack(const struct tcp_pcb *pcb)
{
  struct tcp_wait_row *r;
  u32_t now;
  SYS_ARCH_DECL_PROTECT(level);
  if (!pcb->recovery.usb_screen_rto) return;
  now = sys_now();
  SYS_ARCH_PROTECT(level);
  r = wait_find(pcb, 0);
  if (r && r->active) {
    if (!r->current.ack_count++) r->first_ack_at = now;
    r->last_ack_at = now;
  }
  SYS_ARCH_UNPROTECT(level);
}

void tcp_wait_rto(const struct tcp_pcb *pcb, u32_t id)
{
  struct tcp_wait_row *r;
  SYS_ARCH_DECL_PROTECT(level);
  if (!pcb->recovery.usb_screen_rto) return;
  SYS_ARCH_PROTECT(level);
  r = wait_find(pcb, 1);
  if (r) {
    r->last_rto_id = id;
    if (r->active) r->current.rto_id = id;
  }
  SYS_ARCH_UNPROTECT(level);
}

void tcp_wait_cancel(const struct tcp_pcb *pcb, u8_t result)
{
  struct tcp_wait_row *r;
  u32_t now;
  SYS_ARCH_DECL_PROTECT(level);
  if (!pcb->recovery.usb_screen_rto) return;
  now = sys_now();
  SYS_ARCH_PROTECT(level);
  r = wait_find(pcb, 0);
  if (r) {
    wait_finish(r, now, result, 0, 0);
    if (result == TCP_WAIT_CLOSED) r->closed = 1;
  }
  SYS_ARCH_UNPROTECT(level);
}

int tcp_wait_snapshot(unsigned index, struct tcp_wait_row *out, u32_t *evicted)
{
  int found = 0;
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  *evicted = wait_evicted;
  if (index < TCP_WAIT_ROWS && wait_rows[index].connection) {
    *out = wait_rows[index]; out->snapshot_ms = sys_now(); found = 1;
  }
  SYS_ARCH_UNPROTECT(level);
  return found;
}

int tcp_wait_pop(struct tcp_wait_event *out, u32_t *dropped)
{
  int found;
  SYS_ARCH_DECL_PROTECT(level);
  SYS_ARCH_PROTECT(level);
  *dropped = wait_dropped; found = wait_count != 0;
  if (found) {
    *out = wait_events[wait_head]; wait_head = (wait_head + 1U) % TCP_WAIT_EVENTS;
    --wait_count;
  }
  SYS_ARCH_UNPROTECT(level);
  return found;
}
#endif
