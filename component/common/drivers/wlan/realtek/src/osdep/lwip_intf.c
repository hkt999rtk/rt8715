/* mbed Microcontroller Library
 * Copyright (c) 2013-2016 Realtek Semiconductor Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *                                        
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
//#define _LWIP_INTF_C_

#include <autoconf.h>
#include <lwip_intf.h>
#include <lwip/netif.h>
#if !defined(CONFIG_MBED_ENABLED)
#include <lwip_netconf.h>
#include <ethernetif.h>
#endif
#include <osdep_service.h>
#include <wifi/wifi_util.h>
#if defined(CONFIG_PLATFORM_8195BHP)
#include <dma_api.h>
#include <hal_gdma.h>
#include <stdint.h>
#include <stdio.h>
#endif
//----- ------------------------------------------------------------------
// External Reference
//----- ------------------------------------------------------------------
#if (CONFIG_LWIP_LAYER == 1)
extern struct netif xnetif[];			//LWIP netif
#endif

#if (CONFIG_LWIP_LAYER == 1) && defined(CONFIG_PLATFORM_8195BHP)

#ifndef CONFIG_NET_GDMA_COPY
#define CONFIG_NET_GDMA_COPY 1
#endif

#ifndef CONFIG_NET_GDMA_SELFTEST
#define CONFIG_NET_GDMA_SELFTEST 1
#endif

#ifndef CONFIG_NET_GDMA_STATS
#define CONFIG_NET_GDMA_STATS 1
#endif

#ifndef NET_GDMA_COPY_THRESHOLD
#define NET_GDMA_COPY_THRESHOLD 1024U
#endif

#ifndef NET_GDMA_TIMEOUT_MS
#define NET_GDMA_TIMEOUT_MS 10U
#endif

#define NET_GDMA_CACHE_LINE       32U
#define NET_GDMA_MAX_COPY_LEN   4095U
#define NET_GDMA_REPORT_MS      5000U
#define NET_GDMA_REPORT_PROBE    256U
#define NET_GDMA_SELFTEST_MAX   1500U
#define NET_GDMA_SELFTEST_ALLOC (NET_GDMA_SELFTEST_MAX + (3U * NET_GDMA_CACHE_LINE))

typedef struct net_gdma_context_s {
	gdma_t dma;
	_sema done;
	const char *name;
	u8 initialized;
	u8 available;
	u8 tested;
	volatile u8 busy;
	volatile u8 in_flight;
	u32 dma_ops;
	u32 dma_bytes;
	u32 cpu_ops;
	u32 cpu_bytes;
	u32 alignment_fallbacks;
	u32 reentry_fallbacks;
	u32 timeouts;
	u32 report_probe;
	u32 last_report_tick;
} net_gdma_context_t;

static net_gdma_context_t g_net_rx_gdma;
static net_gdma_context_t g_net_tx_gdma;
static u8 g_net_gdma_initialized;

static void net_gdma_rx_done(uint32_t id)
{
	(void)id;
	if (g_net_rx_gdma.done != NULL) {
		rtw_up_sema_from_isr(&g_net_rx_gdma.done);
	}
}

static void net_gdma_tx_done(uint32_t id)
{
	(void)id;
	if (g_net_tx_gdma.done != NULL) {
		rtw_up_sema_from_isr(&g_net_tx_gdma.done);
	}
}

static void net_gdma_drain_completion(net_gdma_context_t *ctx)
{
	while (rtw_down_timeout_sema(&ctx->done, 0) == _TRUE) {
	}
}

static void net_gdma_init_context(net_gdma_context_t *ctx,
				  const char *name, dma_irq_handler callback)
{
	phal_gdma_adaptor_t adaptor;

	ctx->name = name;
	ctx->initialized = 1;
	rtw_init_sema(&ctx->done, 0);
	if (ctx->done == NULL) {
		printf("[NET_GDMA] %s semaphore allocation failed; CPU copy only\n", name);
		return;
	}

	/*
	 * RTL8195B's dma_api wrapper ignores its id argument and passes the HAL
	 * adaptor to the callback.  Separate callbacks avoid relying on that bug.
	 */
	dma_memcpy_init(&ctx->dma, callback, 0);
	adaptor = &ctx->dma.hal_gdma_adaptor;
	if (!adaptor->have_chnl) {
		printf("[NET_GDMA] %s channel allocation failed; CPU copy only\n", name);
		rtw_free_sema(&ctx->done);
		return;
	}

	ctx->available = 1;
	ctx->last_report_tick = rtw_get_current_time();
	printf("[NET_GDMA] %s allocated gdma=%u channel=%u threshold=%u\n",
	       name, (unsigned int)adaptor->gdma_index,
	       (unsigned int)adaptor->ch_num,
	       (unsigned int)NET_GDMA_COPY_THRESHOLD);
}

static void net_gdma_init_all(void)
{
#if CONFIG_NET_GDMA_COPY
	if (g_net_gdma_initialized) {
		return;
	}

	/* WLAN setup is serialized before packet traffic starts. */
	g_net_gdma_initialized = 1;
	net_gdma_init_context(&g_net_rx_gdma, "RX", net_gdma_rx_done);
	net_gdma_init_context(&g_net_tx_gdma, "TX", net_gdma_tx_done);
#endif
}

static int net_gdma_destination_safe(const void *dst, u32 len,
				     const void *allocation_end)
{
	uintptr_t start = (uintptr_t)dst;
	uintptr_t end;
	uintptr_t rounded_end;

	if ((start & (NET_GDMA_CACHE_LINE - 1U)) != 0U || len == 0U ||
	    len > NET_GDMA_MAX_COPY_LEN) {
		return 0;
	}

	end = start + len;
	if (end < start) {
		return 0;
	}

	if (allocation_end != NULL) {
		rounded_end = (end + NET_GDMA_CACHE_LINE - 1U) &
			      ~(uintptr_t)(NET_GDMA_CACHE_LINE - 1U);
		if (rounded_end < end || rounded_end > (uintptr_t)allocation_end) {
			return 0;
		}
	}

	return 1;
}

static int net_gdma_wait(net_gdma_context_t *ctx)
{
	if (rtw_down_timeout_sema(&ctx->done, NET_GDMA_TIMEOUT_MS) == _TRUE) {
		ctx->in_flight = 0;
		return 0;
	}

	ctx->timeouts++;
	printf("[NET_GDMA][ERROR] %s timeout after %u ms gdma=%u channel=%u\n",
	       ctx->name, (unsigned int)NET_GDMA_TIMEOUT_MS,
	       (unsigned int)ctx->dma.hal_gdma_adaptor.gdma_index,
	       (unsigned int)ctx->dma.hal_gdma_adaptor.ch_num);

	/* Stop DMA before its destination is released or reused. */
	hal_gdma_abort(&ctx->dma.hal_gdma_adaptor);
	dma_memcpy_deinit(&ctx->dma);
	ctx->in_flight = 0;
	ctx->available = 0;
	ctx->tested = 0;
	net_gdma_drain_completion(ctx);

	/* Try to restore this direction for the next packet. */
	dma_memcpy_init(&ctx->dma,
		(ctx == &g_net_rx_gdma) ? net_gdma_rx_done : net_gdma_tx_done, 0);
	if (ctx->dma.hal_gdma_adaptor.have_chnl) {
		ctx->available = 1;
		printf("[NET_GDMA] %s channel recovered; self-test required\n", ctx->name);
	} else {
		printf("[NET_GDMA] %s recovery failed; CPU copy only\n", ctx->name);
	}

	return -1;
}

#if CONFIG_NET_GDMA_SELFTEST
static int net_gdma_selftest(net_gdma_context_t *ctx)
{
	static const u16 lengths[] = { 1024U, 1500U };
	u8 *src_raw;
	u8 *dst_raw;
	u8 *src_base;
	u8 *dst;
	u32 case_index;
	u32 offset;
	u32 i;
	int result = -1;

	src_raw = (u8 *)rtw_malloc(NET_GDMA_SELFTEST_ALLOC);
	dst_raw = (u8 *)rtw_malloc(NET_GDMA_SELFTEST_ALLOC);
	if (src_raw == NULL || dst_raw == NULL) {
		printf("[NET_GDMA][SELFTEST] %s allocation failed\n", ctx->name);
		goto exit;
	}

	src_base = (u8 *)(((uintptr_t)src_raw + NET_GDMA_CACHE_LINE - 1U) &
			  ~(uintptr_t)(NET_GDMA_CACHE_LINE - 1U));
	dst = (u8 *)(((uintptr_t)dst_raw + NET_GDMA_CACHE_LINE - 1U) &
		     ~(uintptr_t)(NET_GDMA_CACHE_LINE - 1U));

	for (case_index = 0; case_index < sizeof(lengths) / sizeof(lengths[0]);
	     ++case_index) {
		for (offset = 0; offset < 4U; ++offset) {
			u8 *src = src_base + offset;
			u32 len = lengths[case_index];

			for (i = 0; i < len; ++i) {
				src[i] = (u8)(i + (offset * 29U) + len);
			}
			rtw_memset(dst, 0xA5, len);
			net_gdma_drain_completion(ctx);
			ctx->in_flight = 1;
			dma_memcpy(&ctx->dma, dst, src, len);
			if (net_gdma_wait(ctx) != 0 ||
			    rtw_memcmp(dst, src, len) != _TRUE) {
				printf("[NET_GDMA][SELFTEST] %s FAIL len=%u src_offset=%u\n",
				       ctx->name, (unsigned int)len,
				       (unsigned int)offset);
				goto exit;
			}
		}
	}

	result = 0;
	printf("[NET_GDMA][SELFTEST] %s PASS cases=%u\n", ctx->name,
	       (unsigned int)(sizeof(lengths) / sizeof(lengths[0]) * 4U));

exit:
	if (src_raw != NULL) {
		rtw_mfree(src_raw, NET_GDMA_SELFTEST_ALLOC);
	}
	if (dst_raw != NULL) {
		rtw_mfree(dst_raw, NET_GDMA_SELFTEST_ALLOC);
	}
	return result;
}
#endif

static int net_gdma_ensure_tested(net_gdma_context_t *ctx)
{
	if (!ctx->available) {
		return -1;
	}
	if (ctx->tested) {
		return 0;
	}

#if CONFIG_NET_GDMA_SELFTEST
	if (net_gdma_selftest(ctx) != 0) {
		if (ctx->available) {
			dma_memcpy_deinit(&ctx->dma);
			ctx->available = 0;
		}
		printf("[NET_GDMA] %s disabled after self-test failure\n", ctx->name);
		return -1;
	}
#endif
	ctx->tested = 1;
	return 0;
}

static void net_gdma_account(net_gdma_context_t *ctx, int used_dma, u32 len)
{
#if CONFIG_NET_GDMA_STATS
	u32 now;
	u32 elapsed_ms;

	if (used_dma) {
		ctx->dma_ops++;
		ctx->dma_bytes += len;
	} else {
		ctx->cpu_ops++;
		ctx->cpu_bytes += len;
	}

	ctx->report_probe++;
	if ((ctx->report_probe % NET_GDMA_REPORT_PROBE) != 0U) {
		return;
	}

	now = rtw_get_current_time();
	elapsed_ms = rtw_systime_to_ms(now - ctx->last_report_tick);
	if (elapsed_ms < NET_GDMA_REPORT_MS) {
		return;
	}

	printf("[NET_GDMA][%s] window_ms=%u dma=%u/%uB cpu=%u/%uB "
	       "align_fallback=%u reentry=%u timeout=%u\n",
	       ctx->name, (unsigned int)elapsed_ms,
	       (unsigned int)ctx->dma_ops, (unsigned int)ctx->dma_bytes,
	       (unsigned int)ctx->cpu_ops, (unsigned int)ctx->cpu_bytes,
	       (unsigned int)ctx->alignment_fallbacks,
	       (unsigned int)ctx->reentry_fallbacks,
	       (unsigned int)ctx->timeouts);

	ctx->dma_ops = 0;
	ctx->dma_bytes = 0;
	ctx->cpu_ops = 0;
	ctx->cpu_bytes = 0;
	ctx->alignment_fallbacks = 0;
	ctx->reentry_fallbacks = 0;
	ctx->timeouts = 0;
	ctx->last_report_tick = now;
#else
	(void)ctx;
	(void)used_dma;
	(void)len;
#endif
}

static int net_gdma_copy(net_gdma_context_t *ctx, void *dst, const void *src,
			 u32 len, const void *allocation_end)
{
	if (!g_net_gdma_initialized) {
		net_gdma_init_all();
	}

	if (len < NET_GDMA_COPY_THRESHOLD || !ctx->available) {
		rtw_memcpy(dst, (void *)src, len);
		net_gdma_account(ctx, 0, len);
		return 0;
	}

	if (!net_gdma_destination_safe(dst, len, allocation_end)) {
		ctx->alignment_fallbacks++;
		rtw_memcpy(dst, (void *)src, len);
		net_gdma_account(ctx, 0, len);
		return 0;
	}

	/*
	 * WLAN and NCM share one channel per direction.  Claim it atomically with
	 * only a few instructions of IRQ masking; no mutex is held while waiting.
	 */
	save_and_cli();
	if (ctx->busy) {
		restore_flags();
		/*
		 * The other producer owns this direction.  Its buffer is independent,
		 * so CPU fallback is safe and avoids queueing behind the DMA channel.
		 */
		ctx->reentry_fallbacks++;
		rtw_memcpy(dst, (void *)src, len);
		net_gdma_account(ctx, 0, len);
		return 0;
	}
	ctx->busy = 1;
	restore_flags();

	if (net_gdma_ensure_tested(ctx) != 0) {
		ctx->busy = 0;
		rtw_memcpy(dst, (void *)src, len);
		net_gdma_account(ctx, 0, len);
		return 0;
	}

	net_gdma_drain_completion(ctx);
	ctx->in_flight = 1;
	dma_memcpy(&ctx->dma, dst, (void *)src, len);
	if (net_gdma_wait(ctx) != 0) {
		/* Destination may be partially written.  Caller must drop the packet. */
		ctx->busy = 0;
		return -1;
	}

	ctx->busy = 0;
	net_gdma_account(ctx, 1, len);
	return 0;
}

/*
 * A cache invalidate must not discard unrelated dirty bytes sharing the first
 * or last destination cache line.  WLAN skb/pbuf destinations are normally
 * aligned, but NCM scatter/gather boundaries are not.  Copy the unaligned
 * edges on the CPU and use GDMA only for a cache-line-isolated middle region.
 */
static int net_gdma_copy_with_edges(net_gdma_context_t *ctx, void *dst,
				    const void *src, u32 len,
				    const void *allocation_end)
{
	uintptr_t start = (uintptr_t)dst;
	u32 prefix;
	u32 body;
	u32 suffix;

	if (len < NET_GDMA_COPY_THRESHOLD || !ctx->available || ctx->busy ||
	    (((start & (NET_GDMA_CACHE_LINE - 1U)) == 0U) &&
	     (allocation_end != NULL ||
	      (len & (NET_GDMA_CACHE_LINE - 1U)) == 0U))) {
		return net_gdma_copy(ctx, dst, src, len, allocation_end);
	}

	prefix = (start & (NET_GDMA_CACHE_LINE - 1U)) != 0U ?
		 NET_GDMA_CACHE_LINE -
		 (u32)(start & (NET_GDMA_CACHE_LINE - 1U)) : 0U;
	if (prefix >= len) {
		return net_gdma_copy(ctx, dst, src, len, allocation_end);
	}
	body = (len - prefix) & ~(NET_GDMA_CACHE_LINE - 1U);
	if (body < NET_GDMA_COPY_THRESHOLD) {
		return net_gdma_copy(ctx, dst, src, len, allocation_end);
	}
	suffix = len - prefix - body;

	if (prefix != 0U) {
		rtw_memcpy(dst, (void *)src, prefix);
		net_gdma_account(ctx, 0, prefix);
	}
	if (net_gdma_copy(ctx, (u8 *)dst + prefix, (const u8 *)src + prefix,
			  body, allocation_end) != 0) {
		return -1;
	}
	if (suffix != 0U) {
		rtw_memcpy((u8 *)dst + prefix + body,
			   (void *)((const u8 *)src + prefix + body), suffix);
		net_gdma_account(ctx, 0, suffix);
	}
	return 0;
}

int rltk_network_gdma_copy_tx(void *dst, const void *src, unsigned int len,
			      const void *allocation_end)
{
	return net_gdma_copy_with_edges(&g_net_tx_gdma, dst, src, len,
					allocation_end);
}

int rltk_network_gdma_copy_rx(void *dst, const void *src, unsigned int len,
			      const void *allocation_end)
{
	return net_gdma_copy_with_edges(&g_net_rx_gdma, dst, src, len,
					allocation_end);
}

#endif /* CONFIG_LWIP_LAYER && CONFIG_PLATFORM_8195BHP */

/**
 *      rltk_wlan_set_netif_info - set netif hw address and register dev pointer to netif device
 *      @idx_wlan: netif index
 *			    0 for STA only or SoftAP only or STA in STA+SoftAP concurrent mode, 
 *			    1 for SoftAP in STA+SoftAP concurrent mode
 *      @dev: register netdev pointer to LWIP. Reserved.
 *      @dev_addr: set netif hw address
 *
 *      Return Value: None
 */     
void rltk_wlan_set_netif_info(int idx_wlan, void * dev, unsigned char * dev_addr)
{
#if (CONFIG_LWIP_LAYER == 1)
#if defined(CONFIG_MBED_ENABLED)
	//rtw_memcpy(xnetif[idx_wlan]->hwaddr, dev_addr, 6);
	//set netif hwaddr later
#else
	rtw_memcpy(xnetif[idx_wlan].hwaddr, dev_addr, 6);
	xnetif[idx_wlan].state = dev;
#endif
#if defined(CONFIG_PLATFORM_8195BHP)
	net_gdma_init_all();
#endif
#endif
}

/**
 *      rltk_wlan_send - send IP packets to WLAN. Called by low_level_output().
 *      @idx: netif index
 *      @sg_list: data buffer list
 *      @sg_len: size of each data buffer
 *      @total_len: total data len
 *
 *      Return Value: None
 */     
static void rltk_wlan_dump_netif_down(int idx, int total_len)
{
#if (CONFIG_LWIP_LAYER == 1)
	if (idx >= 0 && idx < NET_IF_NUM) {
		struct netif *netif = &xnetif[idx];
		const ip4_addr_t *ip = netif_ip4_addr(netif);

		DBG_ERR("[%c%c] netif is DOWN idx=%d len=%d num=%u "
			"flags=0x%02x up=%u link=%u drv_up=%u state=%p "
			"ip=%u.%u.%u.%u",
			netif->name[0], netif->name[1],
			idx, total_len, (unsigned int)netif->num,
			(unsigned int)netif->flags,
			(unsigned int)netif_is_up(netif),
			(unsigned int)netif_is_link_up(netif),
			(unsigned int)rltk_wlan_check_isup(idx), netif->state,
			(unsigned int)ip4_addr1(ip), (unsigned int)ip4_addr2(ip),
			(unsigned int)ip4_addr3(ip), (unsigned int)ip4_addr4(ip));
	} else {
		DBG_ERR("[??] netif is DOWN idx=%d len=%d (invalid WLAN index)",
			idx, total_len);
	}
#else
	(void)idx;
	(void)total_len;
#endif
}

int rltk_wlan_send(int idx, struct eth_drv_sg *sg_list, int sg_len, int total_len)
{
#if (CONFIG_LWIP_LAYER == 1)
	struct eth_drv_sg *last_sg;
	struct sk_buff *skb = NULL;
	unsigned int skb_alloc_len = (unsigned int)total_len;
	int ret = 0;

	if(idx == -1){
		rltk_wlan_dump_netif_down(idx, total_len);
		return -1;
	}
	DBG_TRACE("%s is called", __FUNCTION__);

	save_and_cli();
	if (rltk_wlan_check_isup(idx)) {
		rltk_wlan_tx_inc(idx);
	} else {
		restore_flags();
		rltk_wlan_dump_netif_down(idx, total_len);
		return -1;
	}
	restore_flags();

#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_NET_GDMA_COPY
	/*
	 * rltk_wlan_alloc_skb() reserves only a 4-byte-aligned WLAN headroom.
	 * Give large TX packets one extra cache line plus alignment slack so the
	 * DMA destination can be isolated from adjacent allocator data.  The skb
	 * payload length is still total_len; this changes capacity only.
	 */
	if ((unsigned int)total_len >= NET_GDMA_COPY_THRESHOLD &&
	    (unsigned int)total_len <= NET_GDMA_MAX_COPY_LEN) {
		skb_alloc_len += (2U * NET_GDMA_CACHE_LINE) - 1U;
	}
#endif
	skb = rltk_wlan_alloc_skb(skb_alloc_len);
	if (skb == NULL) {
		//DBG_ERR("rltk_wlan_alloc_skb() for data len=%d failed!", total_len);
		ret = -1;
		goto exit;
	}

#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_NET_GDMA_COPY
	if (skb_alloc_len != (unsigned int)total_len) {
		uintptr_t tail = (uintptr_t)skb->tail;
		unsigned int alignment = (unsigned int)
			((-tail) & (NET_GDMA_CACHE_LINE - 1U));

		skb_reserve(skb, alignment);
	}
#endif

	for (last_sg = &sg_list[sg_len]; sg_list < last_sg; ++sg_list) {
#if defined(CONFIG_PLATFORM_8195BHP)
		if (net_gdma_copy(&g_net_tx_gdma, skb->tail,
				  (void *)(sg_list->buf), sg_list->len,
				  skb->end) != 0) {
			printf("[NET_GDMA][ERROR] TX packet dropped len=%u\n",
			       (unsigned int)sg_list->len);
			kfree_skb(skb);
			skb = NULL;
			ret = -1;
			goto exit;
		}
#else
		rtw_memcpy(skb->tail, (void *)(sg_list->buf), sg_list->len);
#endif
		skb_put(skb,  sg_list->len);
	}

	rltk_wlan_send_skb(idx, skb);

exit:
	save_and_cli();
	rltk_wlan_tx_dec(idx);
	restore_flags();
	return ret;
#endif
}

/**
 *      rltk_wlan_recv - indicate packets to LWIP. Called by ethernetif_recv().
 *      @idx: netif index
 *      @sg_list: data buffer list
 *      @sg_len: size of each data buffer
 *
 *      Return Value: 0 on success, -1 if a DMA timeout made the pbuf unsafe.
 */     
int rltk_wlan_recv(int idx, struct eth_drv_sg *sg_list, int sg_len)
{
#if (CONFIG_LWIP_LAYER == 1)
	struct eth_drv_sg *last_sg;
	struct sk_buff *skb;
	
	DBG_TRACE("%s is called", __FUNCTION__);
	if(idx == -1){
		DBG_ERR("skb is NULL");
		return -1;
	}
	skb = rltk_wlan_get_recv_skb(idx);
	DBG_ASSERT(skb, "No pending rx skb");

	for (last_sg = &sg_list[sg_len]; sg_list < last_sg; ++sg_list) {
		if (sg_list->buf != 0) {
#if defined(CONFIG_PLATFORM_8195BHP)
			if (net_gdma_copy(&g_net_rx_gdma,
					  (void *)(sg_list->buf), skb->data,
					  sg_list->len, NULL) != 0) {
				printf("[NET_GDMA][ERROR] RX packet dropped len=%u\n",
				       (unsigned int)sg_list->len);
				return -1;
			}
#else
			rtw_memcpy((void *)(sg_list->buf), skb->data, sg_list->len);
#endif
			skb_pull(skb, sg_list->len);
		}
	}
	return 0;
#else
	(void)idx;
	(void)sg_list;
	(void)sg_len;
	return -1;
#endif
}

int netif_is_valid_IP(int idx, unsigned char *ip_dest)
{
#if defined(CONFIG_MBED_ENABLED)
	return 1;
#else
#if CONFIG_LWIP_LAYER == 1
	struct netif * pnetif = &xnetif[idx];

	ip_addr_t addr = { 0 };

#ifdef CONFIG_MEMORY_ACCESS_ALIGNED
	unsigned int temp;
	memcpy(&temp, ip_dest, sizeof(unsigned int));
	u32_t *ip_dest_addr = &temp;
#else
	u32_t *ip_dest_addr  = (u32_t*)ip_dest;
#endif

#if LWIP_VERSION_MAJOR >= 2
	ip_addr_set_ip4_u32(&addr, *ip_dest_addr);
#else
	addr.addr = *ip_dest_addr;
#endif

#if (LWIP_VERSION_MAJOR >= 2)
	if((ip_addr_get_ip4_u32(netif_ip_addr4(pnetif))) == 0)
		return 1;
#else

	if(pnetif->ip_addr.addr == 0)
		return 1;
#endif

	if(ip_addr_ismulticast(&addr) || ip_addr_isbroadcast(&addr,pnetif)){
		return 1;
	}

	//if(ip_addr_netcmp(&(pnetif->ip_addr), &addr, &(pnetif->netmask))) //addr&netmask
	//	return 1;

	if(ip_addr_cmp(&(pnetif->ip_addr),&addr))
		return 1;

	DBG_TRACE("invalid IP: %d.%d.%d.%d ",ip_dest[0],ip_dest[1],ip_dest[2],ip_dest[3]);
#endif	
#ifdef CONFIG_DONT_CARE_TP
	if(pnetif->flags & NETIF_FLAG_IPSWITCH)
		return 1;
	else
#endif
	return 0;
#endif
}

#if !defined(CONFIG_MBED_ENABLED)
int netif_get_idx(struct netif* pnetif)
{
#if (CONFIG_LWIP_LAYER == 1)
	int idx = pnetif - xnetif;

	switch(idx) {
	case 0:
		return 0;
	case 1:
		return 1;
	default:
		return -1;
	}
#else	
	return -1;
#endif
}

unsigned char *netif_get_hwaddr(int idx_wlan)
{
#if (CONFIG_LWIP_LAYER == 1)
	return xnetif[idx_wlan].hwaddr;
#else
	return NULL;
#endif
}
#endif

#if defined(CONFIG_MBED_ENABLED)
emac_callback emac_callback_func = NULL;
void *emac_callback_data = NULL;
void set_callback_func(emac_callback p, void *data)
{
	emac_callback_func = p;
	emac_callback_data = data;
}
#endif

void netif_rx(int idx, unsigned int len)
{
#if (CONFIG_LWIP_LAYER == 1)
#if defined(CONFIG_MBED_ENABLED)
	emac_callback_func(emac_callback_data, NULL, len);
#else
	ethernetif_recv(&xnetif[idx], len);
#endif
#endif
#if (CONFIG_INIC_EN == 1)
        inic_netif_rx(idx, len);
#endif
}

void netif_post_sleep_processing(void)
{
#if (CONFIG_LWIP_LAYER == 1)
#if defined(CONFIG_MBED_ENABLED)
#else
	lwip_POST_SLEEP_PROCESSING();	//For FreeRTOS tickless to enable Lwip ARP timer when leaving IPS - Alex Fang
#endif
#endif
}

void netif_pre_sleep_processing(void)
{
#if (CONFIG_LWIP_LAYER == 1)
#if defined(CONFIG_MBED_ENABLED)
#else
	lwip_PRE_SLEEP_PROCESSING();
#endif
#endif
}

#ifdef CONFIG_WOWLAN
unsigned char *rltk_wlan_get_ip(int idx){
#if (CONFIG_LWIP_LAYER == 1)
	return LwIP_GetIP(&xnetif[idx]);
#else
	return NULL;
#endif
}
unsigned char *rltk_wlan_get_gw(int idx){
#if (CONFIG_LWIP_LAYER == 1)
	return LwIP_GetGW(&xnetif[idx]);
#else
	return NULL;
#endif
}

unsigned char *rltk_wlan_get_gwmask(int idx){
#if (CONFIG_LWIP_LAYER == 1)
	return LwIP_GetMASK(&xnetif[idx]);
#else
	return NULL;
#endif
}
#endif
