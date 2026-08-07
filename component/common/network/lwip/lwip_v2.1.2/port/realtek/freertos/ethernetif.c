/**
 * @file
 * Ethernet Interface Skeleton
 *
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

/*
 * This file is a skeleton for developing Ethernet network interface
 * drivers for lwIP. Add code to the low_level functions and do a
 * search-and-replace for the word "ethernetif" to replace it with
 * something that better describes your network interface.
 */

#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/tcpip.h"
#include "lwip/icmp.h"
#include "netif/etharp.h"
#include "lwip/err.h"
#include "ethernetif.h"
#include "lwip_netconf.h"

#include "lwip/ethip6.h" //Add for ipv6

#include "wifi_conf.h"
#include "platform_stdlib.h"
#include "basic_types.h"

#if defined(CONFIG_USBH_CDC_NCM)
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#endif

#if CONFIG_WLAN
#include <lwip_intf.h>
#endif
#if defined(CONFIG_PLATFORM_8195BHP)
#include "cmsis.h"
#include "hal_timer.h"
#endif

#define netifMTU                                (1500)
#define netifINTERFACE_TASK_STACK_SIZE        ( 350 )
#define netifINTERFACE_TASK_PRIORITY        ( configMAX_PRIORITIES - 1 )
#define netifGUARD_BLOCK_TIME            ( 250 )
/* The time to block waiting for input. */
#define emacBLOCK_TIME_WAITING_FOR_INPUT    ( ( portTickType ) 100 )

#define IF2NAME0 'e'
#define IF2NAME1 'n'


struct netif eth_netif;

#if CONFIG_WLAN && CONFIG_WLAN_RX_ZERO_COPY && LWIP_SUPPORT_CUSTOM_PBUF

#define WLAN_RX_ZC_MAGIC       0x5A435250UL
#define WLAN_RX_ZC_REPORT_MS   5000U
#define WLAN_RX_ZC_REPORT_PROBE 256U
#define WLAN_RX_ZC_WORD_BITS   32U
#define WLAN_RX_ZC_WORDS       \
	((WLAN_RX_ZERO_COPY_POOL_SIZE + WLAN_RX_ZC_WORD_BITS - 1U) / \
	 WLAN_RX_ZC_WORD_BITS)

struct wlan_rx_custom_pbuf {
	struct pbuf_custom custom;
	void *rx_ref;
	u32_t magic;
};

struct wlan_rx_zc_stats {
	u32_t ops;
	u32_t bytes;
	u32_t fallback_small;
	u32_t fallback_pool;
	u32_t fallback_clone;
	u32_t fallback_invalid;
	u32_t outstanding;
	u32_t peak;
	u32_t report_probe;
	u32_t last_report_ms;
};

static struct wlan_rx_zc_stats g_wlan_rx_zc_stats;
#if WLAN_RX_ZERO_COPY_STATS
#define WLAN_RX_ZC_STAT_INC(field) (g_wlan_rx_zc_stats.field++)
#define WLAN_RX_ZC_STAT_ADD(field, value) (g_wlan_rx_zc_stats.field += (value))
#else
#define WLAN_RX_ZC_STAT_INC(field) do { } while (0)
#define WLAN_RX_ZC_STAT_ADD(field, value) do { (void)(value); } while (0)
#endif
static u8_t g_wlan_rx_zc_pool_initialized;
static u8_t g_wlan_rx_zc_selftest_state;
static u8_t g_wlan_rx_zc_quiesced;
static u32_t g_wlan_rx_zc_used[WLAN_RX_ZC_WORDS];
/* Allocated once from the DRAM-backed system heap; do not spend scarce ITCM. */
static struct wlan_rx_custom_pbuf *g_wlan_rx_zc_pool;

static void wlan_rx_zc_wrapper_free(struct wlan_rx_custom_pbuf *wrapper);

static void wlan_rx_zc_pool_init(void)
{
	if (g_wlan_rx_zc_pool_initialized) {
		return;
	}

	/* ethernetif_init() is serialized during network bring-up. */
	g_wlan_rx_zc_pool_initialized = 1;
	g_wlan_rx_zc_pool = (struct wlan_rx_custom_pbuf *)
		rtw_zmalloc(sizeof(*g_wlan_rx_zc_pool) *
			    WLAN_RX_ZERO_COPY_POOL_SIZE);
#if WLAN_RX_ZERO_COPY_STATS
	g_wlan_rx_zc_stats.last_report_ms = sys_now();
#endif
	if (g_wlan_rx_zc_pool == NULL) {
		printf("[WLAN_RX_ZC] wrapper pool allocation failed; copy fallback only\n");
	} else {
		printf("[WLAN_RX_ZC] enabled min_len=%u pool=%u\n",
		       (unsigned int)WLAN_RX_ZERO_COPY_MIN_LEN,
		       (unsigned int)WLAN_RX_ZERO_COPY_POOL_SIZE);
	}
}

static void wlan_rx_zc_free(struct pbuf *p)
{
	struct wlan_rx_custom_pbuf *wrapper =
		(struct wlan_rx_custom_pbuf *)p;
	void *rx_ref;

	if (wrapper->magic != WLAN_RX_ZC_MAGIC || wrapper->rx_ref == NULL) {
		printf("[WLAN_RX_ZC][ERROR] invalid custom pbuf release\n");
		return;
	}

	rx_ref = wrapper->rx_ref;
	wrapper->rx_ref = NULL;
	wrapper->magic = 0;
	rltk_wlan_rx_ref_release(rx_ref);
	wlan_rx_zc_wrapper_free(wrapper);
}

static struct wlan_rx_custom_pbuf *wlan_rx_zc_wrapper_alloc(void)
{
	struct wlan_rx_custom_pbuf *wrapper = NULL;
	u32_t word;
	u32_t bit;
	u32_t slot;
	SYS_ARCH_DECL_PROTECT(old_level);

	SYS_ARCH_PROTECT(old_level);
	if (g_wlan_rx_zc_pool == NULL || g_wlan_rx_zc_quiesced) {
		SYS_ARCH_UNPROTECT(old_level);
		return NULL;
	}
	for (word = 0; word < WLAN_RX_ZC_WORDS && wrapper == NULL; word++) {
		if (g_wlan_rx_zc_used[word] == 0xFFFFFFFFUL) {
			continue;
		}
		for (bit = 0; bit < WLAN_RX_ZC_WORD_BITS; bit++) {
			u32_t mask = 1UL << bit;

			slot = word * WLAN_RX_ZC_WORD_BITS + bit;
			if (slot >= WLAN_RX_ZERO_COPY_POOL_SIZE) {
				break;
			}
			if ((g_wlan_rx_zc_used[word] & mask) == 0U) {
				g_wlan_rx_zc_used[word] |= mask;
				wrapper = &g_wlan_rx_zc_pool[slot];
				break;
			}
		}
	}
	if (wrapper != NULL) {
		g_wlan_rx_zc_stats.outstanding++;
#if WLAN_RX_ZERO_COPY_STATS
		if (g_wlan_rx_zc_stats.outstanding > g_wlan_rx_zc_stats.peak) {
			g_wlan_rx_zc_stats.peak = g_wlan_rx_zc_stats.outstanding;
		}
#endif
	}
	SYS_ARCH_UNPROTECT(old_level);
	return wrapper;
}

static void wlan_rx_zc_wrapper_free(struct wlan_rx_custom_pbuf *wrapper)
{
	unsigned int slot = (unsigned int)(wrapper - g_wlan_rx_zc_pool);
	u32_t word = slot / WLAN_RX_ZC_WORD_BITS;
	u32_t mask = 1UL << (slot % WLAN_RX_ZC_WORD_BITS);
	SYS_ARCH_DECL_PROTECT(old_level);

	SYS_ARCH_PROTECT(old_level);
	if (slot >= WLAN_RX_ZERO_COPY_POOL_SIZE ||
	    (g_wlan_rx_zc_used[word] & mask) == 0U) {
		SYS_ARCH_UNPROTECT(old_level);
		printf("[WLAN_RX_ZC][ERROR] invalid wrapper release\n");
		return;
	}
	g_wlan_rx_zc_used[word] &= ~mask;
	if (g_wlan_rx_zc_stats.outstanding > 0U) {
		g_wlan_rx_zc_stats.outstanding--;
	}
	SYS_ARCH_UNPROTECT(old_level);
}

static void wlan_rx_zc_report(void)
{
#if WLAN_RX_ZERO_COPY_STATS
	u32_t now;
	u32_t elapsed;

	/* Avoid a timer read for every RX packet on this hot path. */
	if (++g_wlan_rx_zc_stats.report_probe < WLAN_RX_ZC_REPORT_PROBE) {
		return;
	}
	g_wlan_rx_zc_stats.report_probe = 0;
	now = sys_now();
	elapsed = now - g_wlan_rx_zc_stats.last_report_ms;

	if (elapsed >= WLAN_RX_ZC_REPORT_MS) {
		printf("[WLAN_RX_ZC] window_ms=%u zero_copy=%u/%uB "
		       "outstanding=%u peak=%u fallback small=%u pool=%u "
		       "clone=%u invalid=%u\n",
		       (unsigned int)elapsed,
		       (unsigned int)g_wlan_rx_zc_stats.ops,
		       (unsigned int)g_wlan_rx_zc_stats.bytes,
		       (unsigned int)g_wlan_rx_zc_stats.outstanding,
		       (unsigned int)g_wlan_rx_zc_stats.peak,
		       (unsigned int)g_wlan_rx_zc_stats.fallback_small,
		       (unsigned int)g_wlan_rx_zc_stats.fallback_pool,
		       (unsigned int)g_wlan_rx_zc_stats.fallback_clone,
		       (unsigned int)g_wlan_rx_zc_stats.fallback_invalid);
		g_wlan_rx_zc_stats.ops = 0;
		g_wlan_rx_zc_stats.bytes = 0;
		g_wlan_rx_zc_stats.fallback_small = 0;
		g_wlan_rx_zc_stats.fallback_pool = 0;
		g_wlan_rx_zc_stats.fallback_clone = 0;
		g_wlan_rx_zc_stats.fallback_invalid = 0;
		g_wlan_rx_zc_stats.peak = g_wlan_rx_zc_stats.outstanding;
		g_wlan_rx_zc_stats.last_report_ms = now;
	}
#endif
}

static struct pbuf *wlan_rx_zc_try(int idx, int total_len)
{
	struct wlan_rx_custom_pbuf *wrapper;
	struct pbuf *p;
	void *rx_ref;
	void *payload;
	int ret;
#if WLAN_RX_ZERO_COPY_SELFTEST
	u8_t selftest_state;
	int run_selftest = 0;
	SYS_ARCH_DECL_PROTECT(selftest_level);
#endif

	if ((unsigned int)total_len < WLAN_RX_ZERO_COPY_MIN_LEN) {
		WLAN_RX_ZC_STAT_INC(fallback_small);
		return NULL;
	}

#if WLAN_RX_ZERO_COPY_SELFTEST
	SYS_ARCH_PROTECT(selftest_level);
	selftest_state = g_wlan_rx_zc_selftest_state;
	SYS_ARCH_UNPROTECT(selftest_level);
	if (selftest_state == 2U) {
		WLAN_RX_ZC_STAT_INC(fallback_invalid);
		return NULL;
	}
#endif

	/*
	 * Reserve/count before the clone self-test as well as real acquisition.
	 * This makes wifi_off() wait for every skb-pool user that raced with its
	 * quiesce gate, including the very first diagnostic packet.
	 */
	wrapper = wlan_rx_zc_wrapper_alloc();
	if (wrapper == NULL) {
		WLAN_RX_ZC_STAT_INC(fallback_pool);
		return NULL;
	}

#if WLAN_RX_ZERO_COPY_SELFTEST
	/* Only one WLAN interface may operate on the shared skb-pool test. */
	SYS_ARCH_PROTECT(selftest_level);
	if (g_wlan_rx_zc_selftest_state == 0U) {
		g_wlan_rx_zc_selftest_state = 3U; /* test in progress */
		run_selftest = 1;
	}
	selftest_state = g_wlan_rx_zc_selftest_state;
	SYS_ARCH_UNPROTECT(selftest_level);

	if (run_selftest) {
		ret = rltk_wlan_rx_ref_selftest();
		SYS_ARCH_PROTECT(selftest_level);
		if (ret == 0) {
			g_wlan_rx_zc_selftest_state = 1U;
		} else if (ret == -1) {
			g_wlan_rx_zc_selftest_state = 2U;
		} else {
			g_wlan_rx_zc_selftest_state = 0U; /* transient; retry later */
		}
		SYS_ARCH_UNPROTECT(selftest_level);

		if (ret == 0) {
			printf("[WLAN_RX_ZC][SELFTEST] clone lifetime PASS\n");
		} else if (ret == -1) {
			printf("[WLAN_RX_ZC][SELFTEST] clone lifetime FAIL; "
			       "copy fallback only\n");
			WLAN_RX_ZC_STAT_INC(fallback_invalid);
			wlan_rx_zc_wrapper_free(wrapper);
			return NULL;
		} else {
			WLAN_RX_ZC_STAT_INC(fallback_clone);
			wlan_rx_zc_wrapper_free(wrapper);
			return NULL;
		}
	} else if (selftest_state == 3U) {
		/* Another interface is testing; copy this packet without waiting. */
		WLAN_RX_ZC_STAT_INC(fallback_clone);
		wlan_rx_zc_wrapper_free(wrapper);
		return NULL;
	}
	if (selftest_state == 2U) {
		WLAN_RX_ZC_STAT_INC(fallback_invalid);
		wlan_rx_zc_wrapper_free(wrapper);
		return NULL;
	}
#endif

	ret = rltk_wlan_rx_ref_acquire(idx, (unsigned int)total_len,
				       &rx_ref, &payload);
	if (ret != 0) {
		if (ret == -2) {
			WLAN_RX_ZC_STAT_INC(fallback_clone);
		} else {
			WLAN_RX_ZC_STAT_INC(fallback_invalid);
		}
		wlan_rx_zc_wrapper_free(wrapper);
		return NULL;
	}

	wrapper->custom.custom_free_function = wlan_rx_zc_free;
	wrapper->rx_ref = rx_ref;
	wrapper->magic = WLAN_RX_ZC_MAGIC;
	p = pbuf_alloced_custom(PBUF_RAW, (u16_t)total_len, PBUF_REF,
				&wrapper->custom, payload, (u16_t)total_len);
	if (p == NULL) {
		rltk_wlan_rx_ref_release(rx_ref);
		wrapper->rx_ref = NULL;
		wrapper->magic = 0;
		WLAN_RX_ZC_STAT_INC(fallback_invalid);
		wlan_rx_zc_wrapper_free(wrapper);
		return NULL;
	}

	WLAN_RX_ZC_STAT_INC(ops);
	WLAN_RX_ZC_STAT_ADD(bytes, (u32_t)total_len);
	return p;
}

#endif /* CONFIG_WLAN && CONFIG_WLAN_RX_ZERO_COPY && LWIP_SUPPORT_CUSTOM_PBUF */

int ethernetif_wlan_rx_zc_quiesce(unsigned int timeout_ms)
{
#if CONFIG_WLAN && CONFIG_WLAN_RX_ZERO_COPY && LWIP_SUPPORT_CUSTOM_PBUF
	u32_t start_ms = sys_now();
	u32_t outstanding;
	SYS_ARCH_DECL_PROTECT(old_level);

	/*
	 * Serialize the route gate with wrapper allocation.  Once this flag is
	 * visible, new large frames use the normal copy path, while existing TCP
	 * consumers can continue releasing their retained skb clones.  The wait
	 * sleeps, so Wi-Fi shutdown does not burn CPU while TCP drains.
	 */
	SYS_ARCH_PROTECT(old_level);
	g_wlan_rx_zc_quiesced = 1U;
	outstanding = g_wlan_rx_zc_stats.outstanding;
	SYS_ARCH_UNPROTECT(old_level);

	while (outstanding != 0U) {
		if ((u32_t)(sys_now() - start_ms) >= (u32_t)timeout_ms) {
			SYS_ARCH_PROTECT(old_level);
			outstanding = g_wlan_rx_zc_stats.outstanding;
			if (outstanding == 0U) {
				SYS_ARCH_UNPROTECT(old_level);
				break;
			}
			g_wlan_rx_zc_quiesced = 0U;
			SYS_ARCH_UNPROTECT(old_level);
			printf("[WLAN_RX_ZC][ERROR] drain timeout outstanding=%u; "
			       "Wi-Fi shutdown aborted\n",
			       (unsigned int)outstanding);
			return -1;
		}

		rtw_msleep_os(10U);
		SYS_ARCH_PROTECT(old_level);
		outstanding = g_wlan_rx_zc_stats.outstanding;
		SYS_ARCH_UNPROTECT(old_level);
	}

	printf("[WLAN_RX_ZC] quiesced; outstanding=0\n");
#else
	(void)timeout_ms;
#endif
	return 0;
}

void ethernetif_wlan_rx_zc_resume(void)
{
#if CONFIG_WLAN && CONFIG_WLAN_RX_ZERO_COPY && LWIP_SUPPORT_CUSTOM_PBUF
	u8_t was_quiesced;
	SYS_ARCH_DECL_PROTECT(old_level);

	SYS_ARCH_PROTECT(old_level);
	was_quiesced = g_wlan_rx_zc_quiesced;
	g_wlan_rx_zc_quiesced = 0U;
	SYS_ARCH_UNPROTECT(old_level);
	if (was_quiesced) {
		printf("[WLAN_RX_ZC] resumed\n");
	}
#endif
}


static void arp_timer(void *arg);

static const char *TAG = "ETHERNET";
#define ETHERNET_DEBUG		(0)
#define RTK_LOG_ETHERNET(format, ...) do {               \
        if ( ETHERNET_DEBUG ) printf(format, ##__VA_ARGS__); \
    } while(0);

#if defined(CONFIG_ETHERNET) && CONFIG_ETHERNET
#define MAX_BUFFER_SIZE		(1536)
#define DST_MAC_LEN			(6)
#define SRC_MAC_LEN			(6)
#define PROTO_TYPE_LEN		(2)  // protocol type
#define IP_LEN_OFFSET		(2)  // offset of total length field in IP packet
#define ETHERNET_REASSEMBLE_PACKET	(0)


static u8 TX_BUFFER[MAX_BUFFER_SIZE] __attribute__((aligned(32)));
static u8 RX_BUFFER[MAX_BUFFER_SIZE];

#ifndef CONFIG_NCM_TX_PROFILE
#define CONFIG_NCM_TX_PROFILE 0
#endif

#ifndef CONFIG_NCM_TX_ASYNC
#define CONFIG_NCM_TX_ASYNC 0
#endif

#ifndef CONFIG_NCM_TX_ASYNC_PROFILE
#define CONFIG_NCM_TX_ASYNC_PROFILE 0
#endif

#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_NCM_TX_PROFILE
#define NCM_TX_PROFILE_WINDOW_US 10000000U
#define NCM_TX_PROFILE_SIZE_BINS 4U

struct ncm_tx_profile_stats {
	u32_t window_start_us;
	u32_t calls;
	u32_t success;
	u32_t errors;
	u32_t bytes;
	u32_t single_pbuf;
	u32_t chained_pbuf;
	u32_t pbuf_segments;
	u32_t pbuf_segments_max;
	u32_t send_ge_1ms;
	u32_t send_ge_5ms;
	u32_t send_ge_10ms;
	u32_t send_ge_20ms;
	u32_t size_calls[NCM_TX_PROFILE_SIZE_BINS];
	u32_t size_bytes[NCM_TX_PROFILE_SIZE_BINS];
	uint64_t size_send_cycles[NCM_TX_PROFILE_SIZE_BINS];
	u32_t size_send_max_cycles[NCM_TX_PROFILE_SIZE_BINS];
	uint64_t total_cycles;
	uint64_t flatten_cycles;
	uint64_t send_cycles;
	uint64_t other_cycles;
	u32_t total_max_cycles;
	u32_t flatten_max_cycles;
	u32_t send_max_cycles;
};

static struct ncm_tx_profile_stats g_ncm_tx_profile;

static u32_t ncm_tx_profile_cycles_to_us(uint64_t cycles)
{
	u32_t cycles_per_us = SystemCoreClock / 1000000U;
	return cycles_per_us ? (u32_t)(cycles / cycles_per_us) : 0U;
}

static u32_t ncm_tx_profile_size_bin(u32_t bytes)
{
	if (bytes <= 128U) {
		return 0U;
	}
	if (bytes <= 512U) {
		return 1U;
	}
	if (bytes <= 1024U) {
		return 2U;
	}
	return 3U;
}

static void ncm_tx_profile_report(u32_t now)
{
	u32_t window;
	u32_t calls;
	u32_t segments;
	u32_t bin;

	if (g_ncm_tx_profile.window_start_us == 0U) {
		g_ncm_tx_profile.window_start_us = now;
		return;
	}
	window = now - g_ncm_tx_profile.window_start_us;
	if (window < NCM_TX_PROFILE_WINDOW_US) {
		return;
	}
	calls = g_ncm_tx_profile.calls;
	segments = g_ncm_tx_profile.pbuf_segments;
	printf("[NCMTXPROF] window_us=%u calls ok/error=%u/%u bytes=%u pbuf single/chained=%u/%u "
	       "segments avg/max_x100=%u/%u\n",
	       (unsigned int)window,
	       (unsigned int)g_ncm_tx_profile.success,
	       (unsigned int)g_ncm_tx_profile.errors,
	       (unsigned int)g_ncm_tx_profile.bytes,
	       (unsigned int)g_ncm_tx_profile.single_pbuf,
	       (unsigned int)g_ncm_tx_profile.chained_pbuf,
	       calls ? (unsigned int)((segments * 100U) / calls) : 0U,
	       (unsigned int)(g_ncm_tx_profile.pbuf_segments_max * 100U));
	printf("[NCMTXPROF] phase_us total/flatten/send/other=%u/%u/%u/%u avg=%u/%u/%u/%u "
	       "max total/flatten/send=%u/%u/%u send_ge_ms 1/5/10/20=%u/%u/%u/%u\n",
	       (unsigned int)ncm_tx_profile_cycles_to_us(g_ncm_tx_profile.total_cycles),
	       (unsigned int)ncm_tx_profile_cycles_to_us(g_ncm_tx_profile.flatten_cycles),
	       (unsigned int)ncm_tx_profile_cycles_to_us(g_ncm_tx_profile.send_cycles),
	       (unsigned int)ncm_tx_profile_cycles_to_us(g_ncm_tx_profile.other_cycles),
	       calls ? (unsigned int)ncm_tx_profile_cycles_to_us(g_ncm_tx_profile.total_cycles / calls) : 0U,
	       calls ? (unsigned int)ncm_tx_profile_cycles_to_us(g_ncm_tx_profile.flatten_cycles / calls) : 0U,
	       calls ? (unsigned int)ncm_tx_profile_cycles_to_us(g_ncm_tx_profile.send_cycles / calls) : 0U,
	       calls ? (unsigned int)ncm_tx_profile_cycles_to_us(g_ncm_tx_profile.other_cycles / calls) : 0U,
	       (unsigned int)ncm_tx_profile_cycles_to_us(g_ncm_tx_profile.total_max_cycles),
	       (unsigned int)ncm_tx_profile_cycles_to_us(g_ncm_tx_profile.flatten_max_cycles),
	       (unsigned int)ncm_tx_profile_cycles_to_us(g_ncm_tx_profile.send_max_cycles),
	       (unsigned int)g_ncm_tx_profile.send_ge_1ms,
	       (unsigned int)g_ncm_tx_profile.send_ge_5ms,
	       (unsigned int)g_ncm_tx_profile.send_ge_10ms,
	       (unsigned int)g_ncm_tx_profile.send_ge_20ms);
	printf("[NCMTXPROF] size_bins <=128/<=512/<=1024/>1024 calls/bytes/send_avg_us/send_max_us=");
	for (bin = 0U; bin < NCM_TX_PROFILE_SIZE_BINS; bin++) {
		printf("%s%u:%u:%u:%u", bin ? "/" : "",
		       (unsigned int)g_ncm_tx_profile.size_calls[bin],
		       (unsigned int)g_ncm_tx_profile.size_bytes[bin],
		       g_ncm_tx_profile.size_calls[bin] ?
		       (unsigned int)ncm_tx_profile_cycles_to_us(
			       g_ncm_tx_profile.size_send_cycles[bin] /
			       g_ncm_tx_profile.size_calls[bin]) : 0U,
		       (unsigned int)ncm_tx_profile_cycles_to_us(
			       g_ncm_tx_profile.size_send_max_cycles[bin]));
	}
	printf("\n");
	memset(&g_ncm_tx_profile, 0, sizeof(g_ncm_tx_profile));
	g_ncm_tx_profile.window_start_us = now;
}

static void ncm_tx_profile_commit(u32_t bytes, u32_t segments, int result,
				  u32_t total_cycles, u32_t flatten_cycles,
				  u32_t send_cycles)
{
	u32_t bin = ncm_tx_profile_size_bin(bytes);
	u32_t send_us = ncm_tx_profile_cycles_to_us(send_cycles);
	u32_t accounted = flatten_cycles + send_cycles;
	u32_t other_cycles = total_cycles - accounted;
	u32_t now = hal_read_curtime_us();

	g_ncm_tx_profile.calls++;
	if (result == 0) {
		g_ncm_tx_profile.success++;
	} else {
		g_ncm_tx_profile.errors++;
	}
	g_ncm_tx_profile.bytes += bytes;
	if (segments <= 1U) {
		g_ncm_tx_profile.single_pbuf++;
	} else {
		g_ncm_tx_profile.chained_pbuf++;
	}
	g_ncm_tx_profile.pbuf_segments += segments;
	if (segments > g_ncm_tx_profile.pbuf_segments_max) {
		g_ncm_tx_profile.pbuf_segments_max = segments;
	}
	if (send_us >= 1000U) g_ncm_tx_profile.send_ge_1ms++;
	if (send_us >= 5000U) g_ncm_tx_profile.send_ge_5ms++;
	if (send_us >= 10000U) g_ncm_tx_profile.send_ge_10ms++;
	if (send_us >= 20000U) g_ncm_tx_profile.send_ge_20ms++;
	g_ncm_tx_profile.size_calls[bin]++;
	g_ncm_tx_profile.size_bytes[bin] += bytes;
	g_ncm_tx_profile.size_send_cycles[bin] += send_cycles;
	if (send_cycles > g_ncm_tx_profile.size_send_max_cycles[bin]) {
		g_ncm_tx_profile.size_send_max_cycles[bin] = send_cycles;
	}
	g_ncm_tx_profile.total_cycles += total_cycles;
	g_ncm_tx_profile.flatten_cycles += flatten_cycles;
	g_ncm_tx_profile.send_cycles += send_cycles;
	g_ncm_tx_profile.other_cycles += other_cycles;
	if (total_cycles > g_ncm_tx_profile.total_max_cycles) {
		g_ncm_tx_profile.total_max_cycles = total_cycles;
	}
	if (flatten_cycles > g_ncm_tx_profile.flatten_max_cycles) {
		g_ncm_tx_profile.flatten_max_cycles = flatten_cycles;
	}
	if (send_cycles > g_ncm_tx_profile.send_max_cycles) {
		g_ncm_tx_profile.send_max_cycles = send_cycles;
	}
	ncm_tx_profile_report(now);
}

static void ncm_tx_profile_init(void)
{
	static u8_t ready;

	if (ready == 0U) {
		if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U) {
			CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
			DWT->CYCCNT = 0U;
			DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
		}
		ready = 1U;
	}
}
#endif /* CONFIG_PLATFORM_8195BHP && CONFIG_NCM_TX_PROFILE */

#if defined(CONFIG_USBH_CDC_ECM)
extern int usbh_cdc_ecm_send_data(u8 *buf, u32 len);
#elif defined(CONFIG_USBH_CDC_NCM)
extern int usbh_cdc_ncm_send_data(u8 *buf, u32 len);
#endif

#if defined(CONFIG_USBH_CDC_ECM)
extern u16 usbh_cdc_ecm_get_receive_mps(void);
#elif defined(CONFIG_USBH_CDC_NCM)
extern u16 usbh_cdc_ncm_get_receive_mps(void);
#endif

#endif

/**
 * In this function, the hardware should be initialized.
 * Called from ethernetif_init().
 *
 * @param netif the already initialized lwip network interface structure
 *        for this ethernetif
 */
static void low_level_init(struct netif *netif)
{
	(void) netif;

	/* set MAC hardware address length */
	netif->hwaddr_len = ETHARP_HWADDR_LEN;
	netif->mtu = 1500;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
#if LWIP_IGMP
	/* make LwIP_Init do igmp_start to add group 224.0.0.1 */
	netif->flags |= NETIF_FLAG_IGMP;
#endif

#if LWIP_IPV6
#if LWIP_IPV6_MLD
	netif->flags |= NETIF_FLAG_MLD6;
#endif
#endif


}

/**
 * This function should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @param p the MAC packet to send (e.g. IP packet including MAC addresses and type)
 * @return ERR_OK if the packet could be sent
 *         an err_t value if the packet couldn't be sent
 *
 * @note Returning ERR_MEM here if a DMA queue of your MAC is full can lead to
 *       strange results. You might consider waiting for space in the DMA queue
 *       to become availale since the stack doesn't retry to send a packet
 *       dropped because of memory failure (except for the TCP timers).
 */

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
	int ret;
	struct eth_drv_sg sg_list[MAX_ETH_DRV_SG];
	int sg_len = 0;
	struct pbuf *q;

	for (q = p; q != NULL && sg_len < MAX_ETH_DRV_SG; q = q->next) {
		sg_list[sg_len].buf = (unsigned int) q->payload;
		sg_list[sg_len++].len = q->len;
	}
	if (sg_len) {
		ret = rltk_wlan_send(netif_get_idx(netif), sg_list, sg_len, p->tot_len);
		if (ret) {
			return ERR_BUF;
		}
	}
	return ERR_OK;
}

/*for ethernet mii interface*/
static err_t ncm_send_pbuf_sync(struct pbuf *p)
{
#if (defined(CONFIG_ETHERNET) && CONFIG_ETHERNET)
	// printf("[NCM] tx len=%d\n", p->tot_len);
	struct pbuf *q;
	u8 *pdata = TX_BUFFER;
	u8 *tx_data = TX_BUFFER;
	u32 size = 0;
	int ret = 0;
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_NCM_TX_PROFILE
	u32_t profile_start_cycles;
	u32_t profile_flatten_start_cycles = 0U;
	u32_t profile_flatten_cycles = 0U;
	u32_t profile_send_start_cycles;
	u32_t profile_send_cycles;
	u32_t profile_segments = 1U;

	ncm_tx_profile_init();
	profile_start_cycles = DWT->CYCCNT;
#endif

	/* ncm_wrap_ntb() consumes the payload synchronously and does not modify it. */
	if (p->next == NULL) {
		tx_data = (u8 *)p->payload;
		size = p->len;
	} else {
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_NCM_TX_PROFILE
		profile_flatten_start_cycles = DWT->CYCCNT;
		profile_segments = 0U;
#endif
		for (q = p; q != NULL; q = q->next) {
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_NCM_TX_PROFILE
			profile_segments++;
#endif
			if (q->len > (MAX_BUFFER_SIZE - size)) {
				printf("%s chained pbuf too large: %u\n", __func__,
				       (unsigned int)p->tot_len);
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_NCM_TX_PROFILE
				profile_flatten_cycles = DWT->CYCCNT -
					profile_flatten_start_cycles;
				ncm_tx_profile_commit((u32_t)p->tot_len, profile_segments,
					-1, DWT->CYCCNT - profile_start_cycles,
					profile_flatten_cycles, 0U);
#endif
				return ERR_BUF;
			}
#if defined(CONFIG_PLATFORM_8195BHP)
			if (rltk_network_gdma_copy_tx(pdata, q->payload, q->len,
						 TX_BUFFER + MAX_BUFFER_SIZE) != 0) {
				printf("[NET_GDMA][ERROR] NCM TX packet dropped len=%u\n",
				       (unsigned int)p->tot_len);
				return ERR_BUF;
			}
#else
			memcpy((unsigned int *)pdata, (unsigned int *)q->payload,
			       q->len);
#endif
			pdata += q->len;
			size += q->len;
		}
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_NCM_TX_PROFILE
		profile_flatten_cycles = DWT->CYCCNT - profile_flatten_start_cycles;
#endif
	}

#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_NCM_TX_PROFILE
	profile_send_start_cycles = DWT->CYCCNT;
#endif
#if defined(CONFIG_USBH_CDC_ECM)
	ret = usbh_cdc_ecm_send_data(tx_data, size);
#elif defined(CONFIG_USBH_CDC_NCM)
	ret = usbh_cdc_ncm_send_data(tx_data, size);
#endif
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_NCM_TX_PROFILE
	profile_send_cycles = DWT->CYCCNT - profile_send_start_cycles;
	ncm_tx_profile_commit(size, profile_segments, ret,
		DWT->CYCCNT - profile_start_cycles, profile_flatten_cycles,
		profile_send_cycles);
#endif
	if (ret != 0) {
		printf("%s error = %d\n", __func__, ret);
		return ERR_BUF;
	}
#endif
	(void) p;
	return ERR_OK;
}

#if defined(CONFIG_PLATFORM_8195BHP) && defined(CONFIG_USBH_CDC_NCM) && \
	CONFIG_NCM_TX_ASYNC
#define NCM_TX_ASYNC_QUEUE_LEN       128U
#define NCM_TX_ASYNC_TASK_STACK      2048U
#define NCM_TX_ASYNC_REPORT_US       10000000U
#define NCM_TX_ASYNC_TASK_PRIORITY   TCPIP_THREAD_PRIO

struct ncm_tx_async_item {
	struct pbuf *p;
	u32_t enqueue_us;
};

struct ncm_tx_async_stats {
	u32_t window_start_us;
	u32_t enqueued;
	u32_t sent;
	u32_t send_errors;
	u32_t queue_full;
	u32_t queue_depth_max;
	u32_t queue_wait_max_us;
	uint64_t queue_wait_total_us;
};

static QueueHandle_t g_ncm_tx_async_queue;
static TaskHandle_t g_ncm_tx_async_task;
static struct ncm_tx_async_stats g_ncm_tx_async_stats;

static void ncm_tx_async_report(u32_t now)
{
#if CONFIG_NCM_TX_ASYNC_PROFILE
	struct ncm_tx_async_stats snapshot;
	u32_t window;
	u32_t sent;
	u32_t depth_now;

	taskENTER_CRITICAL();
	if (g_ncm_tx_async_stats.window_start_us == 0U) {
		g_ncm_tx_async_stats.window_start_us = now;
		taskEXIT_CRITICAL();
		return;
	}
	window = now - g_ncm_tx_async_stats.window_start_us;
	if (window < NCM_TX_ASYNC_REPORT_US) {
		taskEXIT_CRITICAL();
		return;
	}
	snapshot = g_ncm_tx_async_stats;
	memset(&g_ncm_tx_async_stats, 0, sizeof(g_ncm_tx_async_stats));
	g_ncm_tx_async_stats.window_start_us = now;
	taskEXIT_CRITICAL();
	depth_now = (u32_t)uxQueueMessagesWaiting(g_ncm_tx_async_queue);
	sent = snapshot.sent;
	printf("[NCMTXASYNC] window_us=%u mode=single-immediate enqueued/sent/error/full=%u/%u/%u/%u "
	       "depth_now/max=%u/%u wait_us avg/max=%u/%u queue_payload_copy=0\n",
	       (unsigned int)window,
	       (unsigned int)snapshot.enqueued,
	       (unsigned int)sent,
	       (unsigned int)snapshot.send_errors,
	       (unsigned int)snapshot.queue_full,
	       (unsigned int)depth_now,
	       (unsigned int)snapshot.queue_depth_max,
	       sent ? (unsigned int)(snapshot.queue_wait_total_us / sent) : 0U,
	       (unsigned int)snapshot.queue_wait_max_us);
#else
	(void)now;
#endif
}

static void ncm_tx_async_worker(void *arg)
{
	struct ncm_tx_async_item item;

	(void)arg;
	for (;;) {
		if (xQueueReceive(g_ncm_tx_async_queue, &item, portMAX_DELAY) == pdTRUE) {
			u32_t now = hal_read_curtime_us();
			u32_t wait_us = now - item.enqueue_us;
			err_t result = ncm_send_pbuf_sync(item.p);

#if CONFIG_NCM_TX_ASYNC_PROFILE
			taskENTER_CRITICAL();
			g_ncm_tx_async_stats.sent++;
			g_ncm_tx_async_stats.queue_wait_total_us += wait_us;
			if (wait_us > g_ncm_tx_async_stats.queue_wait_max_us) {
				g_ncm_tx_async_stats.queue_wait_max_us = wait_us;
			}
			if (result != ERR_OK) {
				g_ncm_tx_async_stats.send_errors++;
			}
			taskEXIT_CRITICAL();
#else
			(void)wait_us;
			(void)result;
#endif
			/* linkoutput() retained one reference before returning to lwIP. */
			pbuf_free(item.p);
			ncm_tx_async_report(hal_read_curtime_us());
		}
	}
}

static int ncm_tx_async_init(void)
{
	if (g_ncm_tx_async_task != NULL) {
		return 1;
	}
	if (g_ncm_tx_async_queue == NULL) {
		g_ncm_tx_async_queue = xQueueCreate(NCM_TX_ASYNC_QUEUE_LEN,
						 sizeof(struct ncm_tx_async_item));
		if (g_ncm_tx_async_queue == NULL) {
			printf("[NCMTXASYNC] ERROR: queue allocation failed; using synchronous NCM TX\n");
			return 0;
		}
	}
	if (xTaskCreate(ncm_tx_async_worker, "ncm_tx", NCM_TX_ASYNC_TASK_STACK,
			NULL, NCM_TX_ASYNC_TASK_PRIORITY, &g_ncm_tx_async_task) != pdPASS) {
		printf("[NCMTXASYNC] ERROR: worker creation failed; using synchronous NCM TX\n");
		vQueueDelete(g_ncm_tx_async_queue);
		g_ncm_tx_async_queue = NULL;
		g_ncm_tx_async_task = NULL;
		return 0;
	}
	printf("[NCMTXASYNC] enabled mode=single-immediate queue=%u priority=%u; "
	       "pbuf-reference only, no payload copy\n",
	       (unsigned int)NCM_TX_ASYNC_QUEUE_LEN,
	       (unsigned int)NCM_TX_ASYNC_TASK_PRIORITY);
	return 1;
}

static int ncm_tx_async_enqueue(struct pbuf *p)
{
	struct ncm_tx_async_item item;

	if (g_ncm_tx_async_task == NULL || g_ncm_tx_async_queue == NULL) {
		return 0;
	}
	item.p = p;
	item.enqueue_us = hal_read_curtime_us();
	/* A network driver may retain a pbuf after linkoutput() only if it owns an
	 * extra reference and releases that reference after TX completion. */
	pbuf_ref(p);
	if (xQueueSend(g_ncm_tx_async_queue, &item, 0) != pdPASS) {
		pbuf_free(p);
#if CONFIG_NCM_TX_ASYNC_PROFILE
		taskENTER_CRITICAL();
		g_ncm_tx_async_stats.queue_full++;
		taskEXIT_CRITICAL();
#endif
		printf("[NCMTXASYNC] ERROR: queue full len=%u; packet not accepted\n",
		       (unsigned int)p->tot_len);
		return -1;
	}
#if CONFIG_NCM_TX_ASYNC_PROFILE
	{
		u32_t depth;

		taskENTER_CRITICAL();
		g_ncm_tx_async_stats.enqueued++;
		depth = (u32_t)uxQueueMessagesWaiting(g_ncm_tx_async_queue);
		if (depth > g_ncm_tx_async_stats.queue_depth_max) {
			g_ncm_tx_async_stats.queue_depth_max = depth;
		}
		taskEXIT_CRITICAL();
	}
#endif
	return 1;
}
#endif

/*for ethernet mii interface*/
static err_t low_level_output_mii(struct netif *netif, struct pbuf *p)
{
	(void)netif;
#if defined(CONFIG_PLATFORM_8195BHP) && defined(CONFIG_USBH_CDC_NCM) && \
	CONFIG_NCM_TX_ASYNC
	{
		int queued = ncm_tx_async_enqueue(p);

		if (queued > 0) {
			return ERR_OK;
		}
		if (queued < 0) {
			return ERR_BUF;
		}
		/* Initialization failure is the only synchronous fallback.  Never run
		 * two callers through the closed NCM TX state machine concurrently. */
	}
#endif
	return ncm_send_pbuf_sync(p);
}


/**
 * Should allocate a pbuf and transfer the bytes of the incoming
 * packet from the interface into the pbuf.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return a pbuf filled with the received packet (including MAC header)
 *         NULL on memory error
 */

#if defined(ETHERNET_REASSEMBLE_PACKET) && ETHERNET_REASSEMBLE_PACKET
static u32 pkt_total_len = 0;
static u32 rx_buffer_saved_data_len = 0;
static u16 eth_type = 0;

u8 rltk_mii_recv_data(u8 *buf, u32 frame_length, u32 *total_len)
{
	RTK_LOG_ETHERNET("enter %s %d\n", __func__, __LINE__);

#if defined(CONFIG_ETHERNET) && CONFIG_ETHERNET
	u8 *pbuf;
	u32 pkt_len_index = DST_MAC_LEN + SRC_MAC_LEN + PROTO_TYPE_LEN;
#if defined(CONFIG_USBH_CDC_ECM)
	u16 usb_receive_mps = usbh_cdc_ecm_get_receive_mps();
#elif defined(CONFIG_USBH_CDC_NCM)
	u16 usb_receive_mps = usbh_cdc_ncm_get_receive_mps();
#else
	u16 usb_receive_mps = 512;
#endif

	if (0 == pkt_total_len) {
		pbuf = RX_BUFFER;
		if (frame_length > 0) {
			memcpy((void *)pbuf, buf, frame_length);
		}
		if ((0 == frame_length) || (frame_length % usb_receive_mps != 0)) {
			*total_len = frame_length;
			rx_buffer_saved_data_len = 0;
			pkt_total_len = 0;
			return 1;
		} else {
			rx_buffer_saved_data_len = frame_length;
			eth_type = buf[DST_MAC_LEN + SRC_MAC_LEN] * 256 + buf[DST_MAC_LEN + SRC_MAC_LEN + 1];

			if (eth_type == ETH_P_IP) {
				pkt_total_len =  buf[pkt_len_index + IP_LEN_OFFSET] * 256 + buf[pkt_len_index + IP_LEN_OFFSET + 1];
			}
		}
	} else {
		if (rx_buffer_saved_data_len + frame_length > MAX_BUFFER_SIZE) {
			printf("frame_length(%d) and rx_buffer_saved_data_len(%d) is too long\n", frame_length, rx_buffer_saved_data_len);
			rx_buffer_saved_data_len = 0;
		}

		pbuf = RX_BUFFER + rx_buffer_saved_data_len;
		if (frame_length > 0) {
			memcpy((void *)pbuf, buf, frame_length);
		}
		rx_buffer_saved_data_len += frame_length;
		if ((0 == frame_length) || (frame_length % usb_receive_mps != 0)) {
			*total_len = rx_buffer_saved_data_len;
			rx_buffer_saved_data_len = 0;
			pkt_total_len = 0;
			return 1;
		}
	}
#endif
	return 0;
}
#endif

u8 rltk_mii_recv_data_check(u8 *mac)
{
	(void) mac;
	u8 check_res = 1;
	return check_res;
#if defined(CONFIG_ETHERNET) && CONFIG_ETHERNET
	{
		u8 *pbuf = RX_BUFFER;
		u8 multi_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

		if (memcmp(mac, pbuf, ETH_ALEN) == 0 || memcmp(multi_mac, pbuf, ETH_ALEN) == 0) {
			check_res = 1;
		} else {
			check_res = 0;
		}
	}
	return check_res;
#endif
}
void rltk_mii_recv(struct eth_drv_sg *sg_list, int sg_len){
	struct eth_drv_sg *last_sg;
	u8* pbuf = RX_BUFFER;

	for (last_sg = &sg_list[sg_len]; sg_list < last_sg; ++sg_list) {
		if (sg_list->buf != 0) {
			rtw_memcpy((void *)(sg_list->buf), pbuf, sg_list->len);
			pbuf+=sg_list->len;
		}			 
	}
}
void ethernetif_mii_recv(u8 *buf, u32 frame_len)
{
#if (defined(CONFIG_ETHERNET) && CONFIG_ETHERNET)
	// printf("[NCM] ethernetif_mii_recv len=%d\n", frame_len);
	struct pbuf *p, *q;
	u8 *frame_data = buf;
	u32 total_len = 0;

	struct netif *netif = &eth_netif;
	u8 *macstr = (u8 *)(netif->hwaddr);

	if (frame_len > MAX_BUFFER_SIZE) {
		printf("recv data len is %d\n", frame_len);
		return;
	}

#if defined(ETHERNET_REASSEMBLE_PACKET) && ETHERNET_REASSEMBLE_PACKET
	RTK_LOG_ETHERNET("%s %d will rltk_mii_recv_data\n", __func__, __LINE__);
	if (0 == rltk_mii_recv_data(buf, frame_len, &total_len)) {
		return;
	}
	frame_data = RX_BUFFER;
#else
	if (0 == frame_len) {
		printf("recv data len is 0\n");
		return;
	}
	total_len = frame_len;
#endif
	if (0 == rltk_mii_recv_data_check(macstr)) {
		RTK_LOG_ETHERNET("rltk_mii_recv_data_check fail\n");
		return;
	}

	RTK_LOG_ETHERNET("%s %d rltk_mii_recv_data_check ok\n", __func__, __LINE__);

	p = pbuf_alloc(PBUF_RAW, total_len, PBUF_POOL);
	if (p == NULL) {
		printf("\n\r[%s]Cannot allocate pbuf to receive packet(%d)\n", __func__, total_len);
		return;
	}

	for (q = p; q != NULL; q = q->next) {
#if defined(CONFIG_PLATFORM_8195BHP)
		if (rltk_network_gdma_copy_rx(q->payload, frame_data, q->len,
					      NULL) != 0) {
			printf("[NET_GDMA][ERROR] NCM RX packet dropped len=%u\n",
			       (unsigned int)total_len);
			pbuf_free(p);
			return;
		}
#else
		memcpy(q->payload, frame_data, q->len);
#endif
		frame_data += q->len;
	}

	if (ERR_OK != netif->input(p, netif)) {
		pbuf_free(p);
	}
#endif
	(void) buf;
	(void) frame_len;
}

/**
 * Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 * This function should be passed as a parameter to netif_add().
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if the loopif is initialized
 *         ERR_MEM if private data couldn't be allocated
 *         any other err_t on error
 */
err_t ethernetif_init(struct netif *netif)
{
	LWIP_ASSERT("netif != NULL", (netif != NULL));

#if CONFIG_WLAN && CONFIG_WLAN_RX_ZERO_COPY && LWIP_SUPPORT_CUSTOM_PBUF
	wlan_rx_zc_pool_init();
#endif

#if LWIP_NETIF_HOSTNAME
	if (netif->name[1] == '0') {
		netif->hostname = "lwip0";
	} else if (netif->name[1] == '1') {
		netif->hostname = "lwip1";
	}
#endif /* LWIP_NETIF_HOSTNAME */

	netif->output = etharp_output;
#if LWIP_IPV6
	netif->output_ip6 = ethip6_output;
	netif->ip6_autoconfig_enabled = 1;
#endif
	netif->linkoutput = low_level_output;

	/* initialize the hardware */
	low_level_init(netif);

	etharp_init();

	return ERR_OK;
}

void rltk_mii_init(void)
{

}

err_t ethernetif_mii_init(struct netif *netif)
{
	LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
	netif->hostname = "lwip2";
#endif /* LWIP_NETIF_HOSTNAME */

	netif->output = etharp_output;
#if LWIP_IPV6
	netif->output_ip6 = ethip6_output;
#endif
	netif->linkoutput = low_level_output_mii;

	/* initialize the hardware */
	low_level_init(netif);

#if defined(CONFIG_PLATFORM_8195BHP) && defined(CONFIG_USBH_CDC_NCM) && \
	CONFIG_NCM_TX_ASYNC
	/* Start the owner of the closed, synchronous NCM TX API before traffic can
	 * reach linkoutput().  TCP_IP only retains and queues pbufs after this. */
	(void)ncm_tx_async_init();
#endif

	etharp_init();

	return ERR_OK;
}

static u32_t arp_next_time = 0;

static void arp_timer(void *arg)
{
	(void) arg;
	etharp_tmr();
	arp_next_time = sys_now() + ARP_TMR_INTERVAL;
	sys_timeout(ARP_TMR_INTERVAL, arp_timer, NULL);
}

/*
 * For FreeRTOS tickless
 */
int lwip_tickless_used = 0;

int arp_timeout_exist(void)
{
	return (arp_next_time != 0) ? 1 : 0;
}

long long wakeup_tick_arptimer(void)
{
	return (long long)arp_next_time;
}

int rltk_mii_recv_memcpy(u8 *buf, u32 frame_len)
{
#if defined(CONFIG_ETHERNET) && CONFIG_ETHERNET
	memcpy((u8 *)RX_BUFFER, (u8 *)buf, frame_len);
#endif
	return 0;
}

void ethernetif_recv(struct netif *netif, int total_len)
{
	struct eth_drv_sg sg_list[MAX_ETH_DRV_SG];
	struct pbuf *p, *q;
	int sg_len = 0;
	int idx = netif_get_idx(netif);

#if CONFIG_WLAN && CONFIG_WLAN_RX_ZERO_COPY && LWIP_SUPPORT_CUSTOM_PBUF
	wlan_rx_zc_report();
	p = wlan_rx_zc_try(idx, total_len);
	if (p != NULL) {
		if (ERR_OK != netif->input(p, netif)) {
			pbuf_free(p);
		}
		return;
	}
#endif

	// Allocate buffer to store received packet
	p = pbuf_alloc(PBUF_RAW, total_len, PBUF_POOL);
	if (p == NULL) {
		return;
	}

	// Create scatter list
	for (q = p; q != NULL && sg_len < MAX_ETH_DRV_SG; q = q->next) {
		sg_list[sg_len].buf = (unsigned int) q->payload;
		sg_list[sg_len++].len = q->len;
	}
	if (rltk_wlan_recv(idx, sg_list, sg_len) != 0) {
		/* A timed-out GDMA destination may contain only part of the frame. */
		pbuf_free(p);
		return;
	}

	// Pass received packet to the interface
	if (ERR_OK != netif->input(p, netif)) {
		pbuf_free(p);
	}
}

void lwip_PRE_SLEEP_PROCESSING(void)
{
}

void lwip_POST_SLEEP_PROCESSING(void)
{
}
