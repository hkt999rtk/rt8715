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
#include <hal_timer.h>
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

#ifndef CONFIG_NET_GDMA_BENCH
#define CONFIG_NET_GDMA_BENCH 0
#endif

#ifndef CONFIG_NET_GDMA_STATS
#define CONFIG_NET_GDMA_STATS 0
#endif

/*
 * Temporary, default-on profiling used to determine whether GDMA IRQ and
 * semaphore wake-up overhead is significant for the network's 1--4 KB
 * copies.  Measurements are accumulated per transfer but printed only by the
 * existing five-second statistics path.  Set this to 0 after board results
 * establish that completion latency is no longer of interest.
 */
#ifndef CONFIG_NET_GDMA_LATENCY_PROFILE
#define CONFIG_NET_GDMA_LATENCY_PROFILE 1
#endif

#ifndef CONFIG_TCP_PHASE_PROFILE
#define CONFIG_TCP_PHASE_PROFILE 1
#endif

#ifndef CONFIG_WLAN_RX_RING_GDMA_VERIFY
#define CONFIG_WLAN_RX_RING_GDMA_VERIFY 0
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

#define NET_GDMA_BENCH_FAIL_NONE      0U
#define NET_GDMA_BENCH_FAIL_ALLOC     1U
#define NET_GDMA_BENCH_FAIL_TRANSFER  2U
#define NET_GDMA_BENCH_FAIL_MISMATCH  3U

typedef struct net_gdma_context_s {
	gdma_t dma;
	_sema done;
	const char *name;
	dma_irq_handler callback;
	u8 initialized;
	u8 available;
	u8 tested;
	volatile u8 busy;
	volatile u8 in_flight;
	volatile u8 irq_error;
	volatile u32 irq_raw_error;
	u32 dma_errors;
	volatile u32 spurious_irqs;
	u32 dma_ops;
	u32 dma_bytes;
	u32 cpu_ops;
	u32 cpu_bytes;
	u32 alignment_fallbacks;
	u32 reentry_fallbacks;
	u32 timeouts;
	u32 report_probe;
	u32 last_report_tick;
#if CONFIG_NET_GDMA_BENCH
	u8 suppress_periodic_report;
	u32 bench_submit_cycles;
	u32 bench_dma_irq_cycles;
	u32 bench_wake_cycles;
	u32 bench_total_cycles;
	u32 bench_poll_count;
	u32 bench_yield_count;
	u32 bench_selftest_failures;
	u32 bench_selftest_last_len;
	u32 bench_selftest_last_offset;
	u8 bench_selftest_last_reason;
#endif
#if CONFIG_NET_GDMA_LATENCY_PROFILE
	volatile u8 latency_irq_seen;
	volatile u32 latency_irq_cycles;
	u32 latency_samples;
	u32 latency_irq_before_wait;
	u32 latency_submit_sum_cycles;
	u32 latency_submit_max_cycles;
	u32 latency_dma_irq_sum_cycles;
	u32 latency_dma_irq_max_cycles;
	u32 latency_wake_sum_cycles;
	u32 latency_wake_max_cycles;
	u32 latency_total_sum_cycles;
	u32 latency_total_max_cycles;
#endif
} net_gdma_context_t;

static net_gdma_context_t g_net_rx_gdma;
static net_gdma_context_t g_net_tx_gdma;
static net_gdma_context_t g_net_socket_rx_gdma;
static net_gdma_context_t g_net_tcp_tx_gdma;
#if CONFIG_NET_GDMA_BENCH
static net_gdma_context_t g_net_bench_gdma;
#endif
static u8 g_net_gdma_initialized;
#if CONFIG_NET_GDMA_LATENCY_PROFILE
static u8 g_net_gdma_latency_ready;
#endif

static void net_gdma_done_from_isr(net_gdma_context_t *ctx)
{
	phal_gdma_adaptor_t adaptor = &ctx->dma.hal_gdma_adaptor;
	u32 channel_mask;

	/*
	 * The SDK memcpy HAL originally enables TransferType and ErrType with one
	 * callback, so a caller cannot tell success from an error interrupt.  The
	 * network setup masks ErrType (an error-only transfer is handled by timeout)
	 * and this callback also samples the raw error bit for the case where the
	 * controller reports transfer-complete and error together.
	 *
	 * Ignore callbacks outside the active wait.  In particular, this prevents a
	 * completion left over from an aborted transfer from creating a semaphore
	 * token that could satisfy the next transfer.
	 */
	if (!ctx->in_flight || ctx->done == NULL || adaptor->gdma_dev == NULL) {
		ctx->spurious_irqs++;
		return;
	}

	channel_mask = 1U << adaptor->ch_num;
	ctx->irq_raw_error = adaptor->gdma_dev->raw_err & channel_mask;
	if (ctx->irq_raw_error != 0U) {
		ctx->irq_error = 1;
	}
#if CONFIG_NET_GDMA_LATENCY_PROFILE
	/*
	 * Timestamp immediately before giving the semaphore.  The task-side
	 * irq-to-return interval therefore includes the callback tail, IRQ return,
	 * semaphore wake-up and scheduling latency.
	 */
	if (g_net_gdma_latency_ready) {
		ctx->latency_irq_cycles = DWT->CYCCNT;
		ctx->latency_irq_seen = 1;
	}
#endif
	ctx->in_flight = 0;
	rtw_up_sema_from_isr(&ctx->done);
}

static void net_gdma_rx_done(uint32_t id)
{
	(void)id;
	net_gdma_done_from_isr(&g_net_rx_gdma);
}

static void net_gdma_tx_done(uint32_t id)
{
	(void)id;
	net_gdma_done_from_isr(&g_net_tx_gdma);
}

static void net_gdma_socket_rx_done(uint32_t id)
{
	(void)id;
	net_gdma_done_from_isr(&g_net_socket_rx_gdma);
}

static void net_gdma_tcp_tx_done(uint32_t id)
{
	(void)id;
	net_gdma_done_from_isr(&g_net_tcp_tx_gdma);
}

#if CONFIG_NET_GDMA_BENCH
static void net_gdma_bench_done(uint32_t id)
{
	net_gdma_context_t *ctx = &g_net_bench_gdma;

	(void)id;
	/* A polling benchmark must not depend on the ROM completion ISR.  This
	 * callback is installed only because dma_memcpy_init() requires one; any
	 * invocation means interrupt suppression failed and is diagnostic only. */
	ctx->spurious_irqs++;
}
#endif

static void net_gdma_drain_completion(net_gdma_context_t *ctx)
{
	while (rtw_down_timeout_sema(&ctx->done, 0) == _TRUE) {
	}
}

static void net_gdma_configure_completion_irq(net_gdma_context_t *ctx)
{
	phal_gdma_adaptor_t adaptor = &ctx->dma.hal_gdma_adaptor;

	/*
	 * Never let the ROM memcpy handler deliver an indistinguishable error IRQ
	 * through the completion callback.  Error-only transfers remain pending and
	 * are diagnosed from raw_err by the bounded timeout path below.
	 */
	hal_gdma_isr_dis(adaptor);
	hal_gdma_clean_pending_isr(adaptor);
	adaptor->gdma_isr_type = TransferType;
	hal_gdma_isr_en(adaptor);
}

static void net_gdma_init_context(net_gdma_context_t *ctx,
				  const char *name, dma_irq_handler callback)
{
	phal_gdma_adaptor_t adaptor;

	ctx->name = name;
	ctx->callback = callback;
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
	net_gdma_configure_completion_irq(ctx);

	ctx->available = 1;
	ctx->last_report_tick = rtw_get_current_time();
	printf("[NET_GDMA] %s allocated gdma=%u channel=%u threshold=%u\n",
	       name, (unsigned int)adaptor->gdma_index,
	       (unsigned int)adaptor->ch_num,
	       (unsigned int)NET_GDMA_COPY_THRESHOLD);
}

#if CONFIG_NET_GDMA_BENCH
static void net_gdma_init_benchmark_context(net_gdma_context_t *ctx)
{
	phal_gdma_adaptor_t adaptor;

	ctx->name = "BENCH";
	ctx->callback = net_gdma_bench_done;
	ctx->initialized = 1;
	/* Reserve a standalone channel without allocating a semaphore.  The
	 * benchmark completion path is hardware polling plus task yield only. */
	dma_memcpy_init(&ctx->dma, net_gdma_bench_done, 0);
	adaptor = &ctx->dma.hal_gdma_adaptor;
	if (!adaptor->have_chnl) {
		printf("[GDMA_BENCH] dedicated channel allocation failed\n");
		return;
	}
	adaptor->gdma_ctl.int_en = 0;
	hal_gdma_isr_dis(adaptor);
	hal_gdma_clean_pending_isr(adaptor);
	ctx->available = 1;
	ctx->tested = 1;
	ctx->suppress_periodic_report = 1;
	printf("[GDMA_BENCH] poll+yield channel gdma=%u channel=%u irq=off sema=none\n",
	       (unsigned int)adaptor->gdma_index,
	       (unsigned int)adaptor->ch_num);
}
#endif

static void net_gdma_init_all(void)
{
#if CONFIG_NET_GDMA_COPY
	if (g_net_gdma_initialized) {
		return;
	}

	/* WLAN setup is serialized before packet traffic starts. */
	g_net_gdma_initialized = 1;
#if CONFIG_NET_GDMA_LATENCY_PROFILE
	/*
	 * hal_read_curtime_us() latches the system timer through a register write
	 * and polling loop, so calling it in both task and GDMA IRQ context would
	 * add measurable overhead and allow nested latch operations.  CYCCNT is a
	 * single, re-entrant register read; unsigned deltas safely cover the short
	 * (bounded to 10 ms) DMA intervals even though the 32-bit counter wraps.
	 */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0U) {
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
		g_net_gdma_latency_ready =
			(DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) ? 1U : 0U;
	}
	printf("[NET_GDMA][LAT] profile enabled counter=%s core_hz=%u\n",
	       g_net_gdma_latency_ready ? "CYCCNT" : "unavailable",
	       (unsigned int)SystemCoreClock);
#endif
	/*
	 * Reserve all four channels once during serialized WLAN setup and retain
	 * them for the lifetime of the network stack.  Socket RX is deliberately
	 * separate: a blocking recv() copy must not contend with driver RX ingress.
	 */
	net_gdma_init_context(&g_net_rx_gdma, "RX", net_gdma_rx_done);
	net_gdma_init_context(&g_net_tx_gdma, "TX", net_gdma_tx_done);
	net_gdma_init_context(&g_net_socket_rx_gdma, "SOCKET_RX",
			       net_gdma_socket_rx_done);
	/* tcp_write() has independent producer/lifetime rules; never serialize it
	 * behind driver/NCM TX traffic on g_net_tx_gdma. */
	net_gdma_init_context(&g_net_tcp_tx_gdma, "TCP_TX",
			       net_gdma_tcp_tx_done);
#if CONFIG_NET_GDMA_BENCH
	/* Normally reserved before application startup by the benchmark thread.
	 * Keep this fallback for integrations that call the copy API directly. */
	if (!g_net_bench_gdma.initialized) {
		net_gdma_init_benchmark_context(&g_net_bench_gdma);
	}
#endif
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

static int net_gdma_transfer_aligned(const void *src, u32 len)
{
	/*
	 * RTL8195B board testing showed that the ROM hal_gdma_memcpy_config()
	 * can complete a transfer without reporting an error while corrupting
	 * data when a word-sized memory copy is submitted with an unaligned
	 * source (for example, aligned destination and source + 1).  The public
	 * HAL documentation does not state this restriction, so enforce the
	 * controller's four-byte transfer alignment here.  Destination alignment
	 * is already stricter (one cache line) in net_gdma_destination_safe().
	 * Unaligned edges are handled by the CPU wrapper below.
	 */
	return (((uintptr_t)src & (sizeof(u32) - 1U)) == 0U) &&
	       ((len & (sizeof(u32) - 1U)) == 0U);
}

static int net_gdma_wait(net_gdma_context_t *ctx)
{
	if (rtw_down_timeout_sema(&ctx->done, NET_GDMA_TIMEOUT_MS) == _TRUE) {
		if (!ctx->irq_error) {
			return 0;
		}
		ctx->dma_errors++;
		printf("[NET_GDMA][ERROR] %s DMA error gdma=%u channel=%u raw_err=0x%02x\n",
		       ctx->name,
		       (unsigned int)ctx->dma.hal_gdma_adaptor.gdma_index,
		       (unsigned int)ctx->dma.hal_gdma_adaptor.ch_num,
		       (unsigned int)ctx->irq_raw_error);
	} else {
		u32 channel_mask = 1U << ctx->dma.hal_gdma_adaptor.ch_num;
		u32 raw_error = ctx->dma.hal_gdma_adaptor.gdma_dev->raw_err & channel_mask;

		ctx->timeouts++;
		printf("[NET_GDMA][ERROR] %s %s after %u ms gdma=%u channel=%u "
		       "raw_err=0x%02x\n",
		       ctx->name, raw_error ? "DMA error timeout" : "timeout",
		       (unsigned int)NET_GDMA_TIMEOUT_MS,
		       (unsigned int)ctx->dma.hal_gdma_adaptor.gdma_index,
		       (unsigned int)ctx->dma.hal_gdma_adaptor.ch_num,
		       (unsigned int)raw_error);
	}

	/*
	 * Mask and invalidate the active generation before aborting.  Together with
	 * the callback's in_flight check, this prevents a late IRQ from satisfying
	 * the semaphore wait of a later transfer.
	 */
	hal_gdma_isr_dis(&ctx->dma.hal_gdma_adaptor);
	ctx->in_flight = 0;
	hal_gdma_abort(&ctx->dma.hal_gdma_adaptor);
	hal_gdma_clean_pending_isr(&ctx->dma.hal_gdma_adaptor);
	dma_memcpy_deinit(&ctx->dma);
	ctx->available = 0;
	ctx->tested = 0;
	ctx->irq_error = 0;
	ctx->irq_raw_error = 0;
	net_gdma_drain_completion(ctx);

	/* Try to restore this direction for the next packet. */
	dma_memcpy_init(&ctx->dma, ctx->callback, 0);
	if (ctx->dma.hal_gdma_adaptor.have_chnl) {
		net_gdma_configure_completion_irq(ctx);
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
	u32 i;
	int result = -1;

	src_raw = (u8 *)rtw_malloc(NET_GDMA_SELFTEST_ALLOC);
	dst_raw = (u8 *)rtw_malloc(NET_GDMA_SELFTEST_ALLOC);
	if (src_raw == NULL || dst_raw == NULL) {
		printf("[NET_GDMA][SELFTEST] %s allocation failed\n", ctx->name);
#if CONFIG_NET_GDMA_BENCH
		ctx->bench_selftest_last_reason = NET_GDMA_BENCH_FAIL_ALLOC;
		ctx->bench_selftest_failures++;
#endif
		goto exit;
	}

	src_base = (u8 *)(((uintptr_t)src_raw + NET_GDMA_CACHE_LINE - 1U) &
			  ~(uintptr_t)(NET_GDMA_CACHE_LINE - 1U));
	dst = (u8 *)(((uintptr_t)dst_raw + NET_GDMA_CACHE_LINE - 1U) &
		     ~(uintptr_t)(NET_GDMA_CACHE_LINE - 1U));

	for (case_index = 0; case_index < sizeof(lengths) / sizeof(lengths[0]);
	     ++case_index) {
		u8 *src = src_base;
		u32 len = lengths[case_index];

		/*
		 * Test the raw HAL only with the alignment guaranteed by the
		 * production submission path.  Misaligned caller buffers are tested
		 * by the benchmark through net_gdma_copy_with_edges(); requiring the
		 * raw ROM HAL to support source + 1 incorrectly disables a healthy
		 * channel even though that combination is never submitted now.
		 */
		for (i = 0; i < len; ++i) {
			src[i] = (u8)(i + len);
		}
		rtw_memset(dst, 0xA5, len);
		net_gdma_drain_completion(ctx);
		ctx->in_flight = 1;
		dma_memcpy(&ctx->dma, dst, src, len);
		if (net_gdma_wait(ctx) != 0) {
#if CONFIG_NET_GDMA_BENCH
			ctx->bench_selftest_last_reason =
				NET_GDMA_BENCH_FAIL_TRANSFER;
			ctx->bench_selftest_last_len = len;
			ctx->bench_selftest_last_offset = 0U;
			ctx->bench_selftest_failures++;
#endif
			printf("[NET_GDMA][SELFTEST] %s FAIL len=%u src_offset=0\n",
			       ctx->name, (unsigned int)len);
			goto exit;
		}
		if (rtw_memcmp(dst, src, len) != _TRUE) {
#if CONFIG_NET_GDMA_BENCH
			ctx->bench_selftest_last_reason =
				NET_GDMA_BENCH_FAIL_MISMATCH;
			ctx->bench_selftest_last_len = len;
			ctx->bench_selftest_last_offset = 0U;
			ctx->bench_selftest_failures++;
#endif
			printf("[NET_GDMA][SELFTEST] %s FAIL len=%u src_offset=0\n",
			       ctx->name, (unsigned int)len);
			goto exit;
		}
	}

	result = 0;
#if CONFIG_NET_GDMA_BENCH
	ctx->bench_selftest_last_reason = NET_GDMA_BENCH_FAIL_NONE;
#endif
	printf("[NET_GDMA][SELFTEST] %s PASS cases=%u\n", ctx->name,
	       (unsigned int)(sizeof(lengths) / sizeof(lengths[0])));

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

#if CONFIG_NET_GDMA_LATENCY_PROFILE
static void net_gdma_latency_record(net_gdma_context_t *ctx,
				    u32 start_cycles, u32 submit_done_cycles,
				    u32 wait_return_cycles, int irq_before_wait)
{
	u32 irq_cycles;
	u32 submit_cycles;
	u32 dma_irq_cycles;
	u32 wake_cycles;
	u32 total_cycles;

	/* A successful semaphore wait must have observed the matching callback. */
	if (!ctx->latency_irq_seen) {
		return;
	}

	irq_cycles = ctx->latency_irq_cycles;
	submit_cycles = submit_done_cycles - start_cycles;
	/*
	 * A very short transfer can complete inside dma_memcpy().  In that case
	 * submit_cycles already contains setup, DMA, IRQ and HAL completion processing;
	 * do not subtract timestamps in the opposite order across a 32-bit wrap.
	 */
	dma_irq_cycles = irq_before_wait ? 0U : irq_cycles - submit_done_cycles;
	wake_cycles = wait_return_cycles - irq_cycles;
	total_cycles = wait_return_cycles - start_cycles;

#if CONFIG_NET_GDMA_BENCH
	ctx->bench_submit_cycles = submit_cycles;
	ctx->bench_dma_irq_cycles = dma_irq_cycles;
	ctx->bench_wake_cycles = wake_cycles;
	ctx->bench_total_cycles = total_cycles;
	/* The benchmark consumes only the latest sample. Avoid overflowing the
	 * normal long-running aggregate counters on its dedicated channel. */
	if (ctx->suppress_periodic_report) {
		return;
	}
#endif

	ctx->latency_samples++;
	ctx->latency_irq_before_wait += irq_before_wait ? 1U : 0U;
	ctx->latency_submit_sum_cycles += submit_cycles;
	ctx->latency_dma_irq_sum_cycles += dma_irq_cycles;
	ctx->latency_wake_sum_cycles += wake_cycles;
	ctx->latency_total_sum_cycles += total_cycles;
	if (submit_cycles > ctx->latency_submit_max_cycles) {
		ctx->latency_submit_max_cycles = submit_cycles;
	}
	if (dma_irq_cycles > ctx->latency_dma_irq_max_cycles) {
		ctx->latency_dma_irq_max_cycles = dma_irq_cycles;
	}
	if (wake_cycles > ctx->latency_wake_max_cycles) {
		ctx->latency_wake_max_cycles = wake_cycles;
	}
	if (total_cycles > ctx->latency_total_max_cycles) {
		ctx->latency_total_max_cycles = total_cycles;
	}
}

static u32 net_gdma_cycles_to_us(u32 cycles)
{
	u32 cycles_per_us = SystemCoreClock / 1000000U;

	if (cycles_per_us == 0U) {
		return 0U;
	}
	return (cycles + (cycles_per_us / 2U)) / cycles_per_us;
}
#endif

static void net_gdma_account(net_gdma_context_t *ctx, int used_dma, u32 len)
{
#if CONFIG_NET_GDMA_STATS || CONFIG_NET_GDMA_LATENCY_PROFILE
	u32 now;
	u32 elapsed_ms;

#if CONFIG_NET_GDMA_BENCH
	if (ctx->suppress_periodic_report) {
		return;
	}
#endif

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
	       "align_fallback=%u reentry=%u timeout=%u dma_error=%u late_irq=%u\n",
	       ctx->name, (unsigned int)elapsed_ms,
	       (unsigned int)ctx->dma_ops, (unsigned int)ctx->dma_bytes,
	       (unsigned int)ctx->cpu_ops, (unsigned int)ctx->cpu_bytes,
	       (unsigned int)ctx->alignment_fallbacks,
	       (unsigned int)ctx->reentry_fallbacks,
	       (unsigned int)ctx->timeouts,
	       (unsigned int)ctx->dma_errors,
	       (unsigned int)ctx->spurious_irqs);

#if CONFIG_NET_GDMA_LATENCY_PROFILE
	if (ctx->latency_samples != 0U) {
		printf("[NET_GDMA][%s][LAT] samples=%u irq_before_wait=%u "
		       "submit_avg/max=%u/%uus dma_irq_avg/max=%u/%uus "
		       "wake_avg/max=%u/%uus total_avg/max=%u/%uus\n",
		       ctx->name,
		       (unsigned int)ctx->latency_samples,
		       (unsigned int)ctx->latency_irq_before_wait,
		       (unsigned int)net_gdma_cycles_to_us(
			       ctx->latency_submit_sum_cycles / ctx->latency_samples),
		       (unsigned int)net_gdma_cycles_to_us(ctx->latency_submit_max_cycles),
		       (unsigned int)net_gdma_cycles_to_us(
			       ctx->latency_dma_irq_sum_cycles / ctx->latency_samples),
		       (unsigned int)net_gdma_cycles_to_us(ctx->latency_dma_irq_max_cycles),
		       (unsigned int)net_gdma_cycles_to_us(
			       ctx->latency_wake_sum_cycles / ctx->latency_samples),
		       (unsigned int)net_gdma_cycles_to_us(ctx->latency_wake_max_cycles),
		       (unsigned int)net_gdma_cycles_to_us(
			       ctx->latency_total_sum_cycles / ctx->latency_samples),
		       (unsigned int)net_gdma_cycles_to_us(ctx->latency_total_max_cycles));
	}
#endif

	ctx->dma_ops = 0;
	ctx->dma_bytes = 0;
	ctx->cpu_ops = 0;
	ctx->cpu_bytes = 0;
	ctx->alignment_fallbacks = 0;
	ctx->reentry_fallbacks = 0;
	ctx->timeouts = 0;
	ctx->dma_errors = 0;
	ctx->spurious_irqs = 0;
#if CONFIG_NET_GDMA_LATENCY_PROFILE
	ctx->latency_samples = 0;
	ctx->latency_irq_before_wait = 0;
	ctx->latency_submit_sum_cycles = 0;
	ctx->latency_submit_max_cycles = 0;
	ctx->latency_dma_irq_sum_cycles = 0;
	ctx->latency_dma_irq_max_cycles = 0;
	ctx->latency_wake_sum_cycles = 0;
	ctx->latency_wake_max_cycles = 0;
	ctx->latency_total_sum_cycles = 0;
	ctx->latency_total_max_cycles = 0;
#endif
	ctx->last_report_tick = now;
#else
	(void)ctx;
	(void)used_dma;
	(void)len;
#endif
}

static int net_gdma_copy(net_gdma_context_t *ctx, void *dst, const void *src,
			 u32 len, const void *allocation_end, u32 *dma_len)
{
#if CONFIG_NET_GDMA_LATENCY_PROFILE
	u32 latency_start_cycles;
	u32 latency_submit_done_cycles;
	u32 latency_wait_return_cycles;
	int latency_irq_before_wait;
	int latency_profile_active;
#endif

	if (dma_len != NULL) {
		*dma_len = 0;
	}

#if CONFIG_NET_GDMA_BENCH
	ctx->bench_submit_cycles = 0;
	ctx->bench_dma_irq_cycles = 0;
	ctx->bench_wake_cycles = 0;
	ctx->bench_total_cycles = 0;
#endif

	if (!g_net_gdma_initialized) {
		net_gdma_init_all();
	}

	if (len < NET_GDMA_COPY_THRESHOLD || !ctx->available) {
		rtw_memcpy(dst, (void *)src, len);
		net_gdma_account(ctx, 0, len);
		return 0;
	}

	if (!net_gdma_transfer_aligned(src, len) ||
	    !net_gdma_destination_safe(dst, len, allocation_end)) {
		ctx->alignment_fallbacks++;
		rtw_memcpy(dst, (void *)src, len);
		net_gdma_account(ctx, 0, len);
		return 0;
	}

	/*
	 * Each network data path owns a fixed channel. Claim it atomically with
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
	hal_gdma_clean_pending_isr(&ctx->dma.hal_gdma_adaptor);
	ctx->irq_error = 0;
	ctx->irq_raw_error = 0;
	ctx->in_flight = 1;
#if CONFIG_NET_GDMA_LATENCY_PROFILE
	ctx->latency_irq_seen = 0;
	latency_profile_active = g_net_gdma_latency_ready ? 1 : 0;
	latency_start_cycles = latency_profile_active ? DWT->CYCCNT : 0U;
#endif
	dma_memcpy(&ctx->dma, dst, (void *)src, len);
#if CONFIG_NET_GDMA_LATENCY_PROFILE
	latency_submit_done_cycles = latency_profile_active ? DWT->CYCCNT : 0U;
	latency_irq_before_wait = ctx->latency_irq_seen ? 1 : 0;
#endif
	if (net_gdma_wait(ctx) != 0) {
		/* Destination may be partially written.  Caller must drop the packet. */
		ctx->busy = 0;
		return -1;
	}
#if CONFIG_NET_GDMA_LATENCY_PROFILE
	if (latency_profile_active) {
		latency_wait_return_cycles = DWT->CYCCNT;
		net_gdma_latency_record(ctx, latency_start_cycles,
					latency_submit_done_cycles,
					latency_wait_return_cycles,
					latency_irq_before_wait);
	}
#endif

	ctx->busy = 0;
	net_gdma_account(ctx, 1, len);
	if (dma_len != NULL) {
		*dma_len = len;
	}
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
				    const void *allocation_end,
				    u32 *dma_len)
{
	uintptr_t start = (uintptr_t)dst;
	u32 prefix;
	u32 body;
	u32 suffix;

	/*
	 * Initialize before inspecting ctx->available.  Otherwise the very first
	 * request can enter net_gdma_copy(), allocate a channel there, and submit
	 * the entire unaligned destination without the cache-line edge split.
	 */
	if (!g_net_gdma_initialized) {
		net_gdma_init_all();
	}

	if (len < NET_GDMA_COPY_THRESHOLD ||
	    (((start & (NET_GDMA_CACHE_LINE - 1U)) == 0U) &&
	     (allocation_end != NULL ||
	      (len & (NET_GDMA_CACHE_LINE - 1U)) == 0U))) {
		return net_gdma_copy(ctx, dst, src, len, allocation_end,
				      dma_len);
	}

	prefix = (start & (NET_GDMA_CACHE_LINE - 1U)) != 0U ?
		 NET_GDMA_CACHE_LINE -
		 (u32)(start & (NET_GDMA_CACHE_LINE - 1U)) : 0U;
	if (prefix >= len) {
		return net_gdma_copy(ctx, dst, src, len, allocation_end,
				      dma_len);
	}
	body = (len - prefix) & ~(NET_GDMA_CACHE_LINE - 1U);
	if (body < NET_GDMA_COPY_THRESHOLD) {
		return net_gdma_copy(ctx, dst, src, len, allocation_end,
				      dma_len);
	}
	suffix = len - prefix - body;

	if (prefix != 0U) {
		rtw_memcpy(dst, (void *)src, prefix);
		net_gdma_account(ctx, 0, prefix);
	}
	if (net_gdma_copy(ctx, (u8 *)dst + prefix, (const u8 *)src + prefix,
			  body, allocation_end, dma_len) != 0) {
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
#if CONFIG_NET_GDMA_COPY
	return net_gdma_copy_with_edges(&g_net_tx_gdma, dst, src, len,
					allocation_end, NULL);
#else
	(void)allocation_end;
	rtw_memcpy(dst, (void *)src, len);
	return 0;
#endif
}

int rltk_network_gdma_copy_rx(void *dst, const void *src, unsigned int len,
			      const void *allocation_end)
{
#if CONFIG_NET_GDMA_COPY
	return net_gdma_copy_with_edges(&g_net_rx_gdma, dst, src, len,
					allocation_end, NULL);
#else
	(void)allocation_end;
	rtw_memcpy(dst, (void *)src, len);
	return 0;
#endif
}

#if CONFIG_NET_GDMA_BENCH
static int net_gdma_benchmark_poll_body(
	net_gdma_context_t *ctx, void *dst, const void *src, u32 len,
	const void *allocation_end, rltk_network_gdma_bench_sample_t *sample)
{
	phal_gdma_adaptor_t adaptor = &ctx->dma.hal_gdma_adaptor;
	u32 start_cycles;
	u32 submit_done_cycles;
	u32 poll_done_cycles;
	u32 finish_done_cycles;
	u32 timeout_cycles;
	u32 channel_mask;
	hal_status_t status;

	if (len < NET_GDMA_COPY_THRESHOLD || !ctx->available ||
	    !net_gdma_transfer_aligned(src, len) ||
	    !net_gdma_destination_safe(dst, len, allocation_end)) {
		rtw_memcpy(dst, (void *)src, len);
		return 0;
	}

	/* This context belongs only to the benchmark task.  Keep the guard anyway
	 * so an accidental second caller cannot overwrite an active descriptor. */
	save_and_cli();
	if (ctx->busy) {
		restore_flags();
		rtw_memcpy(dst, (void *)src, len);
		return 0;
	}
	ctx->busy = 1;
	restore_flags();

	/* This dedicated benchmark channel never uses a completion semaphore or
	 * completion IRQ.  Configure and start the HAL directly, poll the channel
	 * enable bit, and yield after each incomplete observation. */
	hal_gdma_clean_pending_isr(adaptor);
	ctx->irq_error = 0;
	ctx->irq_raw_error = 0;
	ctx->in_flight = 1;
	start_cycles = DWT->CYCCNT;
	status = hal_gdma_memcpy_config(adaptor, dst, (void *)src, len);
	if (status != HAL_OK) {
		ctx->in_flight = 0;
		ctx->busy = 0;
		printf("[GDMA_BENCH][ERROR] poll+yield config failed len=%u err=%d\n",
		       (unsigned int)len, (int)status);
		return -1;
	}
	/* hal_gdma_memcpy_config() restores the normal memcpy defaults, including
	 * interrupt enable.  Override it after configuration so transfer_start()
	 * programs INT_EN=0 into the channel control register. */
	adaptor->gdma_ctl.int_en = 0;
	hal_gdma_isr_dis(adaptor);
	if (adaptor->dcache_clean_by_addr != NULL) {
		adaptor->dcache_clean_by_addr((uint32_t *)src, (int32_t)len);
		/* The caller may have just filled this cacheable destination.  Clean
		 * dirty lines before GDMA starts so an eviction while this task is
		 * yielded cannot write stale bytes over completed DMA output.  The
		 * post-transfer invalidate below then exposes the DMA result. */
		adaptor->dcache_clean_by_addr((uint32_t *)dst, (int32_t)len);
	}
	hal_gdma_transfer_start(adaptor, MultiBlkDis);
	submit_done_cycles = DWT->CYCCNT;
	timeout_cycles = (SystemCoreClock / 1000U) * NET_GDMA_TIMEOUT_MS;
	channel_mask = 1U << adaptor->ch_num;

	/* Do not use hal_gdma_query_chnl_en() here: the RTL8195B ROM helper also
	 * observes adaptor->busy, which is normally cleared by the memcpy ISR.
	 * With IRQ deliberately disabled that software flag stays set forever.
	 * CH_EN is the hardware completion indication and is documented to clear
	 * automatically after the final AXI write reaches the destination. */
	while ((adaptor->gdma_dev->ch_en_reg & channel_mask) != 0U) {
		ctx->bench_poll_count++;
		ctx->irq_raw_error = adaptor->gdma_dev->raw_err & channel_mask;
		if (ctx->irq_raw_error != 0U) {
			ctx->irq_error = 1;
			break;
		}
		if ((DWT->CYCCNT - start_cycles) >= timeout_cycles) {
			ctx->timeouts++;
			ctx->in_flight = 0;
			hal_gdma_abort(adaptor);
			hal_gdma_clean_pending_isr(adaptor);
			ctx->available = 0;
			ctx->tested = 0;
			ctx->busy = 0;
			printf("[GDMA_BENCH][ERROR] poll+yield timeout len=%u ch_en=0x%x raw_tfr=0x%x\n",
			       (unsigned int)len,
			       (unsigned int)adaptor->gdma_dev->ch_en_reg,
			       (unsigned int)adaptor->gdma_dev->raw_tfr);
			return -1;
		}
		ctx->bench_yield_count++;
		rtw_yield_os();
	}
	poll_done_cycles = DWT->CYCCNT;
	ctx->in_flight = 0;
	ctx->irq_raw_error = adaptor->gdma_dev->raw_err & channel_mask;
	if (ctx->irq_raw_error != 0U) {
		ctx->irq_error = 1;
	}

	if (ctx->irq_error) {
		ctx->dma_errors++;
		hal_gdma_abort(adaptor);
		hal_gdma_clean_pending_isr(adaptor);
		ctx->busy = 0;
		printf("[GDMA_BENCH][ERROR] poll+yield DMA error len=%u raw=0x%x\n",
		       (unsigned int)len, (unsigned int)ctx->irq_raw_error);
		return -1;
	}

	/* With the memcpy ISR disabled, its normal destination-cache maintenance
	 * and pending-bit cleanup must be completed synchronously before the caller
	 * compares the copied bytes. */
	if (adaptor->dcache_invalidate_by_addr != NULL) {
		adaptor->dcache_invalidate_by_addr((uint32_t *)dst, (int32_t)len);
	}
	hal_gdma_clean_pending_isr(adaptor);
	adaptor->busy = 0;
	finish_done_cycles = DWT->CYCCNT;

	sample->dma_bytes = len;
	sample->submit_cycles = submit_done_cycles - start_cycles;
	sample->poll_cycles = poll_done_cycles - submit_done_cycles;
	sample->finish_cycles = finish_done_cycles - poll_done_cycles;
	sample->dma_total_cycles = finish_done_cycles - start_cycles;
	sample->poll_count = ctx->bench_poll_count;
	sample->yield_count = ctx->bench_yield_count;
	ctx->busy = 0;
	return 0;
}

static int net_gdma_benchmark_poll_copy(
	void *dst, const void *src, u32 len, const void *allocation_end,
	rltk_network_gdma_bench_sample_t *sample)
{
	uintptr_t start = (uintptr_t)dst;
	u32 prefix;
	u32 body;
	u32 suffix;
	int result;

	if (len < NET_GDMA_COPY_THRESHOLD ||
	    (((start & (NET_GDMA_CACHE_LINE - 1U)) == 0U) &&
	     (allocation_end != NULL ||
	      (len & (NET_GDMA_CACHE_LINE - 1U)) == 0U))) {
		return net_gdma_benchmark_poll_body(
			&g_net_bench_gdma, dst, src, len, allocation_end, sample);
	}

	prefix = (start & (NET_GDMA_CACHE_LINE - 1U)) != 0U ?
		 NET_GDMA_CACHE_LINE -
		 (u32)(start & (NET_GDMA_CACHE_LINE - 1U)) : 0U;
	if (prefix >= len) {
		rtw_memcpy(dst, (void *)src, len);
		return 0;
	}
	body = (len - prefix) & ~(NET_GDMA_CACHE_LINE - 1U);
	if (body < NET_GDMA_COPY_THRESHOLD) {
		rtw_memcpy(dst, (void *)src, len);
		return 0;
	}
	suffix = len - prefix - body;
	if (prefix != 0U) {
		rtw_memcpy(dst, (void *)src, prefix);
	}
	result = net_gdma_benchmark_poll_body(
		&g_net_bench_gdma, (u8 *)dst + prefix,
		(const u8 *)src + prefix, body, allocation_end, sample);
	if (result != 0) {
		return result;
	}
	if (suffix != 0U) {
		rtw_memcpy((u8 *)dst + prefix + body,
			   (void *)((const u8 *)src + prefix + body), suffix);
	}
	return 0;
}
#endif

int rltk_network_gdma_benchmark_init(void)
{
#if CONFIG_NET_GDMA_BENCH
	/*
	 * Reserve a genuinely separate channel before the rest of the application
	 * starts allocating transient GDMA users.  This does not replace, borrow or
	 * reorder any of the four production network contexts.
	 */
	if (!g_net_bench_gdma.initialized) {
		CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
		net_gdma_init_benchmark_context(&g_net_bench_gdma);
		/* Every timed transfer is checked byte-for-byte by the benchmark, so
		 * skip the production semaphore-based one-time self-test here. */
		g_net_bench_gdma.tested = g_net_bench_gdma.available ? 1U : 0U;
	}
	return g_net_bench_gdma.available ? 0 : -1;
#else
	return -1;
#endif
}

void rltk_network_gdma_benchmark_print_status(unsigned int sequence)
{
#if CONFIG_NET_GDMA_BENCH
	static const char *const reasons[] = {
		"none", "alloc", "transfer", "mismatch"
	};
	net_gdma_context_t *ctx = &g_net_bench_gdma;
	u32 reason = ctx->bench_selftest_last_reason;

	if (reason >= sizeof(reasons) / sizeof(reasons[0])) {
		reason = NET_GDMA_BENCH_FAIL_NONE;
	}
	printf("[GDMA_BENCH][%u][STATUS] initialized=%u available=%u tested=%u "
	       "busy=%u in_flight=%u gdma=%u channel=%u selftest_fail=%u "
	       "last=%s/%uB/off%u timeout=%u dma_error=%u unexpected_irq=%u\n",
	       sequence, (unsigned int)ctx->initialized,
	       (unsigned int)ctx->available, (unsigned int)ctx->tested,
	       (unsigned int)ctx->busy, (unsigned int)ctx->in_flight,
	       (unsigned int)ctx->dma.hal_gdma_adaptor.gdma_index,
	       (unsigned int)ctx->dma.hal_gdma_adaptor.ch_num,
	       (unsigned int)ctx->bench_selftest_failures, reasons[reason],
	       (unsigned int)ctx->bench_selftest_last_len,
	       (unsigned int)ctx->bench_selftest_last_offset,
	       (unsigned int)ctx->timeouts, (unsigned int)ctx->dma_errors,
	       (unsigned int)ctx->spurious_irqs);
#else
	(void)sequence;
#endif
}

int rltk_network_gdma_benchmark_copy(
	void *dst, const void *src, unsigned int len, const void *allocation_end,
	rltk_network_gdma_bench_sample_t *sample)
{
#if CONFIG_NET_GDMA_BENCH
	int result;

	if (sample == NULL) {
		return -1;
	}
	rtw_memset(sample, 0, sizeof(*sample));
	g_net_bench_gdma.bench_poll_count = 0;
	g_net_bench_gdma.bench_yield_count = 0;
	result = net_gdma_benchmark_poll_copy(dst, src, len, allocation_end,
					      sample);
	return result;
#else
	(void)allocation_end;
	if (sample != NULL) {
		rtw_memset(sample, 0, sizeof(*sample));
	}
	/* Keep the benchmark meaningful when NET_GDMA_COPY=0: this is an
	 * intentional CPU fallback, not a failed or missing copy. */
	rtw_memcpy(dst, (void *)src, len);
	return 0;
#endif
}

/*
 * Socket recv owns both the source pbuf and the caller's destination buffer
 * until this blocking copy returns. That makes timeout recovery different
 * from the driver RX path: net_gdma_wait() has already aborted the channel,
 * so the still-valid pbuf can be copied again by the CPU without changing the
 * BSD recv() success/error contract.
 *
 * A TCP pbuf chain is scatter/gather. AmebaPro exposes multi-block GDMA, but
 * its SDK cache maintenance treats all source blocks as one continuous range.
 * Non-contiguous pbuf payloads violate that assumption, so socket RX submits
 * segments sequentially on the dedicated socket RX channel instead of using
 * the unsafe list wrapper.
 */
int rltk_network_gdma_copy_socket_rx(void *dst, const void *src,
				     unsigned int len)
{
#if !CONFIG_NET_GDMA_COPY
	rtw_memcpy(dst, (void *)src, len);
	return 0;
#else
#if CONFIG_NET_GDMA_STATS
	static u32 dma_ops;
	static u32 dma_bytes;
	static u32 cpu_ops;
	static u32 cpu_bytes;
	static u32 report_probe;
	static u32 last_report_tick;
#endif
	static u32 recoveries;
	u32 dma_len = 0;
#if CONFIG_NET_GDMA_STATS
	u32 now;
#endif
	int result;
	uintptr_t dst_start = (uintptr_t)dst;
	uintptr_t src_start = (uintptr_t)src;
	uintptr_t dst_end = dst_start + len;
	uintptr_t src_end = src_start + len;
	int dma_accessible;

	/*
	 * GDMA is a bus master and cannot access M33 DTCM.  BSD recv() is generic,
	 * so its caller may legally pass a stack/DTCM buffer even though CarPlay's
	 * high-throughput buffers normally live in LPDDR.  Restrict DMA to the bus
	 * visible SRAM, PSRAM and LPDDR windows; all other buffers use CPU copy.
	 */
	dma_accessible = dst_end >= dst_start && src_end >= src_start &&
		(((dst_start >= 0x20100000U) && (dst_end <= 0x2017A000U)) ||
		 ((dst_start >= 0x60000000U) && (dst_end <= 0x60800000U)) ||
		 ((dst_start >= 0x70000000U) && (dst_end <= 0x72000000U))) &&
		(((src_start >= 0x20100000U) && (src_end <= 0x2017A000U)) ||
		 ((src_start >= 0x60000000U) && (src_end <= 0x60800000U)) ||
		 ((src_start >= 0x70000000U) && (src_end <= 0x72000000U)));

	/*
	 * recv() may return fewer bytes than the caller requested. Do not treat
	 * the unused caller capacity as disposable cache padding: preserving it
	 * requires CPU-copying both unaligned edges around a cache-line body.
	 */
	if (dma_accessible) {
		result = net_gdma_copy_with_edges(&g_net_socket_rx_gdma, dst, src,
						  len, NULL, &dma_len);
	} else {
		rtw_memcpy(dst, (void *)src, len);
		result = 0;
	}
	if (result != 0) {
		/* The timeout path aborts/deinitializes DMA before returning. */
		recoveries++;
		printf("[LWIP_RECV_GDMA][RECOVERY] len=%u result=%d count=%u\n",
		       len, result, (unsigned int)recoveries);
		rtw_memcpy(dst, (void *)src, len);
		dma_len = 0;
	}

#if CONFIG_NET_GDMA_STATS
	if (dma_len != 0U) {
		dma_ops++;
		dma_bytes += dma_len;
	}
	if (dma_len < len) {
		cpu_ops++;
		cpu_bytes += len - dma_len;
	}

	if (last_report_tick == 0U) {
		last_report_tick = rtw_get_current_time();
	}
	report_probe++;
	if ((report_probe % NET_GDMA_REPORT_PROBE) == 0U) {
		now = rtw_get_current_time();
		if (rtw_systime_to_ms(now - last_report_tick) >=
		    NET_GDMA_REPORT_MS) {
			printf("[LWIP_RECV_GDMA][STATS] dma=%u/%uB cpu=%u/%uB "
			       "recoveries=%u\n",
			       (unsigned int)dma_ops, (unsigned int)dma_bytes,
			       (unsigned int)cpu_ops, (unsigned int)cpu_bytes,
			       (unsigned int)recoveries);
			dma_ops = 0;
			dma_bytes = 0;
			cpu_ops = 0;
			cpu_bytes = 0;
			recoveries = 0;
			last_report_tick = now;
		}
	}
#endif

	return 0;
#endif /* CONFIG_NET_GDMA_COPY */
}

/*
 * tcp_write() retains both buffers until this blocking helper returns, so a
 * failed transfer can be repaired from the untouched source.  The destination
 * is commonly offset by TCP headroom; cache-line edges therefore stay on the
 * CPU while only the isolated middle is submitted to this dedicated channel.
 * Keeping TCP_TX separate prevents a TCPIP-thread copy from contending with
 * the driver/NCM TX channel.
 */
int rltk_network_gdma_copy_tcp_tx(void *dst, const void *src,
				  unsigned int len, unsigned int *dma_len_out)
{
#if !CONFIG_NET_GDMA_COPY
	if (dma_len_out != NULL) {
		*dma_len_out = 0U;
	}
	rtw_memcpy(dst, (void *)src, len);
	return 0;
#else
	u32 dma_len = 0;
	uintptr_t dst_start = (uintptr_t)dst;
	uintptr_t src_start = (uintptr_t)src;
	uintptr_t dst_end = dst_start + len;
	uintptr_t src_end = src_start + len;
	int result;
	int dma_accessible;

	dma_accessible = dst_end >= dst_start && src_end >= src_start &&
		(((dst_start >= 0x20100000U) && (dst_end <= 0x2017A000U)) ||
		 ((dst_start >= 0x60000000U) && (dst_end <= 0x60800000U)) ||
		 ((dst_start >= 0x70000000U) && (dst_end <= 0x72000000U))) &&
		(((src_start >= 0x20100000U) && (src_end <= 0x2017A000U)) ||
		 ((src_start >= 0x60000000U) && (src_end <= 0x60800000U)) ||
		 ((src_start >= 0x70000000U) && (src_end <= 0x72000000U)));

	if (dma_accessible) {
		result = net_gdma_copy_with_edges(&g_net_tcp_tx_gdma, dst, src,
						  len, NULL, &dma_len);
	} else {
		rtw_memcpy(dst, (void *)src, len);
		result = 0;
	}
	if (result != 0) {
		printf("[TCP_TX_GDMA][RECOVERY] len=%u result=%d; CPU copy\n",
		       len, result);
		rtw_memcpy(dst, (void *)src, len);
		dma_len = 0;
	}
	if (dma_len_out != NULL) {
		*dma_len_out = dma_len;
	}
	return 0;
#endif /* CONFIG_NET_GDMA_COPY */
}

#if CONFIG_TCP_PHASE_PROFILE
typedef struct tcp_phase_profile_s {
	u32 window_start_us;
	u32 rx_ops;
	u32 rx_bytes;
	u32 rx_total_us;
	u32 rx_checksum_us;
	u32 tx_ops;
	u32 tx_bytes;
	u32 tx_total_us;
	u32 tx_copy_ops;
	u32 tx_copy_bytes;
	u32 tx_copy_dma_bytes;
	u32 tx_copy_us;
} tcp_phase_profile_t;

static tcp_phase_profile_t g_tcp_phase_profile;

static void rltk_tcp_perf_report(u32 now)
{
	u32 window;

	if (g_tcp_phase_profile.window_start_us == 0U) {
		g_tcp_phase_profile.window_start_us = now;
		return;
	}
	window = now - g_tcp_phase_profile.window_start_us;
	if (window < 5000000U) {
		return;
	}
	printf("[TCP_PERF] window_us=%u RX ops=%u bytes=%u total_us=%u checksum_us=%u; "
	       "TX ops=%u bytes=%u total_us=%u copy=%u/%uB dma=%uB copy_us=%u\n",
	       (unsigned int)window,
	       (unsigned int)g_tcp_phase_profile.rx_ops,
	       (unsigned int)g_tcp_phase_profile.rx_bytes,
	       (unsigned int)g_tcp_phase_profile.rx_total_us,
	       (unsigned int)g_tcp_phase_profile.rx_checksum_us,
	       (unsigned int)g_tcp_phase_profile.tx_ops,
	       (unsigned int)g_tcp_phase_profile.tx_bytes,
	       (unsigned int)g_tcp_phase_profile.tx_total_us,
	       (unsigned int)g_tcp_phase_profile.tx_copy_ops,
	       (unsigned int)g_tcp_phase_profile.tx_copy_bytes,
	       (unsigned int)g_tcp_phase_profile.tx_copy_dma_bytes,
	       (unsigned int)g_tcp_phase_profile.tx_copy_us);
	rtw_memset(&g_tcp_phase_profile, 0, sizeof(g_tcp_phase_profile));
	g_tcp_phase_profile.window_start_us = now;
}

unsigned int rltk_tcp_perf_now_us(void)
{
	return hal_read_curtime_us();
}

void rltk_tcp_perf_rx_complete(unsigned int start_us, unsigned int bytes,
			       unsigned int checksum_us)
{
	u32 now = hal_read_curtime_us();
	g_tcp_phase_profile.rx_ops++;
	g_tcp_phase_profile.rx_bytes += bytes;
	g_tcp_phase_profile.rx_total_us += now - start_us;
	g_tcp_phase_profile.rx_checksum_us += checksum_us;
	rltk_tcp_perf_report(now);
}

void rltk_tcp_perf_tx_complete(unsigned int start_us, unsigned int bytes)
{
	u32 now = hal_read_curtime_us();
	g_tcp_phase_profile.tx_ops++;
	g_tcp_phase_profile.tx_bytes += bytes;
	g_tcp_phase_profile.tx_total_us += now - start_us;
	rltk_tcp_perf_report(now);
}

void rltk_tcp_perf_tx_copy(unsigned int bytes, unsigned int dma_bytes,
			   unsigned int elapsed_us)
{
	g_tcp_phase_profile.tx_copy_ops++;
	g_tcp_phase_profile.tx_copy_bytes += bytes;
	g_tcp_phase_profile.tx_copy_dma_bytes += dma_bytes;
	g_tcp_phase_profile.tx_copy_us += elapsed_us;
}
#else
unsigned int rltk_tcp_perf_now_us(void) { return 0; }
void rltk_tcp_perf_rx_complete(unsigned int s, unsigned int b, unsigned int c)
{ (void)s; (void)b; (void)c; }
void rltk_tcp_perf_tx_complete(unsigned int s, unsigned int b)
{ (void)s; (void)b; }
void rltk_tcp_perf_tx_copy(unsigned int b, unsigned int d, unsigned int e)
{ (void)b; (void)d; (void)e; }
#endif

/*
 * This symbol is referenced only by a build-local copy of rtl8195b_recv.o.
 * The original closed WLAN archive remains untouched.  That object has two
 * rtw_memcpy calls: a variable-length RX-ring-to-skb frame copy and a fixed
 * 132-byte metadata copy.  The size gate below keeps the metadata and small
 * packets on the CPU, while frames large enough to amortize setup use RX GDMA.
 *
 * CONFIG_WLAN_RX_RING_GDMA_VERIFY intentionally scans source and destination
 * during board qualification.  Besides detecting a bad transfer, reading the
 * device-written ring before dma_memcpy() makes the HAL's source-cache clean
 * operate on current data.  Disable the verification macro only after the
 * board test has shown no mismatch/recovery and NET_GDMA RX statistics show
 * that the intended traffic is actually using DMA.
 *
 * rtw_memcpy has no error return.  If DMA times out or verification fails, the
 * destination may be partially written, so a complete CPU copy is mandatory
 * before returning it to the closed driver.
 */
#define WLAN_RX_RING_GDMA_MAX_LEN NET_GDMA_MAX_COPY_LEN

static u32 wlan_rx_ring_hash(const void *buffer, u32 len)
{
	const volatile u8 *bytes = (const volatile u8 *)buffer;
	u32 hash = 2166136261U;
	u32 i;

	for (i = 0; i < len; ++i) {
		hash ^= bytes[i];
		hash *= 16777619U;
	}
	return hash;
}

void rtw_rx_ring_memcpy(void *dst, void *src, u32 len)
{
#if !CONFIG_NET_GDMA_COPY
	rtw_memcpy(dst, src, len);
#else
	static u8 first_call = 1;
#if CONFIG_NET_GDMA_STATS || CONFIG_WLAN_RX_RING_GDMA_VERIFY
	static u32 attempts;
#endif
	static u32 recoveries;
#if CONFIG_NET_GDMA_STATS
	static u32 last_report_tick;
#endif
	u32 source_hash = 0;
	int result;

	if (first_call) {
		first_call = 0;
#if CONFIG_NET_GDMA_STATS
		last_report_tick = rtw_get_current_time();
#endif
		printf("[WLAN_RX_GDMA] ring-to-skb redirect active threshold=%u "
		       "max_len=%u verify=%u\n",
		       (unsigned int)NET_GDMA_COPY_THRESHOLD,
		       (unsigned int)WLAN_RX_RING_GDMA_MAX_LEN,
		       (unsigned int)CONFIG_WLAN_RX_RING_GDMA_VERIFY);
	}

	if (len < NET_GDMA_COPY_THRESHOLD ||
	    len > WLAN_RX_RING_GDMA_MAX_LEN) {
		rtw_memcpy(dst, src, len);
		return;
	}

#if CONFIG_WLAN_RX_RING_GDMA_VERIFY
	source_hash = wlan_rx_ring_hash(src, len);
#endif
#if CONFIG_NET_GDMA_STATS || CONFIG_WLAN_RX_RING_GDMA_VERIFY
	attempts++;
#endif
	result = rltk_network_gdma_copy_rx(dst, src, len, NULL);

#if CONFIG_WLAN_RX_RING_GDMA_VERIFY
	if (result == 0 && source_hash != wlan_rx_ring_hash(dst, len)) {
		printf("[WLAN_RX_GDMA][MISMATCH] len=%u attempt=%u; CPU recovery\n",
		       (unsigned int)len, (unsigned int)attempts);
		result = -1;
	}
#endif

	if (result != 0) {
		recoveries++;
		printf("[WLAN_RX_GDMA][RECOVERY] len=%u result=%d count=%u\n",
		       (unsigned int)len, result, (unsigned int)recoveries);
		rtw_memcpy(dst, src, len);
	}

#if CONFIG_NET_GDMA_STATS
	u32 now = rtw_get_current_time();
	if (rtw_systime_to_ms(now - last_report_tick) >= NET_GDMA_REPORT_MS) {
		printf("[WLAN_RX_GDMA][STATS] attempts=%u recoveries=%u verify=%u\n",
		       (unsigned int)attempts, (unsigned int)recoveries,
		       (unsigned int)CONFIG_WLAN_RX_RING_GDMA_VERIFY);
		attempts = 0;
		recoveries = 0;
		last_report_tick = now;
	}
#endif
#endif /* CONFIG_NET_GDMA_COPY */
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
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_NET_GDMA_COPY
		if (net_gdma_copy(&g_net_tx_gdma, skb->tail,
				  (void *)(sg_list->buf), sg_list->len,
				  skb->end, NULL) != 0) {
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
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_NET_GDMA_COPY
			if (net_gdma_copy_with_edges(&g_net_rx_gdma,
						     (void *)(sg_list->buf),
						     skb->data, sg_list->len,
						     NULL, NULL) != 0) {
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

int rltk_wlan_rx_ref_acquire(int idx, unsigned int expected_len,
			     void **handle, void **payload)
{
#if (CONFIG_LWIP_LAYER == 1)
	struct sk_buff *skb;
	struct sk_buff *clone;

	if (handle == NULL || payload == NULL || idx < 0 || expected_len == 0U) {
		return -1;
	}
	*handle = NULL;
	*payload = NULL;

	skb = rltk_wlan_get_recv_skb(idx);
	if (skb == NULL || skb->head == NULL || skb->data == NULL ||
	    skb->tail == NULL || skb->end == NULL || skb->data < skb->head ||
	    skb->tail < skb->data || skb->end < skb->tail ||
	    expected_len > skb->len ||
	    expected_len > (unsigned int)(skb->tail - skb->data)) {
		return -1;
	}

	/*
	 * rtl8195b_recv_tasklet() frees its skb after netif_rx() returns.  A
	 * shallow skb_clone() increments the closed driver's data-buffer
	 * reference count, so a custom lwIP pbuf can retain the payload without
	 * copying it.  Never expose the original skb: its descriptor is owned by
	 * the receive tasklet and is invalid as soon as this callback returns.
	 */
	clone = skb_clone(skb, 0);
	if (clone == NULL) {
		return -2;
	}
	if (clone->data == NULL || clone->len < expected_len ||
	    clone->tail < clone->data ||
	    expected_len > (unsigned int)(clone->tail - clone->data)) {
		kfree_skb(clone);
		return -1;
	}

	*handle = clone;
	*payload = clone->data;
	return 0;
#else
	(void)idx;
	(void)expected_len;
	(void)handle;
	(void)payload;
	return -1;
#endif
}

void rltk_wlan_rx_ref_release(void *handle)
{
#if (CONFIG_LWIP_LAYER == 1)
	if (handle != NULL) {
		/* kfree_skb() drops the shared data reference under its own lock. */
		kfree_skb((struct sk_buff *)handle);
	}
#else
	(void)handle;
#endif
}

int rltk_wlan_rx_ref_selftest(void)
{
#if (CONFIG_LWIP_LAYER == 1)
	struct sk_buff *original;
	struct sk_buff *clone;
	unsigned char *data;
	unsigned int i;
	int ret = 0;

	/*
	 * Run only after real RX traffic starts, when the WLAN skb pools are known
	 * to be initialized.  Freeing the original before checking the clone is
	 * the essential ownership test: it proves the backing data, not merely the
	 * descriptor, remains referenced.  Allocation failure is transient and
	 * asks the caller to retry later rather than disabling zero-copy.
	 */
	original = rltk_wlan_alloc_skb(64U);
	if (original == NULL) {
		return -2;
	}
	data = skb_put(original, 64U);
	for (i = 0; i < 64U; i++) {
		data[i] = (unsigned char)(0xA5U ^ i);
	}

	clone = skb_clone(original, 0);
	if (clone == NULL) {
		kfree_skb(original);
		return -2;
	}
	kfree_skb(original);

	if (clone->data == NULL || clone->len != 64U) {
		ret = -1;
	} else {
		for (i = 0; i < 64U; i++) {
			if (clone->data[i] != (unsigned char)(0xA5U ^ i)) {
				ret = -1;
				break;
			}
		}
	}
	kfree_skb(clone);
	return ret;
#else
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
