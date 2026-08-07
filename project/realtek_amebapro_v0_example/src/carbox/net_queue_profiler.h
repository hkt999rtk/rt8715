#ifndef CARBOX_NET_QUEUE_PROFILER_H
#define CARBOX_NET_QUEUE_PROFILER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RX hooks bracket sys_mbox_trypost().  begin must run before the queue item
 * becomes visible, otherwise a higher-priority TCP_IP task could consume it
 * before the pending counter is incremented. */
/* if_id packs netif index/num/name and is generated at the lwIP boundary. */
void net_queue_profiler_rx_post_begin(uint32_t if_id);
void net_queue_profiler_rx_post_commit(uint32_t if_id, uint32_t bytes,
				       uint32_t mailbox_depth);
void net_queue_profiler_rx_post_abort(uint32_t if_id);
void net_queue_profiler_rx_alloc_fail(uint32_t if_id);
void net_queue_profiler_rx_consume(uint32_t if_id, uint32_t bytes,
				   uint32_t mailbox_depth);

/* Stage-1 pbuf-pointer mailbox validation.  It does not aggregate or delay. */
void net_queue_profiler_rx_stage1_post(uint32_t sequence, uint32_t in_use);
void net_queue_profiler_rx_stage1_consume(uint32_t sequence, uint32_t in_use);
void net_queue_profiler_rx_stage1_pool_fallback(void);
void net_queue_profiler_rx_stage1_post_fail(void);
void net_queue_profiler_rx_stage1_state_error(void);
void net_queue_profiler_rx_timer_init(uint32_t timer_id, uint32_t available);
void net_queue_profiler_rx_timer_arm(void);
void net_queue_profiler_rx_timer_skip(void);
void net_queue_profiler_rx_timer_fire(uint32_t elapsed_us);
void net_queue_profiler_rx_timer_mbox_post(uint32_t task_woken);
void net_queue_profiler_rx_timer_mbox_fail(void);
void net_queue_profiler_rx_timer_mbox_dispatch(void);
void net_queue_profiler_rx_batch_enqueue(uint32_t pending);
void net_queue_profiler_rx_batch_post(uint32_t timer_trigger);
void net_queue_profiler_rx_batch_post_fail(uint32_t timer_trigger);
void net_queue_profiler_rx_batch_dispatch(uint32_t packets,
					  uint32_t oldest_age_us,
					  uint32_t pending_remaining);
void net_queue_profiler_rx_batch_timer_empty(void);

/* Called from TCP_IP context, where walking the active PCB list is safe. */
void net_queue_profiler_tcp_sample(uint32_t active_pcbs,
				   uint32_t snd_queue_len,
				   uint32_t unsent_segments,
				   uint32_t unacked_segments,
				   uint32_t queued_bytes,
				   uint32_t refused_pbufs,
				   uint32_t refused_bytes);

void net_queue_profiler_report(uint32_t sequence);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_NET_QUEUE_PROFILER_H */
