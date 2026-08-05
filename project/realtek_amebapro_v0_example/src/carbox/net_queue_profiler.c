#include "net_queue_profiler.h"

#include <stdint.h>

#include "diag.h"

#ifndef CONFIG_NET_QUEUE_PROFILE
#define CONFIG_NET_QUEUE_PROFILE 0
#endif

#if CONFIG_NET_QUEUE_PROFILE

#define NETQ_IF_SLOTS 8U

typedef struct net_queue_iface_profile_s {
	volatile uint32_t id;
	volatile uint32_t post_ops;
	volatile uint32_t post_bytes;
	volatile uint32_t consume_ops;
	volatile uint32_t consume_bytes;
	volatile uint32_t pending;
	volatile uint32_t pending_peak;
	volatile uint32_t alloc_fail;
	volatile uint32_t post_fail;
} net_queue_iface_profile_t;

typedef struct net_queue_profile_s {
	volatile uint32_t rx_post_ops;
	volatile uint32_t rx_post_bytes;
	volatile uint32_t rx_consume_ops;
	volatile uint32_t rx_consume_bytes;
	volatile uint32_t rx_pending;
	volatile uint32_t rx_pending_peak;
	volatile uint32_t mailbox_depth;
	volatile uint32_t mailbox_depth_peak;
	volatile uint32_t rx_alloc_fail;
	volatile uint32_t rx_post_fail;

	volatile uint32_t tcp_samples;
	volatile uint32_t active_pcbs;
	volatile uint32_t active_pcbs_peak;
	volatile uint32_t snd_queue_len;
	volatile uint32_t snd_queue_len_peak;
	volatile uint32_t unsent_segments;
	volatile uint32_t unsent_segments_peak;
	volatile uint32_t unacked_segments;
	volatile uint32_t unacked_segments_peak;
	volatile uint32_t queued_bytes;
	volatile uint32_t queued_bytes_peak;
	volatile uint32_t refused_pbufs;
	volatile uint32_t refused_pbufs_peak;
	volatile uint32_t refused_bytes;
	volatile uint32_t refused_bytes_peak;
	net_queue_iface_profile_t iface[NETQ_IF_SLOTS];
} net_queue_profile_t;

static net_queue_profile_t netq;

static uint32_t netq_add(volatile uint32_t *value, uint32_t amount)
{
	return __atomic_add_fetch(value, amount, __ATOMIC_RELAXED);
}

static void netq_sub(volatile uint32_t *value, uint32_t amount)
{
	(void)__atomic_sub_fetch(value, amount, __ATOMIC_RELAXED);
}

static uint32_t netq_load(volatile uint32_t *value)
{
	return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static uint32_t netq_exchange(volatile uint32_t *value, uint32_t replacement)
{
	return __atomic_exchange_n(value, replacement, __ATOMIC_RELAXED);
}

static void netq_update_peak(volatile uint32_t *peak, uint32_t sample)
{
	uint32_t old = netq_load(peak);

	while (sample > old &&
	       !__atomic_compare_exchange_n(peak, &old, sample, 1,
					    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
		/* old is refreshed by compare_exchange. */
	}
}

static net_queue_iface_profile_t *netq_iface(uint32_t if_id)
{
	uint32_t slot = if_id & 0xffU;

	if (slot >= NETQ_IF_SLOTS) {
		slot = 0U;
	}
	__atomic_store_n(&netq.iface[slot].id, if_id, __ATOMIC_RELAXED);
	return &netq.iface[slot];
}

void net_queue_profiler_rx_post_begin(uint32_t if_id)
{
	net_queue_iface_profile_t *iface = netq_iface(if_id);
	uint32_t pending;

	pending = netq_add(&netq.rx_pending, 1U);
	netq_update_peak(&netq.rx_pending_peak, pending);
	pending = netq_add(&iface->pending, 1U);
	netq_update_peak(&iface->pending_peak, pending);
}

void net_queue_profiler_rx_post_commit(uint32_t if_id, uint32_t bytes,
				       uint32_t mailbox_depth)
{
	net_queue_iface_profile_t *iface = netq_iface(if_id);

	netq_add(&netq.rx_post_ops, 1U);
	netq_add(&netq.rx_post_bytes, bytes);
	netq_add(&iface->post_ops, 1U);
	netq_add(&iface->post_bytes, bytes);
	__atomic_store_n(&netq.mailbox_depth, mailbox_depth, __ATOMIC_RELAXED);
	netq_update_peak(&netq.mailbox_depth_peak, mailbox_depth);
}

void net_queue_profiler_rx_post_abort(uint32_t if_id)
{
	net_queue_iface_profile_t *iface = netq_iface(if_id);

	netq_sub(&netq.rx_pending, 1U);
	netq_add(&netq.rx_post_fail, 1U);
	netq_sub(&iface->pending, 1U);
	netq_add(&iface->post_fail, 1U);
}

void net_queue_profiler_rx_alloc_fail(uint32_t if_id)
{
	net_queue_iface_profile_t *iface = netq_iface(if_id);

	netq_add(&netq.rx_alloc_fail, 1U);
	netq_add(&iface->alloc_fail, 1U);
}

void net_queue_profiler_rx_consume(uint32_t if_id, uint32_t bytes,
				   uint32_t mailbox_depth)
{
	net_queue_iface_profile_t *iface = netq_iface(if_id);

	netq_add(&netq.rx_consume_ops, 1U);
	netq_add(&netq.rx_consume_bytes, bytes);
	netq_sub(&netq.rx_pending, 1U);
	netq_add(&iface->consume_ops, 1U);
	netq_add(&iface->consume_bytes, bytes);
	netq_sub(&iface->pending, 1U);
	__atomic_store_n(&netq.mailbox_depth, mailbox_depth, __ATOMIC_RELAXED);
}

void net_queue_profiler_tcp_sample(uint32_t active_pcbs,
				   uint32_t snd_queue_len,
				   uint32_t unsent_segments,
				   uint32_t unacked_segments,
				   uint32_t queued_bytes,
				   uint32_t refused_pbufs,
				   uint32_t refused_bytes)
{
	netq_add(&netq.tcp_samples, 1U);
	__atomic_store_n(&netq.active_pcbs, active_pcbs, __ATOMIC_RELAXED);
	__atomic_store_n(&netq.snd_queue_len, snd_queue_len, __ATOMIC_RELAXED);
	__atomic_store_n(&netq.unsent_segments, unsent_segments, __ATOMIC_RELAXED);
	__atomic_store_n(&netq.unacked_segments, unacked_segments, __ATOMIC_RELAXED);
	__atomic_store_n(&netq.queued_bytes, queued_bytes, __ATOMIC_RELAXED);
	__atomic_store_n(&netq.refused_pbufs, refused_pbufs, __ATOMIC_RELAXED);
	__atomic_store_n(&netq.refused_bytes, refused_bytes, __ATOMIC_RELAXED);
	netq_update_peak(&netq.active_pcbs_peak, active_pcbs);
	netq_update_peak(&netq.snd_queue_len_peak, snd_queue_len);
	netq_update_peak(&netq.unsent_segments_peak, unsent_segments);
	netq_update_peak(&netq.unacked_segments_peak, unacked_segments);
	netq_update_peak(&netq.queued_bytes_peak, queued_bytes);
	netq_update_peak(&netq.refused_pbufs_peak, refused_pbufs);
	netq_update_peak(&netq.refused_bytes_peak, refused_bytes);
}

void net_queue_profiler_report(uint32_t sequence)
{
	uint32_t i;
	uint32_t rx_post_ops = netq_exchange(&netq.rx_post_ops, 0U);
	uint32_t rx_post_bytes = netq_exchange(&netq.rx_post_bytes, 0U);
	uint32_t rx_consume_ops = netq_exchange(&netq.rx_consume_ops, 0U);
	uint32_t rx_consume_bytes = netq_exchange(&netq.rx_consume_bytes, 0U);
	uint32_t rx_alloc_fail = netq_exchange(&netq.rx_alloc_fail, 0U);
	uint32_t rx_post_fail = netq_exchange(&netq.rx_post_fail, 0U);
	uint32_t rx_pending_peak = netq_exchange(&netq.rx_pending_peak,
						 netq_load(&netq.rx_pending));
	uint32_t mailbox_depth_peak = netq_exchange(&netq.mailbox_depth_peak,
						    netq_load(&netq.mailbox_depth));
	uint32_t tcp_samples = netq_exchange(&netq.tcp_samples, 0U);

	rt_printf("[NETQ][%lu] RX post=%lu/%luB consume=%lu/%luB pending_now/max=%lu/%lu "
		    "tcpip_mbox_now/max=%lu/%lu alloc_fail=%lu post_fail=%lu\n",
		    (unsigned long)sequence,
		    (unsigned long)rx_post_ops, (unsigned long)rx_post_bytes,
		    (unsigned long)rx_consume_ops, (unsigned long)rx_consume_bytes,
		    (unsigned long)netq_load(&netq.rx_pending),
		    (unsigned long)rx_pending_peak,
		    (unsigned long)netq_load(&netq.mailbox_depth),
		    (unsigned long)mailbox_depth_peak,
		    (unsigned long)rx_alloc_fail, (unsigned long)rx_post_fail);

	rt_printf("[NETQ][%lu] TX samples=%lu pcbs_now/max=%lu/%lu snd_qlen_now/max=%lu/%lu "
		    "seg_unsent_now/max=%lu/%lu seg_unacked_now/max=%lu/%lu queued_now/max=%lu/%luB\n",
		    (unsigned long)sequence, (unsigned long)tcp_samples,
		    (unsigned long)netq_load(&netq.active_pcbs),
		    (unsigned long)netq_exchange(&netq.active_pcbs_peak,
						   netq_load(&netq.active_pcbs)),
		    (unsigned long)netq_load(&netq.snd_queue_len),
		    (unsigned long)netq_exchange(&netq.snd_queue_len_peak,
						   netq_load(&netq.snd_queue_len)),
		    (unsigned long)netq_load(&netq.unsent_segments),
		    (unsigned long)netq_exchange(&netq.unsent_segments_peak,
						   netq_load(&netq.unsent_segments)),
		    (unsigned long)netq_load(&netq.unacked_segments),
		    (unsigned long)netq_exchange(&netq.unacked_segments_peak,
						   netq_load(&netq.unacked_segments)),
		    (unsigned long)netq_load(&netq.queued_bytes),
		    (unsigned long)netq_exchange(&netq.queued_bytes_peak,
						   netq_load(&netq.queued_bytes)));

	rt_printf("[NETQ][%lu] RX_APP refused_pbufs_now/max=%lu/%lu refused_bytes_now/max=%lu/%luB\n",
		    (unsigned long)sequence,
		    (unsigned long)netq_load(&netq.refused_pbufs),
		    (unsigned long)netq_exchange(&netq.refused_pbufs_peak,
						   netq_load(&netq.refused_pbufs)),
		    (unsigned long)netq_load(&netq.refused_bytes),
		    (unsigned long)netq_exchange(&netq.refused_bytes_peak,
						   netq_load(&netq.refused_bytes)));

	for (i = 0U; i < NETQ_IF_SLOTS; ++i) {
		net_queue_iface_profile_t *iface = &netq.iface[i];
		uint32_t id = netq_load(&iface->id);
		uint32_t post_ops = netq_exchange(&iface->post_ops, 0U);
		uint32_t post_bytes = netq_exchange(&iface->post_bytes, 0U);
		uint32_t consume_ops = netq_exchange(&iface->consume_ops, 0U);
		uint32_t consume_bytes = netq_exchange(&iface->consume_bytes, 0U);
		uint32_t alloc_fail = netq_exchange(&iface->alloc_fail, 0U);
		uint32_t post_fail = netq_exchange(&iface->post_fail, 0U);
		uint32_t pending = netq_load(&iface->pending);
		uint32_t pending_peak = netq_exchange(&iface->pending_peak, pending);

		if (id == 0U && post_ops == 0U && consume_ops == 0U &&
		    pending == 0U && alloc_fail == 0U && post_fail == 0U) {
			continue;
		}
		rt_printf("[NETQ][%lu][%c%c%lu idx=%lu] RX post=%lu/%luB consume=%lu/%luB "
			  "pending_now/max=%lu/%lu alloc_fail=%lu post_fail=%lu\n",
			  (unsigned long)sequence,
			  (char)((id >> 16) & 0xffU), (char)((id >> 24) & 0xffU),
			  (unsigned long)((id >> 8) & 0xffU),
			  (unsigned long)(id & 0xffU),
			  (unsigned long)post_ops, (unsigned long)post_bytes,
			  (unsigned long)consume_ops, (unsigned long)consume_bytes,
			  (unsigned long)pending, (unsigned long)pending_peak,
			  (unsigned long)alloc_fail, (unsigned long)post_fail);
	}
}

#else

void net_queue_profiler_rx_post_begin(uint32_t id) { (void)id; }
void net_queue_profiler_rx_post_commit(uint32_t id, uint32_t bytes, uint32_t depth)
{ (void)id; (void)bytes; (void)depth; }
void net_queue_profiler_rx_post_abort(uint32_t id) { (void)id; }
void net_queue_profiler_rx_alloc_fail(uint32_t id) { (void)id; }
void net_queue_profiler_rx_consume(uint32_t id, uint32_t bytes, uint32_t depth)
{ (void)id; (void)bytes; (void)depth; }
void net_queue_profiler_tcp_sample(uint32_t a, uint32_t b, uint32_t c,
				   uint32_t d, uint32_t e, uint32_t f, uint32_t g)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; }
void net_queue_profiler_report(uint32_t sequence) { (void)sequence; }

#endif
