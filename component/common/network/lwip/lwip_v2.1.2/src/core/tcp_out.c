/**
 * @file
 * Transmission Control Protocol, outgoing traffic
 *
 * The output functions of TCP.
 *
 * There are two distinct ways for TCP segments to get sent:
 * - queued data: these are segments transferring data or segments containing
 *   SYN or FIN (which both count as one sequence number). They are created as
 *   struct @ref pbuf together with a struct tcp_seg and enqueue to the
 *   unsent list of the pcb. They are sent by tcp_output:
 *   - @ref tcp_write : creates data segments
 *   - @ref tcp_split_unsent_seg : splits a data segment
 *   - @ref tcp_enqueue_flags : creates SYN-only or FIN-only segments
 *   - @ref tcp_output / tcp_output_segment : finalize the tcp header
 *      (e.g. sequence numbers, options, checksum) and output to IP
 *   - the various tcp_rexmit functions shuffle around segments between the
 *     unsent an unacked lists to retransmit them
 *   - tcp_create_segment and tcp_pbuf_prealloc allocate pbuf and
 *     segment for these functions
 * - direct send: these segments don't contain data but control the connection
 *   behaviour. They are created as pbuf only and sent directly without
 *   enqueueing them:
 *   - @ref tcp_send_empty_ack sends an ACK-only segment
 *   - @ref tcp_rst sends a RST segment
 *   - @ref tcp_keepalive sends a keepalive segment
 *   - @ref tcp_zero_window_probe sends a window probe segment
 *   - tcp_output_alloc_header allocates a header-only pbuf for these functions
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

#include "lwip/opt.h"

#if LWIP_TCP /* don't build if not configured for use in lwipopts.h */

#include "lwip/priv/tcp_priv.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/inet_chksum.h"
#include "lwip/stats.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#if defined(CONFIG_PLATFORM_8195BHP)
#include "lwip_intf.h"
#include "cmsis.h"
#include "hal_cache.h"
#include "large_memcpy_gdma.h"
#endif
#if LWIP_TCP_TIMESTAMPS
#include "lwip/sys.h"
#endif

#include <string.h>

#ifndef CONFIG_TCP_TX_LINKED_GDMA
/* Production-safe default: TX pbuf scatter GDMA is enabled only by an explicit
 * board build option.  TCP receive GDMA is controlled independently. */
#define CONFIG_TCP_TX_LINKED_GDMA 0
#endif

#ifndef CONFIG_TCP_TX_LINKED_GDMA_PROFILE
#define CONFIG_TCP_TX_LINKED_GDMA_PROFILE 0
#endif

#ifndef TCP_TX_LINKED_GDMA_THRESHOLD
#define TCP_TX_LINKED_GDMA_THRESHOLD 4096U
#endif

#ifndef TCP_TX_LINKED_GDMA_VERIFY
#define TCP_TX_LINKED_GDMA_VERIFY 0
#endif

#ifndef TCP_TX_LINKED_GDMA_SHADOW
#define TCP_TX_LINKED_GDMA_SHADOW 0
#endif

#ifndef TCP_TX_LINKED_GDMA_DIRECT_BLOCKS
#define TCP_TX_LINKED_GDMA_DIRECT_BLOCKS 0
#endif

#ifndef TCP_TX_LINKED_GDMA_PROBE_BLOCKS
#define TCP_TX_LINKED_GDMA_PROBE_BLOCKS 0
#endif

/* Diagnostic mode, 1-based.  A bounded range of real TCP pbufs becomes one
 * linked-GDMA submission.  Every other real pbuf is populated synchronously
 * by M33, allowing descriptor count and destination position to be varied
 * independently. */
#ifndef TCP_TX_LINKED_GDMA_ISOLATE_BLOCK
#define TCP_TX_LINKED_GDMA_ISOLATE_BLOCK 0
#endif

#ifndef TCP_TX_LINKED_GDMA_ISOLATE_COUNT
#define TCP_TX_LINKED_GDMA_ISOLATE_COUNT 1
#endif

#ifndef TCP_TX_LINKED_GDMA_GUARD_BYTES
#define TCP_TX_LINKED_GDMA_GUARD_BYTES 0
#endif

#define TCP_TX_LINKED_GDMA_MAX_BLOCKS 48U
#define TCP_TX_LINKED_GDMA_SHADOW_SIZE 65535U
#define TCP_TX_LINKED_GDMA_CACHE_LINE 32U
/* heap_4_2 rounds its 8-byte BlockLink_t up to portBYTE_ALIGNMENT.  This
 * platform uses 64-byte heap alignment, so the allocator metadata for a
 * PBUF_RAM allocation occupies the two cache lines immediately before p. */
#define TCP_TX_LINKED_GDMA_HEAP_HEADER_BYTES 64U
#define TCP_TX_LINKED_GDMA_HEAP_ALLOCATED_BIT 0x80000000UL
#define TCP_TX_LINKED_GDMA_PROBE_PRE_BYTES 128U
/* Match the observed heap pbuf layout exactly: p->payload is +0x80, a full
 * TCP segment copy is 0x5a0, and consecutive pbuf metadata starts are 0x6c0
 * apart.  The final 0xa0 bytes emulate guard/alignment/heap spacing. */
#define TCP_TX_LINKED_GDMA_PROBE_PAYLOAD_BYTES 1440U
#define TCP_TX_LINKED_GDMA_PROBE_POST_BYTES 160U
#define TCP_TX_LINKED_GDMA_GUARD_RESERVE \
  (TCP_TX_LINKED_GDMA_GUARD_BYTES + TCP_TX_LINKED_GDMA_CACHE_LINE - 1U)

#if (TCP_TX_LINKED_GDMA_GUARD_BYTES > 0) && \
    ((TCP_TX_LINKED_GDMA_GUARD_BYTES % TCP_TX_LINKED_GDMA_CACHE_LINE) != 0)
#error "TCP_TX_LINKED_GDMA_GUARD_BYTES must be a multiple of 32"
#endif

#if (TCP_TX_LINKED_GDMA_ISOLATE_BLOCK > 0) && \
    ((TCP_TX_LINKED_GDMA_ISOLATE_COUNT == 0) || \
     ((TCP_TX_LINKED_GDMA_ISOLATE_BLOCK + \
       TCP_TX_LINKED_GDMA_ISOLATE_COUNT - 1U) > \
      TCP_TX_LINKED_GDMA_MAX_BLOCKS))
#error "TCP TX GDMA isolate range exceeds descriptor table"
#endif

#if (TCP_TX_LINKED_GDMA_ISOLATE_BLOCK > 0) && \
    ((TCP_TX_LINKED_GDMA_DIRECT_BLOCKS > 0) || \
     (TCP_TX_LINKED_GDMA_PROBE_BLOCKS > 0))
#error "real-pbuf isolation cannot be combined with direct/probe modes"
#endif

#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_TX_LINKED_GDMA
/* tcp_write() executes under the lwIP core lock, and linked GDMA completes
 * synchronously before the newly allocated pbufs are published.  Keep this
 * relatively large descriptor table in BSS rather than on TCP_IP's stack. */
static carbox_gdma_copy_block_t
tcp_tx_linked_gdma_blocks[TCP_TX_LINKED_GDMA_MAX_BLOCKS];
struct tcp_tx_linked_gdma_pbuf_diag {
  const struct pbuf *p;
  const void *payload;
  void *copy_dst;
  u8_t *guard;
  struct pbuf *snapshot_next;
  void *snapshot_payload;
  u16_t copy_len;
  u16_t pbuf_len;
  u16_t snapshot_tot_len;
  u16_t snapshot_len;
  u8_t snapshot_type_internal;
  u8_t snapshot_flags;
  LWIP_PBUF_REF_T snapshot_ref;
  u8_t snapshot_if_idx;
  u8_t *snapshot_heap_header;
  u8_t snapshot_heap_header_bytes[TCP_TX_LINKED_GDMA_HEAP_HEADER_BYTES];
  u8_t *snapshot_cache_line;
  u8_t snapshot_cache_bytes[TCP_TX_LINKED_GDMA_CACHE_LINE];
};
static struct tcp_tx_linked_gdma_pbuf_diag
tcp_tx_linked_gdma_pbuf_diag[TCP_TX_LINKED_GDMA_MAX_BLOCKS];
#if TCP_TX_LINKED_GDMA_SHADOW
/* Diagnostic-only destination.  Real TX pbufs are still populated by M33, so
 * a GDMA/cache defect cannot publish corrupt TCP state during isolation. */
static u8_t tcp_tx_linked_gdma_shadow[TCP_TX_LINKED_GDMA_SHADOW_SIZE];
#endif
#if TCP_TX_LINKED_GDMA_PROBE_BLOCKS > 0
struct tcp_tx_linked_gdma_probe_slot {
  u8_t pre[TCP_TX_LINKED_GDMA_PROBE_PRE_BYTES];
  u8_t payload[TCP_TX_LINKED_GDMA_PROBE_PAYLOAD_BYTES];
  u8_t post[TCP_TX_LINKED_GDMA_PROBE_POST_BYTES];
} __attribute__((aligned(TCP_TX_LINKED_GDMA_CACHE_LINE)));
static struct tcp_tx_linked_gdma_probe_slot
tcp_tx_linked_gdma_probe[TCP_TX_LINKED_GDMA_PROBE_BLOCKS];

static void
tcp_tx_linked_gdma_probe_prepare(u16_t index)
{
  struct tcp_tx_linked_gdma_probe_slot *slot =
    &tcp_tx_linked_gdma_probe[index];

  memset(slot->pre, 0xa6, sizeof(slot->pre));
  memset(slot->post, 0x6a, sizeof(slot->post));
  dcache_clean_by_addr((uint32_t *)slot->pre, (int32_t)sizeof(slot->pre));
  dcache_clean_by_addr((uint32_t *)slot->post, (int32_t)sizeof(slot->post));
  __DSB();
}

static int
tcp_tx_linked_gdma_probe_check(u16_t count)
{
  u16_t index;

  for (index = 0U; index < count; ++index) {
    struct tcp_tx_linked_gdma_probe_slot *slot =
      &tcp_tx_linked_gdma_probe[index];
    u16_t offset;

    dcache_invalidate_by_addr((uint32_t *)slot->pre,
                              (int32_t)sizeof(slot->pre));
    dcache_invalidate_by_addr((uint32_t *)slot->post,
                              (int32_t)sizeof(slot->post));
    __DSB();
    for (offset = 0U; offset < sizeof(slot->pre); ++offset) {
      if (slot->pre[offset] != 0xa6U) {
        LWIP_PLATFORM_DIAG(("[TXGDMA][PROBE][FATAL] block=%u side=pre "
                            "offset=%u value=0x%02x\r\n",
                            (unsigned int)index, (unsigned int)offset,
                            (unsigned int)slot->pre[offset]));
        return 0;
      }
    }
    for (offset = 0U; offset < sizeof(slot->post); ++offset) {
      if (slot->post[offset] != 0x6aU) {
        LWIP_PLATFORM_DIAG(("[TXGDMA][PROBE][FATAL] block=%u side=post "
                            "offset=%u value=0x%02x\r\n",
                            (unsigned int)index, (unsigned int)offset,
                            (unsigned int)slot->post[offset]));
        return 0;
      }
    }
  }
  return 1;
}
#endif
static u8_t tcp_tx_linked_gdma_direct_disabled;

#if TCP_TX_LINKED_GDMA_GUARD_BYTES > 0
static u8_t *
tcp_tx_linked_gdma_guard_start(void *payload, u16_t logical_alloc)
{
  uintptr_t logical_end = (uintptr_t)payload + logical_alloc;

  return (u8_t *)((logical_end + TCP_TX_LINKED_GDMA_CACHE_LINE - 1U) &
                  ~(uintptr_t)(TCP_TX_LINKED_GDMA_CACHE_LINE - 1U));
}

static void
tcp_tx_linked_gdma_guard_clean(u8_t *guard)
{
  dcache_clean_by_addr((uint32_t *)guard,
                       (int32_t)TCP_TX_LINKED_GDMA_GUARD_BYTES);
  __DSB();
}

static void
tcp_tx_linked_gdma_guard_invalidate(u8_t *guard)
{
  dcache_invalidate_by_addr((uint32_t *)guard,
                            (int32_t)TCP_TX_LINKED_GDMA_GUARD_BYTES);
  __DSB();
}
#endif
#endif

#ifdef LWIP_HOOK_FILENAME
#include LWIP_HOOK_FILENAME
#endif

/* Allow to add custom TCP header options by defining this hook */
#ifdef LWIP_HOOK_TCP_OUT_TCPOPT_LENGTH
#define LWIP_TCP_OPT_LENGTH_SEGMENT(flags, pcb) LWIP_HOOK_TCP_OUT_TCPOPT_LENGTH(pcb, LWIP_TCP_OPT_LENGTH(flags))
#else
#define LWIP_TCP_OPT_LENGTH_SEGMENT(flags, pcb) LWIP_TCP_OPT_LENGTH(flags)
#endif

/* Define some copy-macros for checksum-on-copy so that the code looks
   nicer by preventing too many ifdef's. */
#if TCP_CHECKSUM_ON_COPY
#define TCP_DATA_COPY(dst, src, len, seg) do { \
  tcp_seg_add_chksum(LWIP_CHKSUM_COPY(dst, src, len), \
                     len, &seg->chksum, &seg->chksum_swapped); \
  seg->flags |= TF_SEG_DATA_CHECKSUMMED; } while(0)
#define TCP_DATA_COPY2(dst, src, len, chksum, chksum_swapped)  \
  tcp_seg_add_chksum(LWIP_CHKSUM_COPY(dst, src, len), len, chksum, chksum_swapped);
#else /* TCP_CHECKSUM_ON_COPY*/
#if defined(CONFIG_PLATFORM_8195BHP)
static void
tcp_data_copy_gdma(void *dst, const void *src, u16_t len)
{
  unsigned int dma_len = 0;
#if CONFIG_TCP_PHASE_PROFILE
  unsigned int start_us = rltk_tcp_perf_now_us();
#endif

  (void)rltk_network_gdma_copy_tcp_tx(dst, src, len, &dma_len);
#if CONFIG_TCP_PHASE_PROFILE
  rltk_tcp_perf_tx_copy(len, dma_len, rltk_tcp_perf_now_us() - start_us);
#else
  LWIP_UNUSED_ARG(dma_len);
#endif
}
#define TCP_DATA_COPY(dst, src, len, seg)                     tcp_data_copy_gdma(dst, src, len)
#define TCP_DATA_COPY2(dst, src, len, chksum, chksum_swapped) tcp_data_copy_gdma(dst, src, len)
#else
#define TCP_DATA_COPY(dst, src, len, seg)                     MEMCPY(dst, src, len)
#define TCP_DATA_COPY2(dst, src, len, chksum, chksum_swapped) MEMCPY(dst, src, len)
#endif
#endif /* TCP_CHECKSUM_ON_COPY*/

#if CONFIG_TCP_TX_LINKED_GDMA && TCP_CHECKSUM_ON_COPY
#error "TCP TX linked GDMA requires LWIP_CHECKSUM_ON_COPY=0"
#endif

#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_TX_LINKED_GDMA_PROFILE
struct tcp_tx_linked_gdma_stats {
  u32_t window_start_us;
  u32_t calls;
  u32_t attempts;
  u32_t successes;
  u32_t fallbacks;
  u32_t small_cpu;
  u32_t segments;
  u32_t bytes;
  u32_t dma_bytes;
  u32_t cpu_edge_bytes;
  u32_t wait_count;
  u32_t wait_us_total;
  u32_t wait_us_max;
  u32_t cache_clean_us;
  u32_t cache_clean_max_us;
  u32_t dma_wait_us;
  u32_t dma_wait_max_us;
  u32_t cache_invalidate_us;
  u32_t cache_invalidate_max_us;
  u32_t cpu_edge_us;
  u32_t cpu_edge_max_us;
  u32_t verify_failures;
  u32_t pbuf_blocks;
  u32_t dst_aligned32;
  u32_t dst_word_aligned;
  u32_t dst_unaligned;
  u32_t prefix_bytes;
  u32_t prefix_max;
  u32_t body_bytes;
  u32_t edge_bytes;
  u32_t metadata_line_overlap;
  u32_t payload_offset_min;
  u32_t payload_offset_max;
  u32_t src_min;
  u32_t src_max;
  u32_t dst_min;
  u32_t dst_max;
  u32_t same_block_overlap;
  u32_t cross_block_overlap;
  u32_t copy_outside_pbuf;
  u8_t layout_count;
  u32_t layout_p[4];
  u32_t layout_dst[4];
  u32_t layout_end[4];
  u32_t layout_guard[4];
};

static struct tcp_tx_linked_gdma_stats tcp_tx_linked_gdma_stats;
static u32_t tcp_tx_linked_gdma_report_sequence;

static void __attribute__((noinline))
tcp_tx_linked_gdma_profile(u16_t segments, u32_t bytes, int attempted,
                           int success,
                           const carbox_gdma_copyv_result_t *result,
                           int verify_failed)
{
  struct tcp_tx_linked_gdma_stats *stats = &tcp_tx_linked_gdma_stats;
  u32_t now = rltk_tcp_perf_now_us();
  u32_t window;

  if (stats->window_start_us == 0U) {
    stats->window_start_us = now;
  }
  stats->calls++;
  stats->segments += segments;
  stats->bytes += bytes;
  if (attempted) {
    u16_t block_index;

    if (stats->layout_count == 0U) {
      stats->layout_count = (u8_t)LWIP_MIN(segments, 4U);
      for (block_index = 0U; block_index < stats->layout_count;
           ++block_index) {
        const struct tcp_tx_linked_gdma_pbuf_diag *layout =
          &tcp_tx_linked_gdma_pbuf_diag[block_index];

        stats->layout_p[block_index] =
          (u32_t)(uintptr_t)layout->p;
        stats->layout_dst[block_index] =
          (u32_t)(uintptr_t)layout->copy_dst;
        stats->layout_end[block_index] =
          (u32_t)((uintptr_t)layout->copy_dst + layout->copy_len);
        stats->layout_guard[block_index] =
          (u32_t)(uintptr_t)layout->guard;
      }
    }

    stats->attempts++;
    stats->cache_clean_us += result->cache_clean_us;
    stats->dma_wait_us += result->dma_wait_us;
    stats->cache_invalidate_us += result->cache_invalidate_us;
    stats->cpu_edge_us += result->cpu_edge_us;
    stats->cache_clean_max_us = LWIP_MAX(stats->cache_clean_max_us,
                                         result->cache_clean_us);
    stats->dma_wait_max_us = LWIP_MAX(stats->dma_wait_max_us,
                                      result->dma_wait_us);
    stats->cache_invalidate_max_us = LWIP_MAX(
      stats->cache_invalidate_max_us, result->cache_invalidate_us);
    stats->cpu_edge_max_us = LWIP_MAX(stats->cpu_edge_max_us,
                                      result->cpu_edge_us);
    if (success) {
      stats->successes++;
      stats->dma_bytes += result->dma_bytes;
      stats->cpu_edge_bytes += result->cpu_edge_bytes;
    } else {
      stats->fallbacks++;
    }
    if (verify_failed) {
      stats->verify_failures++;
    }
    if (result->channel_waited) {
      stats->wait_count++;
      stats->wait_us_total += result->channel_wait_us;
      if (result->channel_wait_us > stats->wait_us_max) {
        stats->wait_us_max = result->channel_wait_us;
      }
    }
    for (block_index = 0U; block_index < segments; ++block_index) {
      const struct tcp_tx_linked_gdma_pbuf_diag *diag =
        &tcp_tx_linked_gdma_pbuf_diag[block_index];
      uintptr_t dst = (uintptr_t)diag->copy_dst;
      uintptr_t body_start = (dst + 31U) & ~(uintptr_t)31U;
      u32_t prefix = (u32_t)(body_start - dst);
      u32_t body = 0U;
      uintptr_t body_end;
      uintptr_t metadata_start;
      uintptr_t metadata_end;
      u32_t payload_offset;
      uintptr_t src = (uintptr_t)tcp_tx_linked_gdma_blocks[block_index].src;
      uintptr_t src_end = src + diag->copy_len;
      uintptr_t dst_end = dst + diag->copy_len;
      uintptr_t payload = (uintptr_t)diag->payload;
      u16_t other_index;

      stats->pbuf_blocks++;
      if ((dst & 31U) == 0U) {
        stats->dst_aligned32++;
      } else if ((dst & 3U) == 0U) {
        stats->dst_word_aligned++;
      } else {
        stats->dst_unaligned++;
      }
      if (prefix < diag->copy_len) {
        body = ((u32_t)diag->copy_len - prefix) & ~63U;
      } else {
        prefix = diag->copy_len;
      }
      stats->prefix_bytes += prefix;
      if (prefix > stats->prefix_max) {
        stats->prefix_max = prefix;
      }
      stats->body_bytes += body;
      stats->edge_bytes += (u32_t)diag->copy_len - body;

      body_end = body_start + body;
      metadata_start = (uintptr_t)diag->p & ~(uintptr_t)31U;
      metadata_end = ((uintptr_t)diag->p + sizeof(*diag->p) + 31U) &
                     ~(uintptr_t)31U;
      if (body != 0U && body_start < metadata_end &&
          body_end > metadata_start) {
        stats->metadata_line_overlap++;
      }

      payload_offset = (u32_t)((uintptr_t)diag->payload -
                               (uintptr_t)diag->p);
      if (stats->payload_offset_min == 0U ||
          payload_offset < stats->payload_offset_min) {
        stats->payload_offset_min = payload_offset;
      }
      if (payload_offset > stats->payload_offset_max) {
        stats->payload_offset_max = payload_offset;
      }
      if (stats->src_min == 0U || src < stats->src_min) {
        stats->src_min = (u32_t)src;
      }
      if (src_end > stats->src_max) {
        stats->src_max = (u32_t)src_end;
      }
      if (stats->dst_min == 0U || dst < stats->dst_min) {
        stats->dst_min = (u32_t)dst;
      }
      if (dst_end > stats->dst_max) {
        stats->dst_max = (u32_t)dst_end;
      }
      if (src < dst_end && src_end > dst) {
        stats->same_block_overlap++;
      }
      if (dst < payload || dst_end > payload + diag->pbuf_len) {
        stats->copy_outside_pbuf++;
      }
      for (other_index = 0U; other_index < segments; ++other_index) {
        const struct tcp_tx_linked_gdma_pbuf_diag *other =
          &tcp_tx_linked_gdma_pbuf_diag[other_index];
        uintptr_t other_dst;
        uintptr_t other_end;

        if (other_index == block_index) {
          continue;
        }
        other_dst = (uintptr_t)other->copy_dst;
        other_end = other_dst + other->copy_len;
        if (src < other_end && src_end > other_dst) {
          stats->cross_block_overlap++;
          break;
        }
      }
    }
  } else {
    stats->small_cpu++;
  }

  window = now - stats->window_start_us;
  if (window >= 10000000U) {
    LWIP_PLATFORM_DIAG(("[TXGDMA] window_us=%u calls/attempt/success/fallback/small_cpu="
                        "%u/%u/%u/%u/%u segments/bytes=%u/%u "
                        "dma/edge_bytes=%u/%u verify_fail=%u direct_disabled=%u\r\n",
                        window, stats->calls, stats->attempts,
                        stats->successes, stats->fallbacks,
                        stats->small_cpu, stats->segments, stats->bytes,
                        stats->dma_bytes, stats->cpu_edge_bytes,
                        stats->verify_failures,
                        (unsigned int)tcp_tx_linked_gdma_direct_disabled));
    LWIP_PLATFORM_DIAG(("[TXGDMA] channel_wait count=%u wait_us total/avg/max="
                        "%u/%u/%u mechanism=nonblocking-atomic-fastpath\r\n",
                        stats->wait_count, stats->wait_us_total,
                        stats->wait_count != 0U ?
                          stats->wait_us_total / stats->wait_count : 0U,
                        stats->wait_us_max));
    LWIP_PLATFORM_DIAG(("[TXGDMA][PHASE] us total/avg/max "
                        "cache_clean=%u/%u/%u dma_wait=%u/%u/%u "
                        "cache_invalidate=%u/%u/%u cpu_edge=%u/%u/%u\r\n",
                        stats->cache_clean_us,
                        stats->attempts != 0U ?
                          stats->cache_clean_us / stats->attempts : 0U,
                        stats->cache_clean_max_us,
                        stats->dma_wait_us,
                        stats->attempts != 0U ?
                          stats->dma_wait_us / stats->attempts : 0U,
                        stats->dma_wait_max_us,
                        stats->cache_invalidate_us,
                        stats->attempts != 0U ?
                          stats->cache_invalidate_us / stats->attempts : 0U,
                        stats->cache_invalidate_max_us,
                        stats->cpu_edge_us,
                        stats->attempts != 0U ?
                          stats->cpu_edge_us / stats->attempts : 0U,
                        stats->cpu_edge_max_us));
    LWIP_PLATFORM_DIAG(("[TXGDMA][PBUF] blocks=%u dst_align32/word/unaligned="
                        "%u/%u/%u prefix_bytes avg/max=%u/%u "
                        "body/edge_bytes=%u/%u metadata_line_overlap=%u "
                        "payload_minus_p min/max=%u/%u sizeof_pbuf=%u "
                        "direct_blocks_per_write=%u\r\n",
                        stats->pbuf_blocks, stats->dst_aligned32,
                        stats->dst_word_aligned, stats->dst_unaligned,
                        stats->pbuf_blocks != 0U ?
                          stats->prefix_bytes / stats->pbuf_blocks : 0U,
                        stats->prefix_max, stats->body_bytes,
                        stats->edge_bytes, stats->metadata_line_overlap,
                        stats->payload_offset_min, stats->payload_offset_max,
                        (unsigned int)sizeof(struct pbuf),
                        (unsigned int)TCP_TX_LINKED_GDMA_DIRECT_BLOCKS));
    LWIP_PLATFORM_DIAG(("[TXGDMA][ISOLATE] first_real_block=%u (1-based) "
                        "submitted_descriptors=%u\r\n",
                        (unsigned int)TCP_TX_LINKED_GDMA_ISOLATE_BLOCK,
                        TCP_TX_LINKED_GDMA_ISOLATE_BLOCK != 0U ?
                          (unsigned int)TCP_TX_LINKED_GDMA_ISOLATE_COUNT : 0U));
    LWIP_PLATFORM_DIAG(("[TXGDMA][RANGE] src=%08x-%08x dst=%08x-%08x "
                        "shadow=%08x overlap same/cross=%u/%u "
                        "copy_outside_pbuf=%u\r\n",
                        stats->src_min, stats->src_max,
                        stats->dst_min, stats->dst_max,
#if TCP_TX_LINKED_GDMA_SHADOW
                        (unsigned int)(uintptr_t)tcp_tx_linked_gdma_shadow,
#else
                        0U,
#endif
                        stats->same_block_overlap,
                        stats->cross_block_overlap,
                        stats->copy_outside_pbuf));
    LWIP_PLATFORM_DIAG(("[TXGDMA][LAYOUT] n=%u "
                        "b0 p/dst/end/guard=%08x/%08x/%08x/%08x "
                        "b1=%08x/%08x/%08x/%08x "
                        "b2=%08x/%08x/%08x/%08x "
                        "b3=%08x/%08x/%08x/%08x\r\n",
                        stats->layout_count,
                        stats->layout_p[0], stats->layout_dst[0],
                        stats->layout_end[0], stats->layout_guard[0],
                        stats->layout_p[1], stats->layout_dst[1],
                        stats->layout_end[1], stats->layout_guard[1],
                        stats->layout_p[2], stats->layout_dst[2],
                        stats->layout_end[2], stats->layout_guard[2],
                        stats->layout_p[3], stats->layout_dst[3],
                        stats->layout_end[3], stats->layout_guard[3]));

    /* The generic GDMA report used to be emitted only by the optional socket
     * receive profiler.  That profiler is normally disabled while TX is under
     * test, which hid the channel-programming failure behind TX fallback=N.
     * Emit the shared channel snapshot from this same 10-second TX window so
     * the attempted SAR/DAR/LLP/CTL values and the hardware readback are
     * adjacent to the TX success/fallback counters in every test log. */
    carbox_large_memcpy_gdma_report(++tcp_tx_linked_gdma_report_sequence);

    memset(stats, 0, sizeof(*stats));
    stats->window_start_us = now;
  }
}
#else
#define tcp_tx_linked_gdma_profile(segments, bytes, attempted, success, result, verify_failed) \
  do { LWIP_UNUSED_ARG(segments); LWIP_UNUSED_ARG(bytes); \
       LWIP_UNUSED_ARG(attempted); LWIP_UNUSED_ARG(success); \
       LWIP_UNUSED_ARG(result); LWIP_UNUSED_ARG(verify_failed); } while (0)
#endif

/** Define this to 1 for an extra check that the output checksum is valid
 * (usefule when the checksum is generated by the application, not the stack) */
#ifndef TCP_CHECKSUM_ON_COPY_SANITY_CHECK
#define TCP_CHECKSUM_ON_COPY_SANITY_CHECK   0
#endif
/* Allow to override the failure of sanity check from warning to e.g. hard failure */
#if TCP_CHECKSUM_ON_COPY_SANITY_CHECK
#ifndef TCP_CHECKSUM_ON_COPY_SANITY_CHECK_FAIL
#define TCP_CHECKSUM_ON_COPY_SANITY_CHECK_FAIL(msg) LWIP_DEBUGF(TCP_DEBUG | LWIP_DBG_LEVEL_WARNING, msg)
#endif
#endif

#if TCP_OVERSIZE
/** The size of segment pbufs created when TCP_OVERSIZE is enabled */
#ifndef TCP_OVERSIZE_CALC_LENGTH
#define TCP_OVERSIZE_CALC_LENGTH(length) ((length) + TCP_OVERSIZE)
#endif
#endif

/* Forward declarations.*/
static err_t tcp_output_segment(struct tcp_seg *seg, struct tcp_pcb *pcb, struct netif *netif);

#ifndef CONFIG_TCP_OUTPUT_PROFILE
#define CONFIG_TCP_OUTPUT_PROFILE 0
#endif

#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
#define TCP_OUTPUT_PROFILE_RETURN(value, category) do { \
  err_t profile_return_value = (value); \
  rltk_tcp_output_profile_end((category), profile_segments, profile_bytes, \
                              DWT->CYCCNT - profile_start_cycles); \
  return profile_return_value; \
} while (0)
#else
#define TCP_OUTPUT_PROFILE_RETURN(value, category) return (value)
#endif

/* tcp_route: common code that returns a fixed bound netif or calls ip_route */
static struct netif *
tcp_route(const struct tcp_pcb *pcb, const ip_addr_t *src, const ip_addr_t *dst)
{
  LWIP_UNUSED_ARG(src); /* in case IPv4-only and source-based routing is disabled */

  if ((pcb != NULL) && (pcb->netif_idx != NETIF_NO_INDEX)) {
    return netif_get_by_index(pcb->netif_idx);
  } else {
    return ip_route(src, dst);
  }
}

/**
 * Create a TCP segment with prefilled header.
 *
 * Called by @ref tcp_write, @ref tcp_enqueue_flags and @ref tcp_split_unsent_seg
 *
 * @param pcb Protocol control block for the TCP connection.
 * @param p pbuf that is used to hold the TCP header.
 * @param hdrflags TCP flags for header.
 * @param seqno TCP sequence number of this packet
 * @param optflags options to include in TCP header
 * @return a new tcp_seg pointing to p, or NULL.
 * The TCP header is filled in except ackno and wnd.
 * p is freed on failure.
 */
static struct tcp_seg *
tcp_create_segment(const struct tcp_pcb *pcb, struct pbuf *p, u8_t hdrflags, u32_t seqno, u8_t optflags)
{
  struct tcp_seg *seg;
  u8_t optlen;

  LWIP_ASSERT("tcp_create_segment: invalid pcb", pcb != NULL);
  LWIP_ASSERT("tcp_create_segment: invalid pbuf", p != NULL);

  optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(optflags, pcb);

  if ((seg = (struct tcp_seg *)memp_malloc(MEMP_TCP_SEG)) == NULL) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("tcp_create_segment: no memory.\n"));
    pbuf_free(p);
    return NULL;
  }
  seg->flags = optflags;
  seg->next = NULL;
  seg->p = p;
  LWIP_ASSERT("p->tot_len >= optlen", p->tot_len >= optlen);
  seg->len = p->tot_len - optlen;
#if TCP_OVERSIZE_DBGCHECK
  seg->oversize_left = 0;
#endif /* TCP_OVERSIZE_DBGCHECK */
#if TCP_CHECKSUM_ON_COPY
  seg->chksum = 0;
  seg->chksum_swapped = 0;
  /* check optflags */
  LWIP_ASSERT("invalid optflags passed: TF_SEG_DATA_CHECKSUMMED",
              (optflags & TF_SEG_DATA_CHECKSUMMED) == 0);
#endif /* TCP_CHECKSUM_ON_COPY */

  /* build TCP header */
  if (pbuf_add_header(p, TCP_HLEN)) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("tcp_create_segment: no room for TCP header in pbuf.\n"));
    TCP_STATS_INC(tcp.err);
    tcp_seg_free(seg);
    return NULL;
  }
  seg->tcphdr = (struct tcp_hdr *)seg->p->payload;
  seg->tcphdr->src = lwip_htons(pcb->local_port);
  seg->tcphdr->dest = lwip_htons(pcb->remote_port);
  seg->tcphdr->seqno = lwip_htonl(seqno);
  /* ackno is set in tcp_output */
  TCPH_HDRLEN_FLAGS_SET(seg->tcphdr, (5 + optlen / 4), hdrflags);
  /* wnd and chksum are set in tcp_output */
  seg->tcphdr->urgp = 0;
  return seg;
}

/**
 * Allocate a PBUF_RAM pbuf, perhaps with extra space at the end.
 *
 * This function is like pbuf_alloc(layer, length, PBUF_RAM) except
 * there may be extra bytes available at the end.
 *
 * Called by @ref tcp_write
 *
 * @param layer flag to define header size.
 * @param length size of the pbuf's payload.
 * @param max_length maximum usable size of payload+oversize.
 * @param oversize pointer to a u16_t that will receive the number of usable tail bytes.
 * @param pcb The TCP connection that will enqueue the pbuf.
 * @param apiflags API flags given to tcp_write.
 * @param first_seg true when this pbuf will be used in the first enqueued segment.
 */
#if TCP_OVERSIZE
static struct pbuf *
tcp_pbuf_prealloc(pbuf_layer layer, u16_t length, u16_t max_length,
                  u16_t *oversize, const struct tcp_pcb *pcb, u8_t apiflags,
                  u8_t first_seg)
{
  struct pbuf *p;
  u16_t alloc = length;
  u16_t logical_alloc;

  LWIP_ASSERT("tcp_pbuf_prealloc: invalid oversize", oversize != NULL);
  LWIP_ASSERT("tcp_pbuf_prealloc: invalid pcb", pcb != NULL);

#if LWIP_NETIF_TX_SINGLE_PBUF
  LWIP_UNUSED_ARG(max_length);
  LWIP_UNUSED_ARG(pcb);
  LWIP_UNUSED_ARG(apiflags);
  LWIP_UNUSED_ARG(first_seg);
  alloc = max_length;
#else /* LWIP_NETIF_TX_SINGLE_PBUF */
  if (length < max_length) {
    /* Should we allocate an oversized pbuf, or just the minimum
     * length required? If tcp_write is going to be called again
     * before this segment is transmitted, we want the oversized
     * buffer. If the segment will be transmitted immediately, we can
     * save memory by allocating only length. We use a simple
     * heuristic based on the following information:
     *
     * Did the user set TCP_WRITE_FLAG_MORE?
     *
     * Will the Nagle algorithm defer transmission of this segment?
     */
    if ((apiflags & TCP_WRITE_FLAG_MORE) ||
        (!(pcb->flags & TF_NODELAY) &&
         (!first_seg ||
          pcb->unsent != NULL ||
          pcb->unacked != NULL))) {
      alloc = LWIP_MIN(max_length, LWIP_MEM_ALIGN_SIZE(TCP_OVERSIZE_CALC_LENGTH(length)));
    }
  }
#endif /* LWIP_NETIF_TX_SINGLE_PBUF */
  logical_alloc = alloc;
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_TX_LINKED_GDMA && \
    (TCP_TX_LINKED_GDMA_GUARD_BYTES > 0)
  LWIP_ASSERT("tcp_pbuf_prealloc: guard allocation overflow",
              alloc <= (u16_t)(0xffffU -
                                TCP_TX_LINKED_GDMA_GUARD_RESERVE));
  alloc = (u16_t)(alloc + TCP_TX_LINKED_GDMA_GUARD_RESERVE);
#endif
  p = pbuf_alloc(layer, alloc, PBUF_RAM);
  if (p == NULL) {
    return NULL;
  }
  LWIP_ASSERT("need unchained pbuf", p->next == NULL);
  *oversize = logical_alloc - length;
  /* trim p->len to the currently used size */
  p->len = p->tot_len = length;
  return p;
}
#else /* TCP_OVERSIZE */
#define tcp_pbuf_prealloc(layer, length, mx, os, pcb, api, fst) pbuf_alloc((layer), (length), PBUF_RAM)
#endif /* TCP_OVERSIZE */

#if TCP_CHECKSUM_ON_COPY
/** Add a checksum of newly added data to the segment.
 *
 * Called by tcp_write and tcp_split_unsent_seg.
 */
static void
tcp_seg_add_chksum(u16_t chksum, u16_t len, u16_t *seg_chksum,
                   u8_t *seg_chksum_swapped)
{
  u32_t helper;
  /* add chksum to old chksum and fold to u16_t */
  helper = chksum + *seg_chksum;
  chksum = FOLD_U32T(helper);
  if ((len & 1) != 0) {
    *seg_chksum_swapped = 1 - *seg_chksum_swapped;
    chksum = SWAP_BYTES_IN_WORD(chksum);
  }
  *seg_chksum = chksum;
}
#endif /* TCP_CHECKSUM_ON_COPY */

/** Checks if tcp_write is allowed or not (checks state, snd_buf and snd_queuelen).
 *
 * @param pcb the tcp pcb to check for
 * @param len length of data to send (checked agains snd_buf)
 * @return ERR_OK if tcp_write is allowed to proceed, another err_t otherwise
 */
static err_t
tcp_write_checks(struct tcp_pcb *pcb, u16_t len)
{
  LWIP_ASSERT("tcp_write_checks: invalid pcb", pcb != NULL);

  /* connection is in invalid state for data transmission? */
  if ((pcb->state != ESTABLISHED) &&
      (pcb->state != CLOSE_WAIT) &&
      (pcb->state != SYN_SENT) &&
      (pcb->state != SYN_RCVD)) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_STATE | LWIP_DBG_LEVEL_SEVERE, ("tcp_write() called in invalid state\n"));
    return ERR_CONN;
  } else if (len == 0) {
    return ERR_OK;
  }

  /* fail on too much data */
  if (len > pcb->snd_buf) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_LEVEL_SEVERE, ("tcp_write: too much data (len=%"U16_F" > snd_buf=%"TCPWNDSIZE_F")\n",
                len, pcb->snd_buf));
    tcp_set_flags(pcb, TF_NAGLEMEMERR);
    return ERR_MEM;
  }

  LWIP_DEBUGF(TCP_QLEN_DEBUG, ("tcp_write: queuelen: %"TCPWNDSIZE_F"\n", (tcpwnd_size_t)pcb->snd_queuelen));

  /* If total number of pbufs on the unsent/unacked queues exceeds the
   * configured maximum, return an error */
  /* check for configured max queuelen and possible overflow */
  if (pcb->snd_queuelen >= LWIP_MIN(TCP_SND_QUEUELEN, (TCP_SNDQUEUELEN_OVERFLOW + 1))) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_LEVEL_SEVERE, ("tcp_write: too long queue %"U16_F" (max %"U16_F")\n",
                pcb->snd_queuelen, (u16_t)TCP_SND_QUEUELEN));
    TCP_STATS_INC(tcp.memerr);
    tcp_set_flags(pcb, TF_NAGLEMEMERR);
    return ERR_MEM;
  }
  if (pcb->snd_queuelen != 0) {
    LWIP_ASSERT("tcp_write: pbufs on queue => at least one queue non-empty",
                pcb->unacked != NULL || pcb->unsent != NULL);
  } else {
    LWIP_ASSERT("tcp_write: no pbufs on queue => both queues empty",
                pcb->unacked == NULL && pcb->unsent == NULL);
  }
  return ERR_OK;
}

/**
 * @ingroup tcp_raw
 * Write data for sending (but does not send it immediately).
 *
 * It waits in the expectation of more data being sent soon (as
 * it can send them more efficiently by combining them together).
 * To prompt the system to send data now, call tcp_output() after
 * calling tcp_write().
 * 
 * This function enqueues the data pointed to by the argument dataptr. The length of
 * the data is passed as the len parameter. The apiflags can be one or more of:
 * - TCP_WRITE_FLAG_COPY: indicates whether the new memory should be allocated
 *   for the data to be copied into. If this flag is not given, no new memory
 *   should be allocated and the data should only be referenced by pointer. This
 *   also means that the memory behind dataptr must not change until the data is
 *   ACKed by the remote host
 * - TCP_WRITE_FLAG_MORE: indicates that more data follows. If this is omitted,
 *   the PSH flag is set in the last segment created by this call to tcp_write.
 *   If this flag is given, the PSH flag is not set.
 *
 * The tcp_write() function will fail and return ERR_MEM if the length
 * of the data exceeds the current send buffer size or if the length of
 * the queue of outgoing segment is larger than the upper limit defined
 * in lwipopts.h. The number of bytes available in the output queue can
 * be retrieved with the tcp_sndbuf() function.
 *
 * The proper way to use this function is to call the function with at
 * most tcp_sndbuf() bytes of data. If the function returns ERR_MEM,
 * the application should wait until some of the currently enqueued
 * data has been successfully received by the other host and try again.
 *
 * @param pcb Protocol control block for the TCP connection to enqueue data for.
 * @param arg Pointer to the data to be enqueued for sending.
 * @param len Data length in bytes
 * @param apiflags combination of following flags :
 * - TCP_WRITE_FLAG_COPY (0x01) data will be copied into memory belonging to the stack
 * - TCP_WRITE_FLAG_MORE (0x02) for TCP connection, PSH flag will not be set on last segment sent,
 * @return ERR_OK if enqueued, another err_t on error
 */
err_t
tcp_write(struct tcp_pcb *pcb, const void *arg, u16_t len, u8_t apiflags)
{
  struct pbuf *concat_p = NULL;
  struct tcp_seg *last_unsent = NULL, *seg = NULL, *prev_seg = NULL, *queue = NULL;
  u16_t pos = 0; /* position in 'arg' data */
  u16_t queuelen;
  u8_t optlen;
  u8_t optflags = 0;
#if TCP_OVERSIZE
  u16_t oversize = 0;
  u16_t oversize_used = 0;
#if TCP_OVERSIZE_DBGCHECK
  u16_t oversize_add = 0;
#endif /* TCP_OVERSIZE_DBGCHECK*/
#endif /* TCP_OVERSIZE */
  u16_t extendlen = 0;
#if TCP_CHECKSUM_ON_COPY
  u16_t concat_chksum = 0;
  u8_t concat_chksum_swapped = 0;
  u16_t concat_chksummed = 0;
#endif /* TCP_CHECKSUM_ON_COPY */
  err_t err;
  u16_t mss_local;
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_TX_LINKED_GDMA
  carbox_gdma_copyv_result_t tx_gdma_result;
  u32_t tx_gdma_bytes = 0U;
  u16_t tx_gdma_block_count = 0U;
  u8_t tx_gdma_defer = 0U;
#endif
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_PHASE_PROFILE
  unsigned int tcp_perf_start_us;
#endif

  LWIP_ERROR("tcp_write: invalid pcb", pcb != NULL, return ERR_ARG);

  /* don't allocate segments bigger than half the maximum window we ever received */
  mss_local = LWIP_MIN(pcb->mss, TCPWND_MIN16(pcb->snd_wnd_max / 2));
  mss_local = mss_local ? mss_local : pcb->mss;

  LWIP_ASSERT_CORE_LOCKED();

#if LWIP_NETIF_TX_SINGLE_PBUF
  /* Always copy to try to create single pbufs for TX */
  apiflags |= TCP_WRITE_FLAG_COPY;
#endif /* LWIP_NETIF_TX_SINGLE_PBUF */

#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_TX_LINKED_GDMA
  tx_gdma_defer = ((apiflags & TCP_WRITE_FLAG_COPY) != 0U) &&
                  (len > TCP_TX_LINKED_GDMA_THRESHOLD);
  memset(&tx_gdma_result, 0, sizeof(tx_gdma_result));
#endif

  LWIP_DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_write(pcb=%p, data=%p, len=%"U16_F", apiflags=%"U16_F")\n",
                                 (void *)pcb, arg, len, (u16_t)apiflags));
  LWIP_ERROR("tcp_write: arg == NULL (programmer violates API)",
             arg != NULL, return ERR_ARG;);

  err = tcp_write_checks(pcb, len);
  if (err != ERR_OK) {
    return err;
  }
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_PHASE_PROFILE
  tcp_perf_start_us = rltk_tcp_perf_now_us();
#endif
  queuelen = pcb->snd_queuelen;

#if LWIP_TCP_TIMESTAMPS
  if ((pcb->flags & TF_TIMESTAMP)) {
    /* Make sure the timestamp option is only included in data segments if we
       agreed about it with the remote host. */
    optflags = TF_SEG_OPTS_TS;
    optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(TF_SEG_OPTS_TS, pcb);
    /* ensure that segments can hold at least one data byte... */
    mss_local = LWIP_MAX(mss_local, LWIP_TCP_OPT_LEN_TS + 1);
  } else
#endif /* LWIP_TCP_TIMESTAMPS */
  {
    optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(0, pcb);
  }


  /*
   * TCP segmentation is done in three phases with increasing complexity:
   *
   * 1. Copy data directly into an oversized pbuf.
   * 2. Chain a new pbuf to the end of pcb->unsent.
   * 3. Create new segments.
   *
   * We may run out of memory at any point. In that case we must
   * return ERR_MEM and not change anything in pcb. Therefore, all
   * changes are recorded in local variables and committed at the end
   * of the function. Some pcb fields are maintained in local copies:
   *
   * queuelen = pcb->snd_queuelen
   * oversize = pcb->unsent_oversize
   *
   * These variables are set consistently by the phases:
   *
   * seg points to the last segment tampered with.
   *
   * pos records progress as data is segmented.
   */

  /* Find the tail of the unsent queue. */
  if (pcb->unsent != NULL) {
    u16_t space;
    u16_t unsent_optlen;

    /* @todo: this could be sped up by keeping last_unsent in the pcb */
    for (last_unsent = pcb->unsent; last_unsent->next != NULL;
         last_unsent = last_unsent->next);

    /* Usable space at the end of the last unsent segment */
    unsent_optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(last_unsent->flags, pcb);
    LWIP_ASSERT("mss_local is too small", mss_local >= last_unsent->len + unsent_optlen);
    space = mss_local - (last_unsent->len + unsent_optlen);

    /*
     * Phase 1: Copy data directly into an oversized pbuf.
     *
     * The number of bytes copied is recorded in the oversize_used
     * variable. The actual copying is done at the bottom of the
     * function.
     */
#if TCP_OVERSIZE
#if TCP_OVERSIZE_DBGCHECK
    /* check that pcb->unsent_oversize matches last_unsent->oversize_left */
    LWIP_ASSERT("unsent_oversize mismatch (pcb vs. last_unsent)",
                pcb->unsent_oversize == last_unsent->oversize_left);
#endif /* TCP_OVERSIZE_DBGCHECK */
    oversize = pcb->unsent_oversize;
    if (oversize > 0) {
      LWIP_ASSERT("inconsistent oversize vs. space", oversize <= space);
      seg = last_unsent;
      oversize_used = LWIP_MIN(space, LWIP_MIN(oversize, len));
      pos += oversize_used;
      oversize -= oversize_used;
      space -= oversize_used;
    }
    /* now we are either finished or oversize is zero */
    LWIP_ASSERT("inconsistent oversize vs. len", (oversize == 0) || (pos == len));
#endif /* TCP_OVERSIZE */

#if !LWIP_NETIF_TX_SINGLE_PBUF
    /*
     * Phase 2: Chain a new pbuf to the end of pcb->unsent.
     *
     * As an exception when NOT copying the data, if the given data buffer
     * directly follows the last unsent data buffer in memory, extend the last
     * ROM pbuf reference to the buffer, thus saving a ROM pbuf allocation.
     *
     * We don't extend segments containing SYN/FIN flags or options
     * (len==0). The new pbuf is kept in concat_p and pbuf_cat'ed at
     * the end.
     *
     * This phase is skipped for LWIP_NETIF_TX_SINGLE_PBUF as we could only execute
     * it after rexmit puts a segment from unacked to unsent and at this point,
     * oversize info is lost.
     */
    if ((pos < len) && (space > 0) && (last_unsent->len > 0)) {
      u16_t seglen = LWIP_MIN(space, len - pos);
      seg = last_unsent;

      /* Create a pbuf with a copy or reference to seglen bytes. We
       * can use PBUF_RAW here since the data appears in the middle of
       * a segment. A header will never be prepended. */
      if (apiflags & TCP_WRITE_FLAG_COPY) {
        /* Data is copied */
        if ((concat_p = tcp_pbuf_prealloc(PBUF_RAW, seglen, space, &oversize, pcb, apiflags, 1)) == NULL) {
          LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
                      ("tcp_write : could not allocate memory for pbuf copy size %"U16_F"\n",
                       seglen));
          goto memerr;
        }
#if TCP_OVERSIZE_DBGCHECK
        oversize_add = oversize;
#endif /* TCP_OVERSIZE_DBGCHECK */
        TCP_DATA_COPY2(concat_p->payload, (const u8_t *)arg + pos, seglen, &concat_chksum, &concat_chksum_swapped);
#if TCP_CHECKSUM_ON_COPY
        concat_chksummed += seglen;
#endif /* TCP_CHECKSUM_ON_COPY */
        queuelen += pbuf_clen(concat_p);
      } else {
        /* Data is not copied */
        /* If the last unsent pbuf is of type PBUF_ROM, try to extend it. */
        struct pbuf *p;
        for (p = last_unsent->p; p->next != NULL; p = p->next);
        if (((p->type_internal & (PBUF_TYPE_FLAG_STRUCT_DATA_CONTIGUOUS | PBUF_TYPE_FLAG_DATA_VOLATILE)) == 0) &&
            (const u8_t *)p->payload + p->len == (const u8_t *)arg) {
          LWIP_ASSERT("tcp_write: ROM pbufs cannot be oversized", pos == 0);
          extendlen = seglen;
        } else {
          if ((concat_p = pbuf_alloc(PBUF_RAW, seglen, PBUF_ROM)) == NULL) {
            LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
                        ("tcp_write: could not allocate memory for zero-copy pbuf\n"));
            goto memerr;
          }
          /* reference the non-volatile payload data */
          ((struct pbuf_rom *)concat_p)->payload = (const u8_t *)arg + pos;
          queuelen += pbuf_clen(concat_p);
        }
#if TCP_CHECKSUM_ON_COPY
        /* calculate the checksum of nocopy-data */
        tcp_seg_add_chksum(~inet_chksum((const u8_t *)arg + pos, seglen), seglen,
                           &concat_chksum, &concat_chksum_swapped);
        concat_chksummed += seglen;
#endif /* TCP_CHECKSUM_ON_COPY */
      }

      pos += seglen;
    }
#endif /* !LWIP_NETIF_TX_SINGLE_PBUF */
  } else {
#if TCP_OVERSIZE
    LWIP_ASSERT("unsent_oversize mismatch (pcb->unsent is NULL)",
                pcb->unsent_oversize == 0);
#endif /* TCP_OVERSIZE */
  }

  /*
   * Phase 3: Create new segments.
   *
   * The new segments are chained together in the local 'queue'
   * variable, ready to be appended to pcb->unsent.
   */
  while (pos < len) {
    struct pbuf *p;
    u16_t left = len - pos;
    u16_t max_len = mss_local - optlen;
    u16_t seglen = LWIP_MIN(left, max_len);
#if TCP_CHECKSUM_ON_COPY
    u16_t chksum = 0;
    u8_t chksum_swapped = 0;
#endif /* TCP_CHECKSUM_ON_COPY */

    if (apiflags & TCP_WRITE_FLAG_COPY) {
      /* If copy is set, memory should be allocated and data copied
       * into pbuf */
      if ((p = tcp_pbuf_prealloc(PBUF_TRANSPORT, seglen + optlen, mss_local, &oversize, pcb, apiflags, queue == NULL)) == NULL) {
        LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("tcp_write : could not allocate memory for pbuf copy size %"U16_F"\n", seglen));
        goto memerr;
      }
      LWIP_ASSERT("tcp_write: check that first pbuf can hold the complete seglen",
                  (p->len >= seglen));
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_TX_LINKED_GDMA
      if (tx_gdma_defer &&
          tx_gdma_block_count < TCP_TX_LINKED_GDMA_MAX_BLOCKS) {
        carbox_gdma_copy_block_t *block =
          &tcp_tx_linked_gdma_blocks[tx_gdma_block_count++];

        block->dst =
#if TCP_TX_LINKED_GDMA_SHADOW
#if TCP_TX_LINKED_GDMA_ISOLATE_BLOCK > 0
          !tcp_tx_linked_gdma_direct_disabled &&
          tx_gdma_block_count >= TCP_TX_LINKED_GDMA_ISOLATE_BLOCK &&
          tx_gdma_block_count < (TCP_TX_LINKED_GDMA_ISOLATE_BLOCK +
                                 TCP_TX_LINKED_GDMA_ISOLATE_COUNT) ?
            (char *)p->payload + optlen :
#endif
#if TCP_TX_LINKED_GDMA_PROBE_BLOCKS > 0
          !tcp_tx_linked_gdma_direct_disabled &&
          tx_gdma_block_count <= TCP_TX_LINKED_GDMA_PROBE_BLOCKS &&
          seglen <= TCP_TX_LINKED_GDMA_PROBE_PAYLOAD_BYTES ?
            (tcp_tx_linked_gdma_probe_prepare(
               (u16_t)(tx_gdma_block_count - 1U)),
             tcp_tx_linked_gdma_probe[tx_gdma_block_count - 1U].payload) :
#endif
          !tcp_tx_linked_gdma_direct_disabled &&
          tx_gdma_block_count <= TCP_TX_LINKED_GDMA_DIRECT_BLOCKS ?
            (char *)p->payload + optlen :
            tcp_tx_linked_gdma_shadow + pos;
#else
          (char *)p->payload + optlen;
#endif
        block->src = (const u8_t *)arg + pos;
        block->len = seglen;
        tcp_tx_linked_gdma_pbuf_diag[tx_gdma_block_count - 1U].p = p;
        tcp_tx_linked_gdma_pbuf_diag[tx_gdma_block_count - 1U].payload =
          p->payload;
        tcp_tx_linked_gdma_pbuf_diag[tx_gdma_block_count - 1U].copy_dst =
          (char *)p->payload + optlen;
        tcp_tx_linked_gdma_pbuf_diag[tx_gdma_block_count - 1U].copy_len =
          seglen;
        tcp_tx_linked_gdma_pbuf_diag[tx_gdma_block_count - 1U].pbuf_len =
          p->len;
#if TCP_TX_LINKED_GDMA_GUARD_BYTES > 0
        tcp_tx_linked_gdma_pbuf_diag[tx_gdma_block_count - 1U].guard =
          NULL;
#if TCP_TX_LINKED_GDMA_ISOLATE_BLOCK > 0
        if (tx_gdma_block_count >= TCP_TX_LINKED_GDMA_ISOLATE_BLOCK &&
            tx_gdma_block_count < (TCP_TX_LINKED_GDMA_ISOLATE_BLOCK +
                                   TCP_TX_LINKED_GDMA_ISOLATE_COUNT)) {
#else
        if (tx_gdma_block_count <= TCP_TX_LINKED_GDMA_DIRECT_BLOCKS) {
#endif
          tcp_tx_linked_gdma_pbuf_diag[tx_gdma_block_count - 1U].guard =
            tcp_tx_linked_gdma_guard_start(p->payload,
                                           (u16_t)(p->len + oversize));
          memset(tcp_tx_linked_gdma_pbuf_diag[tx_gdma_block_count - 1U].guard,
                 0xa5, TCP_TX_LINKED_GDMA_GUARD_BYTES);
          /* Publish the canary to DRAM before GDMA starts.  Without this
           * clean, a write-through can remain hidden behind a dirty A5 line. */
          tcp_tx_linked_gdma_guard_clean(
            tcp_tx_linked_gdma_pbuf_diag[tx_gdma_block_count - 1U].guard);
        }
#else
        tcp_tx_linked_gdma_pbuf_diag[tx_gdma_block_count - 1U].guard = NULL;
#endif
        tx_gdma_bytes += seglen;
#if TCP_TX_LINKED_GDMA_SHADOW
#if TCP_TX_LINKED_GDMA_ISOLATE_BLOCK > 0
        if (tcp_tx_linked_gdma_direct_disabled ||
            tx_gdma_block_count < TCP_TX_LINKED_GDMA_ISOLATE_BLOCK ||
            tx_gdma_block_count >= (TCP_TX_LINKED_GDMA_ISOLATE_BLOCK +
                                    TCP_TX_LINKED_GDMA_ISOLATE_COUNT)) {
#else
        if (tcp_tx_linked_gdma_direct_disabled ||
            tx_gdma_block_count > TCP_TX_LINKED_GDMA_DIRECT_BLOCKS ||
            TCP_TX_LINKED_GDMA_PROBE_BLOCKS > 0) {
#endif
          /* Except for the deliberately tiny direct-DMA test set, keep the
           * production destination on the proven M33 path. */
          TCP_DATA_COPY2((char *)p->payload + optlen,
                         (const u8_t *)arg + pos, seglen,
                         &chksum, &chksum_swapped);
        }
#endif
      } else {
        u16_t copy_index;

        /* An unexpectedly small negotiated MSS can exceed the fixed local
         * descriptor table.  Complete every deferred block by CPU before
         * returning to the original immediate-copy path. */
        if (tx_gdma_defer) {
          for (copy_index = 0U; copy_index < tx_gdma_block_count;
               ++copy_index) {
            TCP_DATA_COPY2(tcp_tx_linked_gdma_blocks[copy_index].dst,
                           tcp_tx_linked_gdma_blocks[copy_index].src,
                           tcp_tx_linked_gdma_blocks[copy_index].len,
                           NULL, NULL);
          }
          tx_gdma_defer = 0U;
          tx_gdma_block_count = 0U;
          tx_gdma_bytes = 0U;
        }
        TCP_DATA_COPY2((char *)p->payload + optlen,
                       (const u8_t *)arg + pos, seglen,
                       &chksum, &chksum_swapped);
      }
#else
      TCP_DATA_COPY2((char *)p->payload + optlen, (const u8_t *)arg + pos, seglen, &chksum, &chksum_swapped);
#endif
    } else {
      /* Copy is not set: First allocate a pbuf for holding the data.
       * Since the referenced data is available at least until it is
       * sent out on the link (as it has to be ACKed by the remote
       * party) we can safely use PBUF_ROM instead of PBUF_REF here.
       */
      struct pbuf *p2;
#if TCP_OVERSIZE
      LWIP_ASSERT("oversize == 0", oversize == 0);
#endif /* TCP_OVERSIZE */
      if ((p2 = pbuf_alloc(PBUF_TRANSPORT, seglen, PBUF_ROM)) == NULL) {
        LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("tcp_write: could not allocate memory for zero-copy pbuf\n"));
        goto memerr;
      }
#if TCP_CHECKSUM_ON_COPY
      /* calculate the checksum of nocopy-data */
      chksum = ~inet_chksum((const u8_t *)arg + pos, seglen);
      if (seglen & 1) {
        chksum_swapped = 1;
        chksum = SWAP_BYTES_IN_WORD(chksum);
      }
#endif /* TCP_CHECKSUM_ON_COPY */
      /* reference the non-volatile payload data */
      ((struct pbuf_rom *)p2)->payload = (const u8_t *)arg + pos;

      /* Second, allocate a pbuf for the headers. */
      if ((p = pbuf_alloc(PBUF_TRANSPORT, optlen, PBUF_RAM)) == NULL) {
        /* If allocation fails, we have to deallocate the data pbuf as
         * well. */
        pbuf_free(p2);
        LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("tcp_write: could not allocate memory for header pbuf\n"));
        goto memerr;
      }
      /* Concatenate the headers and data pbufs together. */
      pbuf_cat(p/*header*/, p2/*data*/);
    }

    queuelen += pbuf_clen(p);

    /* Now that there are more segments queued, we check again if the
     * length of the queue exceeds the configured maximum or
     * overflows. */
    if (queuelen > LWIP_MIN(TCP_SND_QUEUELEN, TCP_SNDQUEUELEN_OVERFLOW)) {
      LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("tcp_write: queue too long %"U16_F" (%d)\n",
                  queuelen, (int)TCP_SND_QUEUELEN));
      pbuf_free(p);
      goto memerr;
    }

    if ((seg = tcp_create_segment(pcb, p, 0, pcb->snd_lbb + pos, optflags)) == NULL) {
      goto memerr;
    }
#if TCP_OVERSIZE_DBGCHECK
    seg->oversize_left = oversize;
#endif /* TCP_OVERSIZE_DBGCHECK */
#if TCP_CHECKSUM_ON_COPY
    seg->chksum = chksum;
    seg->chksum_swapped = chksum_swapped;
    seg->flags |= TF_SEG_DATA_CHECKSUMMED;
#endif /* TCP_CHECKSUM_ON_COPY */

    /* first segment of to-be-queued data? */
    if (queue == NULL) {
      queue = seg;
    } else {
      /* Attach the segment to the end of the queued segments */
      LWIP_ASSERT("prev_seg != NULL", prev_seg != NULL);
      prev_seg->next = seg;
    }
    /* remember last segment of to-be-queued data for next iteration */
    prev_seg = seg;

    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_TRACE, ("tcp_write: queueing %"U32_F":%"U32_F"\n",
                lwip_ntohl(seg->tcphdr->seqno),
                lwip_ntohl(seg->tcphdr->seqno) + TCP_TCPLEN(seg)));

    pos += seglen;
  }

#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_TX_LINKED_GDMA
  if (tx_gdma_block_count != 0U) {
    int tx_gdma_attempted =
      tx_gdma_bytes > TCP_TX_LINKED_GDMA_THRESHOLD
#if TCP_TX_LINKED_GDMA_ISOLATE_BLOCK > 0
      && tx_gdma_block_count >= (TCP_TX_LINKED_GDMA_ISOLATE_BLOCK +
                                 TCP_TX_LINKED_GDMA_ISOLATE_COUNT - 1U)
#endif
      ;
    int tx_gdma_success = 0;
    int tx_gdma_verify_failed = 0;

    if (tx_gdma_attempted) {
      u32_t copy_start_us = rltk_tcp_perf_now_us();
      u16_t snapshot_index;

      for (snapshot_index = 0U; snapshot_index < tx_gdma_block_count;
           ++snapshot_index) {
        struct tcp_tx_linked_gdma_pbuf_diag *diag =
          &tcp_tx_linked_gdma_pbuf_diag[snapshot_index];
        const struct pbuf *p = diag->p;

        diag->snapshot_next = p->next;
        diag->snapshot_payload = p->payload;
        diag->snapshot_tot_len = p->tot_len;
        diag->snapshot_len = p->len;
        diag->snapshot_type_internal = p->type_internal;
        diag->snapshot_flags = p->flags;
        diag->snapshot_ref = p->ref;
        diag->snapshot_if_idx = p->if_idx;
        diag->snapshot_heap_header = NULL;
        diag->snapshot_cache_line = NULL;
#if TCP_TX_LINKED_GDMA_ISOLATE_BLOCK > 0
        {
          const u16_t isolate_index =
            (u16_t)(TCP_TX_LINKED_GDMA_ISOLATE_BLOCK - 1U);
          const u16_t snapshot_first = isolate_index > 0U ?
            (u16_t)(isolate_index - 1U) : isolate_index;
          const u16_t snapshot_last = LWIP_MIN(
            tx_gdma_block_count,
            (u16_t)(isolate_index + TCP_TX_LINKED_GDMA_ISOLATE_COUNT + 1U));

          if (snapshot_index >= snapshot_first &&
              snapshot_index < snapshot_last) {
            uintptr_t line = (uintptr_t)p &
              ~(uintptr_t)(TCP_TX_LINKED_GDMA_CACHE_LINE - 1U);
            uintptr_t heap_header = (uintptr_t)p -
              TCP_TX_LINKED_GDMA_HEAP_HEADER_BYTES;
            const u32_t *heap_words = (const u32_t *)heap_header;

            diag->snapshot_heap_header = (u8_t *)heap_header;
            diag->snapshot_cache_line = (u8_t *)line;
            dcache_clean_by_addr((uint32_t *)heap_header,
                                 TCP_TX_LINKED_GDMA_HEAP_HEADER_BYTES);
            dcache_clean_by_addr((uint32_t *)line,
                                 TCP_TX_LINKED_GDMA_CACHE_LINE);
            __DSB();
            memcpy(diag->snapshot_heap_header_bytes,
                   (const void *)heap_header,
                   TCP_TX_LINKED_GDMA_HEAP_HEADER_BYTES);
            memcpy(diag->snapshot_cache_bytes, (const void *)line,
                   TCP_TX_LINKED_GDMA_CACHE_LINE);

            /* An allocated heap_4_2 block has a NULL free-list link and the
             * top bit set in xBlockSize.  If this is already false, GDMA is
             * not the cause; keep the pbuf on the CPU path and report the
             * pre-existing lifetime/heap fault without touching hardware. */
            if ((heap_words[0] != 0U) ||
                ((heap_words[1] & TCP_TX_LINKED_GDMA_HEAP_ALLOCATED_BIT) == 0U)) {
              LWIP_PLATFORM_DIAG(("[TXGDMA][HEAP-HEADER][PREEXISTING] "
                                  "block=%u p=%p hdr=%p next=%p size=0x%08x; "
                                  "skipping direct GDMA\r\n",
                                  (unsigned int)snapshot_index, (void *)p,
                                  (void *)heap_header,
                                  (void *)(uintptr_t)heap_words[0],
                                  (unsigned int)heap_words[1]));
              tcp_tx_linked_gdma_direct_disabled = 1U;
            }
          }
        }
#endif
      }

      /* First validation stage must not add a new blocking dependency to the
       * TCP core.  If both linked channels are occupied, use the CPU path. */
#if TCP_TX_LINKED_GDMA_ISOLATE_BLOCK > 0
      if (!tcp_tx_linked_gdma_direct_disabled) {
        static u8_t isolate_addr_reported;
        const u16_t first = TCP_TX_LINKED_GDMA_ISOLATE_BLOCK - 1U;
        const struct tcp_tx_linked_gdma_pbuf_diag *first_diag =
          &tcp_tx_linked_gdma_pbuf_diag[first];
        const struct tcp_tx_linked_gdma_pbuf_diag *last_diag =
          &tcp_tx_linked_gdma_pbuf_diag[first +
                                        TCP_TX_LINKED_GDMA_ISOLATE_COUNT - 1U];

        if (!isolate_addr_reported) {
          isolate_addr_reported = 1U;
          LWIP_PLATFORM_DIAG(("[TXGDMA][ISOLATE-ADDR] first block=%u p=%p "
                              "hdr=%p last block=%u p=%p hdr=%p\r\n",
                              (unsigned int)first, (void *)first_diag->p,
                              (void *)first_diag->snapshot_heap_header,
                              (unsigned int)(first +
                                TCP_TX_LINKED_GDMA_ISOLATE_COUNT - 1U),
                              (void *)last_diag->p,
                              (void *)last_diag->snapshot_heap_header));
        }
        tx_gdma_success = carbox_linked_gdma_copyv_force_try(
          &tcp_tx_linked_gdma_blocks[first],
          TCP_TX_LINKED_GDMA_ISOLATE_COUNT, &tx_gdma_result);
      }
#else
      tx_gdma_success = carbox_linked_gdma_copyv_try(
        tcp_tx_linked_gdma_blocks, tx_gdma_block_count, &tx_gdma_result);
#endif

#if TCP_TX_LINKED_GDMA_PROBE_BLOCKS > 0
      if (tx_gdma_success && !tcp_tx_linked_gdma_direct_disabled) {
        u16_t probe_count = LWIP_MIN(tx_gdma_block_count,
                                    TCP_TX_LINKED_GDMA_PROBE_BLOCKS);

        if (!tcp_tx_linked_gdma_probe_check(probe_count)) {
          tcp_tx_linked_gdma_direct_disabled = 1U;
          tx_gdma_success = 0;
        }
      }
#endif

      if (tx_gdma_success && !tcp_tx_linked_gdma_direct_disabled) {
        u16_t guard_index;
#if TCP_TX_LINKED_GDMA_ISOLATE_BLOCK > 0
        const u16_t isolate_index =
          (u16_t)(TCP_TX_LINKED_GDMA_ISOLATE_BLOCK - 1U);
        const u16_t validate_first = isolate_index > 0U ?
          (u16_t)(isolate_index - 1U) : isolate_index;
        const u16_t validate_last = LWIP_MIN(
          tx_gdma_block_count,
          (u16_t)(isolate_index + TCP_TX_LINKED_GDMA_ISOLATE_COUNT + 1U));
#else
        const u16_t validate_first = 0U;
        const u16_t validate_last = LWIP_MIN(
          tx_gdma_block_count, (u16_t)TCP_TX_LINKED_GDMA_DIRECT_BLOCKS);
#endif

        for (guard_index = validate_first;
             guard_index < validate_last;
             ++guard_index) {
          struct tcp_tx_linked_gdma_pbuf_diag *diag =
            &tcp_tx_linked_gdma_pbuf_diag[guard_index];
          struct pbuf *p = (struct pbuf *)diag->p;
          u32_t guard_offset;

          if (diag->snapshot_heap_header != NULL) {
            u32_t raw_offset;

            dcache_invalidate_by_addr(
              (uint32_t *)diag->snapshot_heap_header,
              TCP_TX_LINKED_GDMA_HEAP_HEADER_BYTES);
            __DSB();
            for (raw_offset = 0U;
                 raw_offset < TCP_TX_LINKED_GDMA_HEAP_HEADER_BYTES;
                 ++raw_offset) {
              if (diag->snapshot_heap_header[raw_offset] ==
                  diag->snapshot_heap_header_bytes[raw_offset]) {
                continue;
              }
              LWIP_PLATFORM_DIAG(("[TXGDMA][HEAP-HEADER][FATAL] block=%u "
                                  "p=%p hdr=%p offset=%u value=0x%02x/0x%02x; "
                                  "restoring header and disabling direct GDMA\r\n",
                                  guard_index, (void *)p,
                                  diag->snapshot_heap_header,
                                  (unsigned int)raw_offset,
                                  (unsigned int)
                                    diag->snapshot_heap_header[raw_offset],
                                  (unsigned int)
                                    diag->snapshot_heap_header_bytes[raw_offset]));
              memcpy(diag->snapshot_heap_header,
                     diag->snapshot_heap_header_bytes,
                     TCP_TX_LINKED_GDMA_HEAP_HEADER_BYTES);
              dcache_clean_by_addr(
                (uint32_t *)diag->snapshot_heap_header,
                TCP_TX_LINKED_GDMA_HEAP_HEADER_BYTES);
              __DSB();
              tcp_tx_linked_gdma_direct_disabled = 1U;
              tx_gdma_success = 0;
              break;
            }
            if (!tx_gdma_success) {
              break;
            }
          }

          if (diag->snapshot_cache_line != NULL) {
            u32_t raw_offset;

            dcache_invalidate_by_addr(
              (uint32_t *)diag->snapshot_cache_line,
              TCP_TX_LINKED_GDMA_CACHE_LINE);
            __DSB();
            for (raw_offset = 0U;
                 raw_offset < TCP_TX_LINKED_GDMA_CACHE_LINE;
                 ++raw_offset) {
              if (diag->snapshot_cache_line[raw_offset] ==
                  diag->snapshot_cache_bytes[raw_offset]) {
                continue;
              }
              LWIP_PLATFORM_DIAG(("[TXGDMA][PBUF-LINE][FATAL] block=%u "
                                  "p=%p line=%p offset=%u value=0x%02x/0x%02x; "
                                  "restoring line and disabling direct GDMA\r\n",
                                  guard_index, (void *)p,
                                  diag->snapshot_cache_line,
                                  (unsigned int)raw_offset,
                                  (unsigned int)
                                    diag->snapshot_cache_line[raw_offset],
                                  (unsigned int)
                                    diag->snapshot_cache_bytes[raw_offset]));
              memcpy(diag->snapshot_cache_line,
                     diag->snapshot_cache_bytes,
                     TCP_TX_LINKED_GDMA_CACHE_LINE);
              dcache_clean_by_addr(
                (uint32_t *)diag->snapshot_cache_line,
                TCP_TX_LINKED_GDMA_CACHE_LINE);
              __DSB();
              tcp_tx_linked_gdma_direct_disabled = 1U;
              tx_gdma_success = 0;
              break;
            }
            if (!tx_gdma_success) {
              break;
            }
          }

#if TCP_TX_LINKED_GDMA_GUARD_BYTES > 0
          /* Read the post-GDMA DRAM contents, not the pre-DMA cached canary.
           * guard is cache-line aligned and occupies dedicated reserve bytes,
           * so this cannot discard payload or heap-header state. */
          if (diag->guard != NULL) {
            tcp_tx_linked_gdma_guard_invalidate(diag->guard);
          }
#endif
          int metadata_changed =
            p->next != diag->snapshot_next ||
            p->payload != diag->snapshot_payload ||
            p->tot_len != diag->snapshot_tot_len ||
            p->len != diag->snapshot_len ||
            p->type_internal != diag->snapshot_type_internal ||
            p->flags != diag->snapshot_flags ||
            p->ref != diag->snapshot_ref ||
            p->if_idx != diag->snapshot_if_idx;

          if (metadata_changed) {
            LWIP_PLATFORM_DIAG(("[TXGDMA][GUARD][FATAL] block=%u p=%p "
                                "metadata changed next=%p/%p payload=%p/%p "
                                "tot=%u/%u len=%u/%u ref=%u/%u; "
                                "disabling direct GDMA\r\n",
                                guard_index, (void *)p,
                                (void *)p->next, (void *)diag->snapshot_next,
                                p->payload, diag->snapshot_payload,
                                p->tot_len, diag->snapshot_tot_len,
                                p->len, diag->snapshot_len,
                                (unsigned int)p->ref,
                                (unsigned int)diag->snapshot_ref));
            p->next = diag->snapshot_next;
            p->payload = diag->snapshot_payload;
            p->tot_len = diag->snapshot_tot_len;
            p->len = diag->snapshot_len;
            p->type_internal = diag->snapshot_type_internal;
            p->flags = diag->snapshot_flags;
            p->ref = diag->snapshot_ref;
            p->if_idx = diag->snapshot_if_idx;
            tcp_tx_linked_gdma_direct_disabled = 1U;
            tx_gdma_success = 0;
            break;
          }
#if TCP_TX_LINKED_GDMA_GUARD_BYTES > 0
          if (diag->guard != NULL) {
            for (guard_offset = 0U;
                 guard_offset < TCP_TX_LINKED_GDMA_GUARD_BYTES;
                 ++guard_offset) {
              if (diag->guard[guard_offset] == 0xa5U) {
                continue;
              }
              LWIP_PLATFORM_DIAG(("[TXGDMA][GUARD][FATAL] block=%u p=%p "
                                  "write-through offset=%u value=0x%02x; "
                                  "disabling direct GDMA\r\n",
                                  guard_index, (void *)p,
                                  (unsigned int)guard_offset,
                                  (unsigned int)diag->guard[guard_offset]));
              tcp_tx_linked_gdma_direct_disabled = 1U;
              tx_gdma_success = 0;
              break;
            }
          }
          if (!tx_gdma_success) {
            break;
          }
#else
          LWIP_UNUSED_ARG(guard_offset);
#endif
        }
      }

#if TCP_TX_LINKED_GDMA_VERIFY
      if (tx_gdma_success) {
        u16_t verify_index;

#if TCP_TX_LINKED_GDMA_ISOLATE_BLOCK > 0
        for (verify_index = TCP_TX_LINKED_GDMA_ISOLATE_BLOCK - 1U;
             verify_index < (TCP_TX_LINKED_GDMA_ISOLATE_BLOCK - 1U +
                             TCP_TX_LINKED_GDMA_ISOLATE_COUNT);
             ++verify_index) {
#else
        for (verify_index = 0U; verify_index < tx_gdma_block_count;
             ++verify_index) {
#endif
          const carbox_gdma_copy_block_t *block =
            &tcp_tx_linked_gdma_blocks[verify_index];

          if (memcmp(block->dst, block->src, block->len) != 0) {
            tx_gdma_verify_failed = 1;
            tx_gdma_success = 0;
            LWIP_PLATFORM_DIAG(("[TXGDMA][VERIFY] mismatch block=%u/%u "
                                "len=%u; recopying complete write with M33\r\n",
                                verify_index, tx_gdma_block_count,
                                (unsigned int)block->len));
            break;
          }
        }
      }
#endif

      if (tx_gdma_success) {
#if CONFIG_TCP_PHASE_PROFILE
        rltk_tcp_perf_tx_copy(tx_gdma_bytes, tx_gdma_result.dma_bytes,
                              rltk_tcp_perf_now_us() - copy_start_us);
#endif
      }
    }
    if (!tx_gdma_success) {
      u16_t copy_index;

      /* The helper quiesces a failed channel before returning.  In production
       * mode recopy every destination so a partial DMA transaction can never
       * be published.  Shadow mode already populated real pbufs with M33; this
       * loop only makes the diagnostic shadow complete. */
      for (copy_index = 0U; copy_index < tx_gdma_block_count;
           ++copy_index) {
        TCP_DATA_COPY2(tcp_tx_linked_gdma_blocks[copy_index].dst,
                       tcp_tx_linked_gdma_blocks[copy_index].src,
                       tcp_tx_linked_gdma_blocks[copy_index].len,
                       NULL, NULL);
      }
    }
    tcp_tx_linked_gdma_profile(tx_gdma_block_count, tx_gdma_bytes,
                               tx_gdma_attempted,
                               tx_gdma_success, &tx_gdma_result,
                               tx_gdma_verify_failed);
  } else {
    tcp_tx_linked_gdma_profile(0U, len, 0, 0, &tx_gdma_result, 0);
  }
#endif

  /*
   * All three segmentation phases were successful. We can commit the
   * transaction.
   */
#if TCP_OVERSIZE_DBGCHECK
  if ((last_unsent != NULL) && (oversize_add != 0)) {
    last_unsent->oversize_left += oversize_add;
  }
#endif /* TCP_OVERSIZE_DBGCHECK */

  /*
   * Phase 1: If data has been added to the preallocated tail of
   * last_unsent, we update the length fields of the pbuf chain.
   */
#if TCP_OVERSIZE
  if (oversize_used > 0) {
    struct pbuf *p;
    /* Bump tot_len of whole chain, len of tail */
    for (p = last_unsent->p; p; p = p->next) {
      p->tot_len += oversize_used;
      if (p->next == NULL) {
        TCP_DATA_COPY((char *)p->payload + p->len, arg, oversize_used, last_unsent);
        p->len += oversize_used;
      }
    }
    last_unsent->len += oversize_used;
#if TCP_OVERSIZE_DBGCHECK
    LWIP_ASSERT("last_unsent->oversize_left >= oversize_used",
                last_unsent->oversize_left >= oversize_used);
    last_unsent->oversize_left -= oversize_used;
#endif /* TCP_OVERSIZE_DBGCHECK */
  }
  pcb->unsent_oversize = oversize;
#endif /* TCP_OVERSIZE */

  /*
   * Phase 2: concat_p can be concatenated onto last_unsent->p, unless we
   * determined that the last ROM pbuf can be extended to include the new data.
   */
  if (concat_p != NULL) {
    LWIP_ASSERT("tcp_write: cannot concatenate when pcb->unsent is empty",
                (last_unsent != NULL));
    pbuf_cat(last_unsent->p, concat_p);
    last_unsent->len += concat_p->tot_len;
  } else if (extendlen > 0) {
    struct pbuf *p;
    LWIP_ASSERT("tcp_write: extension of reference requires reference",
                last_unsent != NULL && last_unsent->p != NULL);
    for (p = last_unsent->p; p->next != NULL; p = p->next) {
      p->tot_len += extendlen;
    }
    p->tot_len += extendlen;
    p->len += extendlen;
    last_unsent->len += extendlen;
  }

#if TCP_CHECKSUM_ON_COPY
  if (concat_chksummed) {
    LWIP_ASSERT("tcp_write: concat checksum needs concatenated data",
                concat_p != NULL || extendlen > 0);
    /*if concat checksumm swapped - swap it back */
    if (concat_chksum_swapped) {
      concat_chksum = SWAP_BYTES_IN_WORD(concat_chksum);
    }
    tcp_seg_add_chksum(concat_chksum, concat_chksummed, &last_unsent->chksum,
                       &last_unsent->chksum_swapped);
    last_unsent->flags |= TF_SEG_DATA_CHECKSUMMED;
  }
#endif /* TCP_CHECKSUM_ON_COPY */

  /*
   * Phase 3: Append queue to pcb->unsent. Queue may be NULL, but that
   * is harmless
   */
  if (last_unsent == NULL) {
    pcb->unsent = queue;
  } else {
    last_unsent->next = queue;
  }

  /*
   * Finally update the pcb state.
   */
  pcb->snd_lbb += len;
  pcb->snd_buf -= len;
  pcb->snd_queuelen = queuelen;

  LWIP_DEBUGF(TCP_QLEN_DEBUG, ("tcp_write: %"S16_F" (after enqueued)\n",
                               pcb->snd_queuelen));
  if (pcb->snd_queuelen != 0) {
    LWIP_ASSERT("tcp_write: valid queue length",
                pcb->unacked != NULL || pcb->unsent != NULL);
  }

  /* Set the PSH flag in the last segment that we enqueued. */
  if (seg != NULL && seg->tcphdr != NULL && ((apiflags & TCP_WRITE_FLAG_MORE) == 0)) {
    TCPH_SET_FLAG(seg->tcphdr, TCP_PSH);
  }

#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_PHASE_PROFILE
  rltk_tcp_perf_tx_complete(tcp_perf_start_us, len);
#endif
  return ERR_OK;
memerr:
  tcp_set_flags(pcb, TF_NAGLEMEMERR);
  TCP_STATS_INC(tcp.memerr);

  if (concat_p != NULL) {
    pbuf_free(concat_p);
  }
  if (queue != NULL) {
    tcp_segs_free(queue);
  }
  if (pcb->snd_queuelen != 0) {
    LWIP_ASSERT("tcp_write: valid queue length", pcb->unacked != NULL ||
                pcb->unsent != NULL);
  }
  LWIP_DEBUGF(TCP_QLEN_DEBUG | LWIP_DBG_STATE, ("tcp_write: %"S16_F" (with mem err)\n", pcb->snd_queuelen));
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_PHASE_PROFILE
  rltk_tcp_perf_tx_complete(tcp_perf_start_us, len);
#endif
  return ERR_MEM;
}

/**
 * Split segment on the head of the unsent queue.  If return is not
 * ERR_OK, existing head remains intact
 *
 * The split is accomplished by creating a new TCP segment and pbuf
 * which holds the remainder payload after the split.  The original
 * pbuf is trimmed to new length.  This allows splitting of read-only
 * pbufs
 *
 * @param pcb the tcp_pcb for which to split the unsent head
 * @param split the amount of payload to remain in the head
 */
err_t
tcp_split_unsent_seg(struct tcp_pcb *pcb, u16_t split)
{
  struct tcp_seg *seg = NULL, *useg = NULL;
  struct pbuf *p = NULL;
  u8_t optlen;
  u8_t optflags;
  u8_t split_flags;
  u8_t remainder_flags;
  u16_t remainder;
  u16_t offset;
#if TCP_CHECKSUM_ON_COPY
  u16_t chksum = 0;
  u8_t chksum_swapped = 0;
  struct pbuf *q;
#endif /* TCP_CHECKSUM_ON_COPY */

  LWIP_ASSERT("tcp_split_unsent_seg: invalid pcb", pcb != NULL);

  useg = pcb->unsent;
  if (useg == NULL) {
    return ERR_MEM;
  }

  if (split == 0) {
    LWIP_ASSERT("Can't split segment into length 0", 0);
    return ERR_VAL;
  }

  if (useg->len <= split) {
    return ERR_OK;
  }

  LWIP_ASSERT("split <= mss", split <= pcb->mss);
  LWIP_ASSERT("useg->len > 0", useg->len > 0);

  /* We should check that we don't exceed TCP_SND_QUEUELEN but we need
   * to split this packet so we may actually exceed the max value by
   * one!
   */
  LWIP_DEBUGF(TCP_QLEN_DEBUG, ("tcp_enqueue: split_unsent_seg: %u\n", (unsigned int)pcb->snd_queuelen));

  optflags = useg->flags;
#if TCP_CHECKSUM_ON_COPY
  /* Remove since checksum is not stored until after tcp_create_segment() */
  optflags &= ~TF_SEG_DATA_CHECKSUMMED;
#endif /* TCP_CHECKSUM_ON_COPY */
  optlen = LWIP_TCP_OPT_LENGTH(optflags);
  remainder = useg->len - split;

  /* Create new pbuf for the remainder of the split */
  p = pbuf_alloc(PBUF_TRANSPORT, remainder + optlen, PBUF_RAM);
  if (p == NULL) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
                ("tcp_split_unsent_seg: could not allocate memory for pbuf remainder %u\n", remainder));
    goto memerr;
  }

  /* Offset into the original pbuf is past TCP/IP headers, options, and split amount */
  offset = useg->p->tot_len - useg->len + split;
  /* Copy remainder into new pbuf, headers and options will not be filled out */
  if (pbuf_copy_partial(useg->p, (u8_t *)p->payload + optlen, remainder, offset ) != remainder) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
                ("tcp_split_unsent_seg: could not copy pbuf remainder %u\n", remainder));
    goto memerr;
  }
#if TCP_CHECKSUM_ON_COPY
  /* calculate the checksum on remainder data */
  tcp_seg_add_chksum(~inet_chksum((const u8_t *)p->payload + optlen, remainder), remainder,
                     &chksum, &chksum_swapped);
#endif /* TCP_CHECKSUM_ON_COPY */

  /* Options are created when calling tcp_output() */

  /* Migrate flags from original segment */
  split_flags = TCPH_FLAGS(useg->tcphdr);
  remainder_flags = 0; /* ACK added in tcp_output() */

  if (split_flags & TCP_PSH) {
    split_flags &= ~TCP_PSH;
    remainder_flags |= TCP_PSH;
  }
  if (split_flags & TCP_FIN) {
    split_flags &= ~TCP_FIN;
    remainder_flags |= TCP_FIN;
  }
  /* SYN should be left on split, RST should not be present with data */

  seg = tcp_create_segment(pcb, p, remainder_flags, lwip_ntohl(useg->tcphdr->seqno) + split, optflags);
  if (seg == NULL) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
                ("tcp_split_unsent_seg: could not create new TCP segment\n"));
    goto memerr;
  }

#if TCP_CHECKSUM_ON_COPY
  seg->chksum = chksum;
  seg->chksum_swapped = chksum_swapped;
  seg->flags |= TF_SEG_DATA_CHECKSUMMED;
#endif /* TCP_CHECKSUM_ON_COPY */

  /* Remove this segment from the queue since trimming it may free pbufs */
  pcb->snd_queuelen -= pbuf_clen(useg->p);

  /* Trim the original pbuf into our split size.  At this point our remainder segment must be setup
  successfully because we are modifying the original segment */
  pbuf_realloc(useg->p, useg->p->tot_len - remainder);
  useg->len -= remainder;
  TCPH_SET_FLAG(useg->tcphdr, split_flags);
#if TCP_OVERSIZE_DBGCHECK
  /* By trimming, realloc may have actually shrunk the pbuf, so clear oversize_left */
  useg->oversize_left = 0;
#endif /* TCP_OVERSIZE_DBGCHECK */

  /* Add back to the queue with new trimmed pbuf */
  pcb->snd_queuelen += pbuf_clen(useg->p);

#if TCP_CHECKSUM_ON_COPY
  /* The checksum on the split segment is now incorrect. We need to re-run it over the split */
  useg->chksum = 0;
  useg->chksum_swapped = 0;
  q = useg->p;
  offset = q->tot_len - useg->len; /* Offset due to exposed headers */

  /* Advance to the pbuf where the offset ends */
  while (q != NULL && offset > q->len) {
    offset -= q->len;
    q = q->next;
  }
  LWIP_ASSERT("Found start of payload pbuf", q != NULL);
  /* Checksum the first payload pbuf accounting for offset, then other pbufs are all payload */
  for (; q != NULL; offset = 0, q = q->next) {
    tcp_seg_add_chksum(~inet_chksum((const u8_t *)q->payload + offset, q->len - offset), q->len - offset,
                       &useg->chksum, &useg->chksum_swapped);
  }
#endif /* TCP_CHECKSUM_ON_COPY */

  /* Update number of segments on the queues. Note that length now may
   * exceed TCP_SND_QUEUELEN! We don't have to touch pcb->snd_buf
   * because the total amount of data is constant when packet is split */
  pcb->snd_queuelen += pbuf_clen(seg->p);

  /* Finally insert remainder into queue after split (which stays head) */
  seg->next = useg->next;
  useg->next = seg;

#if TCP_OVERSIZE
  /* If remainder is last segment on the unsent, ensure we clear the oversize amount
   * because the remainder is always sized to the exact remaining amount */
  if (seg->next == NULL) {
    pcb->unsent_oversize = 0;
  }
#endif /* TCP_OVERSIZE */

  return ERR_OK;
memerr:
  TCP_STATS_INC(tcp.memerr);

  LWIP_ASSERT("seg == NULL", seg == NULL);
  if (p != NULL) {
    pbuf_free(p);
  }

  return ERR_MEM;
}

/**
 * Called by tcp_close() to send a segment including FIN flag but not data.
 * This FIN may be added to an existing segment or a new, otherwise empty
 * segment is enqueued.
 *
 * @param pcb the tcp_pcb over which to send a segment
 * @return ERR_OK if sent, another err_t otherwise
 */
err_t
tcp_send_fin(struct tcp_pcb *pcb)
{
  LWIP_ASSERT("tcp_send_fin: invalid pcb", pcb != NULL);

  /* first, try to add the fin to the last unsent segment */
  if (pcb->unsent != NULL) {
    struct tcp_seg *last_unsent;
    for (last_unsent = pcb->unsent; last_unsent->next != NULL;
         last_unsent = last_unsent->next);

    if ((TCPH_FLAGS(last_unsent->tcphdr) & (TCP_SYN | TCP_FIN | TCP_RST)) == 0) {
      /* no SYN/FIN/RST flag in the header, we can add the FIN flag */
      TCPH_SET_FLAG(last_unsent->tcphdr, TCP_FIN);
      tcp_set_flags(pcb, TF_FIN);
      return ERR_OK;
    }
  }
  /* no data, no length, flags, copy=1, no optdata */
  return tcp_enqueue_flags(pcb, TCP_FIN);
}

/**
 * Enqueue SYN or FIN for transmission.
 *
 * Called by @ref tcp_connect, tcp_listen_input, and @ref tcp_close
 * (via @ref tcp_send_fin)
 *
 * @param pcb Protocol control block for the TCP connection.
 * @param flags TCP header flags to set in the outgoing segment.
 */
err_t
tcp_enqueue_flags(struct tcp_pcb *pcb, u8_t flags)
{
  struct pbuf *p;
  struct tcp_seg *seg;
  u8_t optflags = 0;
  u8_t optlen = 0;

  LWIP_DEBUGF(TCP_QLEN_DEBUG, ("tcp_enqueue_flags: queuelen: %"U16_F"\n", (u16_t)pcb->snd_queuelen));

  LWIP_ASSERT("tcp_enqueue_flags: need either TCP_SYN or TCP_FIN in flags (programmer violates API)",
              (flags & (TCP_SYN | TCP_FIN)) != 0);
  LWIP_ASSERT("tcp_enqueue_flags: invalid pcb", pcb != NULL);

  /* No need to check pcb->snd_queuelen if only SYN or FIN are allowed! */

  /* Get options for this segment. This is a special case since this is the
     only place where a SYN can be sent. */
  if (flags & TCP_SYN) {
    optflags = TF_SEG_OPTS_MSS;
#if LWIP_WND_SCALE
    if ((pcb->state != SYN_RCVD) || (pcb->flags & TF_WND_SCALE)) {
      /* In a <SYN,ACK> (sent in state SYN_RCVD), the window scale option may only
         be sent if we received a window scale option from the remote host. */
      optflags |= TF_SEG_OPTS_WND_SCALE;
    }
#endif /* LWIP_WND_SCALE */
#if LWIP_TCP_SACK_OUT
    if ((pcb->state != SYN_RCVD) || (pcb->flags & TF_SACK)) {
      /* In a <SYN,ACK> (sent in state SYN_RCVD), the SACK_PERM option may only
         be sent if we received a SACK_PERM option from the remote host. */
      optflags |= TF_SEG_OPTS_SACK_PERM;
    }
#endif /* LWIP_TCP_SACK_OUT */
  }
#if LWIP_TCP_TIMESTAMPS
  if ((pcb->flags & TF_TIMESTAMP) || ((flags & TCP_SYN) && (pcb->state != SYN_RCVD))) {
    /* Make sure the timestamp option is only included in data segments if we
       agreed about it with the remote host (and in active open SYN segments). */
    optflags |= TF_SEG_OPTS_TS;
  }
#endif /* LWIP_TCP_TIMESTAMPS */
  optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(optflags, pcb);

  /* Allocate pbuf with room for TCP header + options */
  if ((p = pbuf_alloc(PBUF_TRANSPORT, optlen, PBUF_RAM)) == NULL) {
    tcp_set_flags(pcb, TF_NAGLEMEMERR);
    TCP_STATS_INC(tcp.memerr);
    return ERR_MEM;
  }
  LWIP_ASSERT("tcp_enqueue_flags: check that first pbuf can hold optlen",
              (p->len >= optlen));

  /* Allocate memory for tcp_seg, and fill in fields. */
  if ((seg = tcp_create_segment(pcb, p, flags, pcb->snd_lbb, optflags)) == NULL) {
    tcp_set_flags(pcb, TF_NAGLEMEMERR);
    TCP_STATS_INC(tcp.memerr);
    return ERR_MEM;
  }
  LWIP_ASSERT("seg->tcphdr not aligned", ((mem_ptr_t)seg->tcphdr % LWIP_MIN(MEM_ALIGNMENT, 4)) == 0);
  LWIP_ASSERT("tcp_enqueue_flags: invalid segment length", seg->len == 0);

  LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_TRACE,
              ("tcp_enqueue_flags: queueing %"U32_F":%"U32_F" (0x%"X16_F")\n",
               lwip_ntohl(seg->tcphdr->seqno),
               lwip_ntohl(seg->tcphdr->seqno) + TCP_TCPLEN(seg),
               (u16_t)flags));

  /* Now append seg to pcb->unsent queue */
  if (pcb->unsent == NULL) {
    pcb->unsent = seg;
  } else {
    struct tcp_seg *useg;
    for (useg = pcb->unsent; useg->next != NULL; useg = useg->next);
    useg->next = seg;
  }
#if TCP_OVERSIZE
  /* The new unsent tail has no space */
  pcb->unsent_oversize = 0;
#endif /* TCP_OVERSIZE */

  /* SYN and FIN bump the sequence number */
  if ((flags & TCP_SYN) || (flags & TCP_FIN)) {
    pcb->snd_lbb++;
    /* optlen does not influence snd_buf */
  }
  if (flags & TCP_FIN) {
    tcp_set_flags(pcb, TF_FIN);
  }

  /* update number of segments on the queues */
  pcb->snd_queuelen += pbuf_clen(seg->p);
  LWIP_DEBUGF(TCP_QLEN_DEBUG, ("tcp_enqueue_flags: %"S16_F" (after enqueued)\n", pcb->snd_queuelen));
  if (pcb->snd_queuelen != 0) {
    LWIP_ASSERT("tcp_enqueue_flags: invalid queue length",
                pcb->unacked != NULL || pcb->unsent != NULL);
  }

  return ERR_OK;
}

#if LWIP_TCP_TIMESTAMPS
/* Build a timestamp option (12 bytes long) at the specified options pointer)
 *
 * @param pcb tcp_pcb
 * @param opts option pointer where to store the timestamp option
 */
static void
tcp_build_timestamp_option(const struct tcp_pcb *pcb, u32_t *opts)
{
  LWIP_ASSERT("tcp_build_timestamp_option: invalid pcb", pcb != NULL);

  /* Pad with two NOP options to make everything nicely aligned */
  opts[0] = PP_HTONL(0x0101080A);
  opts[1] = lwip_htonl(sys_now());
  opts[2] = lwip_htonl(pcb->ts_recent);
}
#endif

#if LWIP_TCP_SACK_OUT
/**
 * Calculates the number of SACK entries that should be generated.
 * It takes into account whether TF_SACK flag is set,
 * the number of SACK entries in tcp_pcb that are valid,
 * as well as the available options size.
 *
 * @param pcb tcp_pcb
 * @param optlen the length of other TCP options (in bytes)
 * @return the number of SACK ranges that can be used
 */
static u8_t
tcp_get_num_sacks(const struct tcp_pcb *pcb, u8_t optlen)
{
  u8_t num_sacks = 0;

  LWIP_ASSERT("tcp_get_num_sacks: invalid pcb", pcb != NULL);

  if (pcb->flags & TF_SACK) {
    u8_t i;

    /* The first SACK takes up 12 bytes (it includes SACK header and two NOP options),
       each additional one - 8 bytes. */
    optlen += 12;

    /* Max options size = 40, number of SACK array entries = LWIP_TCP_MAX_SACK_NUM */
    for (i = 0; (i < LWIP_TCP_MAX_SACK_NUM) && (optlen <= TCP_MAX_OPTION_BYTES) &&
         LWIP_TCP_SACK_VALID(pcb, i); ++i) {
      ++num_sacks;
      optlen += 8;
    }
  }

  return num_sacks;
}

/** Build a SACK option (12 or more bytes long) at the specified options pointer)
 *
 * @param pcb tcp_pcb
 * @param opts option pointer where to store the SACK option
 * @param num_sacks the number of SACKs to store
 */
static void
tcp_build_sack_option(const struct tcp_pcb *pcb, u32_t *opts, u8_t num_sacks)
{
  u8_t i;

  LWIP_ASSERT("tcp_build_sack_option: invalid pcb", pcb != NULL);
  LWIP_ASSERT("tcp_build_sack_option: invalid opts", opts != NULL);

  /* Pad with two NOP options to make everything nicely aligned.
     We add the length (of just the SACK option, not the NOPs in front of it),
     which is 2B of header, plus 8B for each SACK. */
  *(opts++) = PP_HTONL(0x01010500 + 2 + num_sacks * 8);

  for (i = 0; i < num_sacks; ++i) {
    *(opts++) = lwip_htonl(pcb->rcv_sacks[i].left);
    *(opts++) = lwip_htonl(pcb->rcv_sacks[i].right);
  }
}

#endif

#if LWIP_WND_SCALE
/** Build a window scale option (3 bytes long) at the specified options pointer)
 *
 * @param opts option pointer where to store the window scale option
 */
static void
tcp_build_wnd_scale_option(u32_t *opts)
{
  LWIP_ASSERT("tcp_build_wnd_scale_option: invalid opts", opts != NULL);

  /* Pad with one NOP option to make everything nicely aligned */
  opts[0] = PP_HTONL(0x01030300 | TCP_RCV_SCALE);
}
#endif

/**
 * @ingroup tcp_raw
 * Find out what we can send and send it
 *
 * @param pcb Protocol control block for the TCP connection to send data
 * @return ERR_OK if data has been sent or nothing to send
 *         another err_t on error
 */
err_t
tcp_output(struct tcp_pcb *pcb)
{
  struct tcp_seg *seg, *useg;
  u32_t wnd, snd_nxt;
  err_t err;
  struct netif *netif;
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
  u32_t profile_start_cycles;
  u32_t profile_segments = 0U;
  u32_t profile_bytes = 0U;
  u8_t profile_had_unsent;

  rltk_tcp_output_profile_begin();
  profile_start_cycles = DWT->CYCCNT;
#endif
#if TCP_CWND_DEBUG
  s16_t i = 0;
#endif /* TCP_CWND_DEBUG */

  LWIP_ASSERT_CORE_LOCKED();

  LWIP_ASSERT("tcp_output: invalid pcb", pcb != NULL);
  /* pcb->state LISTEN not allowed here */
  LWIP_ASSERT("don't call tcp_output for listen-pcbs",
              pcb->state != LISTEN);

  /* First, check if we are invoked by the TCP input processing
     code. If so, we do not output anything. Instead, we rely on the
     input processing code to call us when input processing is done
     with. */
  if (tcp_input_pcb == pcb) {
    TCP_OUTPUT_PROFILE_RETURN(ERR_OK, RLTK_TCP_OUTPUT_DEFERRED);
  }

  wnd = LWIP_MIN(pcb->snd_wnd, pcb->cwnd);

  seg = pcb->unsent;
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
  profile_had_unsent = (seg != NULL);
#endif

  if (seg == NULL) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_output: nothing to send (%p)\n",
                                   (void *)pcb->unsent));
    LWIP_DEBUGF(TCP_CWND_DEBUG, ("tcp_output: snd_wnd %"TCPWNDSIZE_F
                                 ", cwnd %"TCPWNDSIZE_F", wnd %"U32_F
                                 ", seg == NULL, ack %"U32_F"\n",
                                 pcb->snd_wnd, pcb->cwnd, wnd, pcb->lastack));

    /* If the TF_ACK_NOW flag is set and the ->unsent queue is empty, construct
     * an empty ACK segment and send it. */
    if (pcb->flags & TF_ACK_NOW) {
      TCP_OUTPUT_PROFILE_RETURN(tcp_send_empty_ack(pcb), RLTK_TCP_OUTPUT_EMPTY_ACK);
    }
    /* nothing to send: shortcut out of here */
    goto output_done;
  } else {
    LWIP_DEBUGF(TCP_CWND_DEBUG,
                ("tcp_output: snd_wnd %"TCPWNDSIZE_F", cwnd %"TCPWNDSIZE_F", wnd %"U32_F
                 ", effwnd %"U32_F", seq %"U32_F", ack %"U32_F"\n",
                 pcb->snd_wnd, pcb->cwnd, wnd,
                 lwip_ntohl(seg->tcphdr->seqno) - pcb->lastack + seg->len,
                 lwip_ntohl(seg->tcphdr->seqno), pcb->lastack));
  }

  netif = tcp_route(pcb, &pcb->local_ip, &pcb->remote_ip);
  if (netif == NULL) {
    TCP_OUTPUT_PROFILE_RETURN(ERR_RTE, RLTK_TCP_OUTPUT_ERROR);
  }

  /* If we don't have a local IP address, we get one from netif */
  if (ip_addr_isany(&pcb->local_ip)) {
    const ip_addr_t *local_ip = ip_netif_get_local_ip(netif, &pcb->remote_ip);
    if (local_ip == NULL) {
      TCP_OUTPUT_PROFILE_RETURN(ERR_RTE, RLTK_TCP_OUTPUT_ERROR);
    }
    ip_addr_copy(pcb->local_ip, *local_ip);
  }

  /* Handle the current segment not fitting within the window */
  if (lwip_ntohl(seg->tcphdr->seqno) - pcb->lastack + seg->len > wnd) {
    /* We need to start the persistent timer when the next unsent segment does not fit
     * within the remaining (could be 0) send window and RTO timer is not running (we
     * have no in-flight data). If window is still too small after persist timer fires,
     * then we split the segment. We don't consider the congestion window since a cwnd
     * smaller than 1 SMSS implies in-flight data
     */
    if (wnd == pcb->snd_wnd && pcb->unacked == NULL && pcb->persist_backoff == 0) {
      pcb->persist_cnt = 0;
      pcb->persist_backoff = 1;
      pcb->persist_probe = 0;
    }
    /* We need an ACK, but can't send data now, so send an empty ACK */
    if (pcb->flags & TF_ACK_NOW) {
      TCP_OUTPUT_PROFILE_RETURN(tcp_send_empty_ack(pcb), RLTK_TCP_OUTPUT_EMPTY_ACK);
    }
    goto output_done;
  }
  /* Stop persist timer, above conditions are not active */
  pcb->persist_backoff = 0;

  /* useg should point to last segment on unacked queue */
  useg = pcb->unacked;
  if (useg != NULL) {
    for (; useg->next != NULL; useg = useg->next);
  }
  /* data available and window allows it to be sent? */
  while (seg != NULL &&
         lwip_ntohl(seg->tcphdr->seqno) - pcb->lastack + seg->len <= wnd) {
    LWIP_ASSERT("RST not expected here!",
                (TCPH_FLAGS(seg->tcphdr) & TCP_RST) == 0);
    /* Stop sending if the nagle algorithm would prevent it
     * Don't stop:
     * - if tcp_write had a memory error before (prevent delayed ACK timeout) or
     * - if FIN was already enqueued for this PCB (SYN is always alone in a segment -
     *   either seg->next != NULL or pcb->unacked == NULL;
     *   RST is no sent using tcp_write/tcp_output.
     */
    if ((tcp_do_output_nagle(pcb) == 0) &&
        ((pcb->flags & (TF_NAGLEMEMERR | TF_FIN)) == 0)) {
      break;
    }
#if TCP_CWND_DEBUG
    LWIP_DEBUGF(TCP_CWND_DEBUG, ("tcp_output: snd_wnd %"TCPWNDSIZE_F", cwnd %"TCPWNDSIZE_F", wnd %"U32_F", effwnd %"U32_F", seq %"U32_F", ack %"U32_F", i %"S16_F"\n",
                                 pcb->snd_wnd, pcb->cwnd, wnd,
                                 lwip_ntohl(seg->tcphdr->seqno) + seg->len -
                                 pcb->lastack,
                                 lwip_ntohl(seg->tcphdr->seqno), pcb->lastack, i));
    ++i;
#endif /* TCP_CWND_DEBUG */

    if (pcb->state != SYN_SENT) {
      TCPH_SET_FLAG(seg->tcphdr, TCP_ACK);
    }

    err = tcp_output_segment(seg, pcb, netif);
    if (err != ERR_OK) {
      /* segment could not be sent, for whatever reason */
      tcp_set_flags(pcb, TF_NAGLEMEMERR);
      TCP_OUTPUT_PROFILE_RETURN(err, RLTK_TCP_OUTPUT_ERROR);
    }
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
    profile_segments++;
    profile_bytes += seg->len;
#endif
#if TCP_OVERSIZE_DBGCHECK
    seg->oversize_left = 0;
#endif /* TCP_OVERSIZE_DBGCHECK */
    pcb->unsent = seg->next;
    if (pcb->state != SYN_SENT) {
      tcp_clear_flags(pcb, TF_ACK_DELAY | TF_ACK_NOW);
    }
    snd_nxt = lwip_ntohl(seg->tcphdr->seqno) + TCP_TCPLEN(seg);
    if (TCP_SEQ_LT(pcb->snd_nxt, snd_nxt)) {
      pcb->snd_nxt = snd_nxt;
    }
    /* put segment on unacknowledged list if length > 0 */
    if (TCP_TCPLEN(seg) > 0) {
      seg->next = NULL;
      /* unacked list is empty? */
      if (pcb->unacked == NULL) {
        pcb->unacked = seg;
        useg = seg;
        /* unacked list is not empty? */
      } else {
        /* In the case of fast retransmit, the packet should not go to the tail
         * of the unacked queue, but rather somewhere before it. We need to check for
         * this case. -STJ Jul 27, 2004 */
        if (TCP_SEQ_LT(lwip_ntohl(seg->tcphdr->seqno), lwip_ntohl(useg->tcphdr->seqno))) {
          /* add segment to before tail of unacked list, keeping the list sorted */
          struct tcp_seg **cur_seg = &(pcb->unacked);
          while (*cur_seg &&
                 TCP_SEQ_LT(lwip_ntohl((*cur_seg)->tcphdr->seqno), lwip_ntohl(seg->tcphdr->seqno))) {
            cur_seg = &((*cur_seg)->next );
          }
          seg->next = (*cur_seg);
          (*cur_seg) = seg;
        } else {
          /* add segment to tail of unacked list */
          useg->next = seg;
          useg = useg->next;
        }
      }
      /* do not queue empty segments on the unacked list */
    } else {
      tcp_seg_free(seg);
    }
    seg = pcb->unsent;
  }
#if TCP_OVERSIZE
  if (pcb->unsent == NULL) {
    /* last unsent has been removed, reset unsent_oversize */
    pcb->unsent_oversize = 0;
  }
#endif /* TCP_OVERSIZE */

output_done:
  tcp_clear_flags(pcb, TF_NAGLEMEMERR);
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
  if (profile_segments != 0U) {
    TCP_OUTPUT_PROFILE_RETURN(ERR_OK, RLTK_TCP_OUTPUT_DATA);
  }
  if (profile_had_unsent) {
    TCP_OUTPUT_PROFILE_RETURN(ERR_OK, RLTK_TCP_OUTPUT_DEFERRED);
  }
#endif
  TCP_OUTPUT_PROFILE_RETURN(ERR_OK, RLTK_TCP_OUTPUT_NOOP);
}

/** Check if a segment's pbufs are used by someone else than TCP.
 * This can happen on retransmission if the pbuf of this segment is still
 * referenced by the netif driver due to deferred transmission.
 * This is the case (only!) if someone down the TX call path called
 * pbuf_ref() on one of the pbufs!
 *
 * @arg seg the tcp segment to check
 * @return 1 if ref != 1, 0 if ref == 1
 */
static int
tcp_output_segment_busy(const struct tcp_seg *seg)
{
  LWIP_ASSERT("tcp_output_segment_busy: invalid seg", seg != NULL);

  /* We only need to check the first pbuf here:
     If a pbuf is queued for transmission, a driver calls pbuf_ref(),
     which only changes the ref count of the first pbuf */
  if (seg->p->ref != 1) {
    /* other reference found */
    return 1;
  }
  /* no other references found */
  return 0;
}

/**
 * Called by tcp_output() to actually send a TCP segment over IP.
 *
 * @param seg the tcp_seg to send
 * @param pcb the tcp_pcb for the TCP connection used to send the segment
 * @param netif the netif used to send the segment
 */
static err_t
tcp_output_segment(struct tcp_seg *seg, struct tcp_pcb *pcb, struct netif *netif)
{
  err_t err;
  u16_t len;
  u32_t *opts;
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
  u32_t profile_start_cycles = DWT->CYCCNT;
  u32_t profile_checksum_start_cycles;
  u32_t profile_prepare_cycles;
  u32_t profile_checksum_cycles;
  u32_t profile_ip_start_cycles;
  u32_t profile_ip_cycles;
  u32_t profile_bytes;
#endif
#if TCP_CHECKSUM_ON_COPY
  int seg_chksum_was_swapped = 0;
#endif

  LWIP_ASSERT("tcp_output_segment: invalid seg", seg != NULL);
  LWIP_ASSERT("tcp_output_segment: invalid pcb", pcb != NULL);
  LWIP_ASSERT("tcp_output_segment: invalid netif", netif != NULL);

  if (tcp_output_segment_busy(seg)) {
    /* This should not happen: rexmit functions should have checked this.
       However, since this function modifies p->len, we must not continue in this case. */
    LWIP_DEBUGF(TCP_RTO_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("tcp_output_segment: segment busy\n"));
    return ERR_OK;
  }

  /* The TCP header has already been constructed, but the ackno and
   wnd fields remain. */
  seg->tcphdr->ackno = lwip_htonl(pcb->rcv_nxt);

  /* advertise our receive window size in this TCP segment */
#if LWIP_WND_SCALE
  if (seg->flags & TF_SEG_OPTS_WND_SCALE) {
    /* The Window field in a SYN segment itself (the only type where we send
       the window scale option) is never scaled. */
    seg->tcphdr->wnd = lwip_htons(TCPWND_MIN16(pcb->rcv_ann_wnd));
  } else
#endif /* LWIP_WND_SCALE */
  {
    seg->tcphdr->wnd = lwip_htons(TCPWND_MIN16(RCV_WND_SCALE(pcb, pcb->rcv_ann_wnd)));
  }

  pcb->rcv_ann_right_edge = pcb->rcv_nxt + pcb->rcv_ann_wnd;

  /* Add any requested options.  NB MSS option is only set on SYN
     packets, so ignore it here */
  /* cast through void* to get rid of alignment warnings */
  opts = (u32_t *)(void *)(seg->tcphdr + 1);
  if (seg->flags & TF_SEG_OPTS_MSS) {
    u16_t mss;
#if TCP_CALCULATE_EFF_SEND_MSS
    mss = tcp_eff_send_mss_netif(TCP_MSS, netif, &pcb->remote_ip);
#else /* TCP_CALCULATE_EFF_SEND_MSS */
    mss = TCP_MSS;
#endif /* TCP_CALCULATE_EFF_SEND_MSS */
    *opts = TCP_BUILD_MSS_OPTION(mss);
    opts += 1;
  }
#if LWIP_TCP_TIMESTAMPS
  pcb->ts_lastacksent = pcb->rcv_nxt;

  if (seg->flags & TF_SEG_OPTS_TS) {
    tcp_build_timestamp_option(pcb, opts);
    opts += 3;
  }
#endif
#if LWIP_WND_SCALE
  if (seg->flags & TF_SEG_OPTS_WND_SCALE) {
    tcp_build_wnd_scale_option(opts);
    opts += 1;
  }
#endif
#if LWIP_TCP_SACK_OUT
  if (seg->flags & TF_SEG_OPTS_SACK_PERM) {
    /* Pad with two NOP options to make everything nicely aligned
     * NOTE: When we send both timestamp and SACK_PERM options,
     * we could use the first two NOPs before the timestamp to store SACK_PERM option,
     * but that would complicate the code.
     */
    *(opts++) = PP_HTONL(0x01010402);
  }
#endif

  /* Set retransmission timer running if it is not currently enabled
     This must be set before checking the route. */
  if (pcb->rtime < 0) {
    pcb->rtime = 0;
  }

  if (pcb->rttest == 0) {
    pcb->rttest = tcp_ticks;
    pcb->rtseq = lwip_ntohl(seg->tcphdr->seqno);

    LWIP_DEBUGF(TCP_RTO_DEBUG, ("tcp_output_segment: rtseq %"U32_F"\n", pcb->rtseq));
  }
  LWIP_DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_output_segment: %"U32_F":%"U32_F"\n",
                                 lwip_htonl(seg->tcphdr->seqno), lwip_htonl(seg->tcphdr->seqno) +
                                 seg->len));

  len = (u16_t)((u8_t *)seg->tcphdr - (u8_t *)seg->p->payload);
  if (len == 0) {
    /** Exclude retransmitted segments from this count. */
    MIB2_STATS_INC(mib2.tcpoutsegs);
  }

  seg->p->len -= len;
  seg->p->tot_len -= len;

  seg->p->payload = seg->tcphdr;

  seg->tcphdr->chksum = 0;

#ifdef LWIP_HOOK_TCP_OUT_ADD_TCPOPTS
  opts = LWIP_HOOK_TCP_OUT_ADD_TCPOPTS(seg->p, seg->tcphdr, pcb, opts);
#endif
  LWIP_ASSERT("options not filled", (u8_t *)opts == ((u8_t *)(seg->tcphdr + 1)) + LWIP_TCP_OPT_LENGTH_SEGMENT(seg->flags, pcb));

#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
  profile_prepare_cycles = DWT->CYCCNT - profile_start_cycles;
  profile_checksum_start_cycles = DWT->CYCCNT;
#endif

#if CHECKSUM_GEN_TCP
  IF__NETIF_CHECKSUM_ENABLED(netif, NETIF_CHECKSUM_GEN_TCP) {
#if TCP_CHECKSUM_ON_COPY
    u32_t acc;
#if TCP_CHECKSUM_ON_COPY_SANITY_CHECK
    u16_t chksum_slow = ip_chksum_pseudo(seg->p, IP_PROTO_TCP,
                                         seg->p->tot_len, &pcb->local_ip, &pcb->remote_ip);
#endif /* TCP_CHECKSUM_ON_COPY_SANITY_CHECK */
    if ((seg->flags & TF_SEG_DATA_CHECKSUMMED) == 0) {
      LWIP_ASSERT("data included but not checksummed",
                  seg->p->tot_len == TCPH_HDRLEN_BYTES(seg->tcphdr));
    }

    /* rebuild TCP header checksum (TCP header changes for retransmissions!) */
    acc = ip_chksum_pseudo_partial(seg->p, IP_PROTO_TCP,
                                   seg->p->tot_len, TCPH_HDRLEN_BYTES(seg->tcphdr), &pcb->local_ip, &pcb->remote_ip);
    /* add payload checksum */
    if (seg->chksum_swapped) {
      seg_chksum_was_swapped = 1;
      seg->chksum = SWAP_BYTES_IN_WORD(seg->chksum);
      seg->chksum_swapped = 0;
    }
    acc = (u16_t)~acc + seg->chksum;
    seg->tcphdr->chksum = (u16_t)~FOLD_U32T(acc);
#if TCP_CHECKSUM_ON_COPY_SANITY_CHECK
    if (chksum_slow != seg->tcphdr->chksum) {
      TCP_CHECKSUM_ON_COPY_SANITY_CHECK_FAIL(
        ("tcp_output_segment: calculated checksum is %"X16_F" instead of %"X16_F"\n",
         seg->tcphdr->chksum, chksum_slow));
      seg->tcphdr->chksum = chksum_slow;
    }
#endif /* TCP_CHECKSUM_ON_COPY_SANITY_CHECK */
#else /* TCP_CHECKSUM_ON_COPY */
    seg->tcphdr->chksum = ip_chksum_pseudo(seg->p, IP_PROTO_TCP,
                                           seg->p->tot_len, &pcb->local_ip, &pcb->remote_ip);
#endif /* TCP_CHECKSUM_ON_COPY */
  }
#endif /* CHECKSUM_GEN_TCP */
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
  profile_checksum_cycles = DWT->CYCCNT - profile_checksum_start_cycles;
  profile_bytes = seg->p->tot_len;
  profile_ip_start_cycles = DWT->CYCCNT;
#endif
  TCP_STATS_INC(tcp.xmit);

  NETIF_SET_HINTS(netif, &(pcb->netif_hints));
  err = ip_output_if(seg->p, &pcb->local_ip, &pcb->remote_ip, pcb->ttl,
                     pcb->tos, IP_PROTO_TCP, netif);
  NETIF_RESET_HINTS(netif);

#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
  profile_ip_cycles = DWT->CYCCNT - profile_ip_start_cycles;
  rltk_tcp_output_profile_lwip(0U, profile_bytes, profile_prepare_cycles,
                              profile_checksum_cycles, profile_ip_cycles);
#endif

#if TCP_CHECKSUM_ON_COPY
  if (seg_chksum_was_swapped) {
    /* if data is added to this segment later, chksum needs to be swapped,
       so restore this now */
    seg->chksum = SWAP_BYTES_IN_WORD(seg->chksum);
    seg->chksum_swapped = 1;
  }
#endif

  return err;
}

/**
 * Requeue all unacked segments for retransmission
 *
 * Called by tcp_slowtmr() for slow retransmission.
 *
 * @param pcb the tcp_pcb for which to re-enqueue all unacked segments
 */
err_t
tcp_rexmit_rto_prepare(struct tcp_pcb *pcb)
{
  struct tcp_seg *seg;

  LWIP_ASSERT("tcp_rexmit_rto_prepare: invalid pcb", pcb != NULL);

  if (pcb->unacked == NULL) {
    return ERR_VAL;
  }

  /* Move all unacked segments to the head of the unsent queue.
     However, give up if any of the unsent pbufs are still referenced by the
     netif driver due to deferred transmission. No point loading the link further
     if it is struggling to flush its buffered writes. */
  for (seg = pcb->unacked; seg->next != NULL; seg = seg->next) {
    if (tcp_output_segment_busy(seg)) {
      LWIP_DEBUGF(TCP_RTO_DEBUG, ("tcp_rexmit_rto: segment busy\n"));
      return ERR_VAL;
    }
  }
  if (tcp_output_segment_busy(seg)) {
    LWIP_DEBUGF(TCP_RTO_DEBUG, ("tcp_rexmit_rto: segment busy\n"));
    return ERR_VAL;
  }
  /* concatenate unsent queue after unacked queue */
  seg->next = pcb->unsent;
#if TCP_OVERSIZE_DBGCHECK
  /* if last unsent changed, we need to update unsent_oversize */
  if (pcb->unsent == NULL) {
    pcb->unsent_oversize = seg->oversize_left;
  }
#endif /* TCP_OVERSIZE_DBGCHECK */
  /* unsent queue is the concatenated queue (of unacked, unsent) */
  pcb->unsent = pcb->unacked;
  /* unacked queue is now empty */
  pcb->unacked = NULL;

  /* Mark RTO in-progress */
  tcp_set_flags(pcb, TF_RTO);
  /* Record the next byte following retransmit */
  pcb->rto_end = lwip_ntohl(seg->tcphdr->seqno) + TCP_TCPLEN(seg);
  /* Don't take any RTT measurements after retransmitting. */
  pcb->rttest = 0;

  return ERR_OK;
}

/**
 * Requeue all unacked segments for retransmission
 *
 * Called by tcp_slowtmr() for slow retransmission.
 *
 * @param pcb the tcp_pcb for which to re-enqueue all unacked segments
 */
void
tcp_rexmit_rto_commit(struct tcp_pcb *pcb)
{
  LWIP_ASSERT("tcp_rexmit_rto_commit: invalid pcb", pcb != NULL);

  /* increment number of retransmissions */
  if (pcb->nrtx < 0xFF) {
    ++pcb->nrtx;
  }
  /* Do the actual retransmission */
  tcp_output(pcb);
}

/**
 * Requeue all unacked segments for retransmission
 *
 * Called by tcp_process() only, tcp_slowtmr() needs to do some things between
 * "prepare" and "commit".
 *
 * @param pcb the tcp_pcb for which to re-enqueue all unacked segments
 */
void
tcp_rexmit_rto(struct tcp_pcb *pcb)
{
  LWIP_ASSERT("tcp_rexmit_rto: invalid pcb", pcb != NULL);

  if (tcp_rexmit_rto_prepare(pcb) == ERR_OK) {
    tcp_rexmit_rto_commit(pcb);
  }
}

/**
 * Requeue the first unacked segment for retransmission
 *
 * Called by tcp_receive() for fast retransmit.
 *
 * @param pcb the tcp_pcb for which to retransmit the first unacked segment
 */
err_t
tcp_rexmit(struct tcp_pcb *pcb)
{
  struct tcp_seg *seg;
  struct tcp_seg **cur_seg;

  LWIP_ASSERT("tcp_rexmit: invalid pcb", pcb != NULL);

  if (pcb->unacked == NULL) {
    return ERR_VAL;
  }

  seg = pcb->unacked;

  /* Give up if the segment is still referenced by the netif driver
     due to deferred transmission. */
  if (tcp_output_segment_busy(seg)) {
    LWIP_DEBUGF(TCP_RTO_DEBUG, ("tcp_rexmit busy\n"));
    return ERR_VAL;
  }

  /* Move the first unacked segment to the unsent queue */
  /* Keep the unsent queue sorted. */
  pcb->unacked = seg->next;

  cur_seg = &(pcb->unsent);
  while (*cur_seg &&
         TCP_SEQ_LT(lwip_ntohl((*cur_seg)->tcphdr->seqno), lwip_ntohl(seg->tcphdr->seqno))) {
    cur_seg = &((*cur_seg)->next );
  }
  seg->next = *cur_seg;
  *cur_seg = seg;
#if TCP_OVERSIZE
  if (seg->next == NULL) {
    /* the retransmitted segment is last in unsent, so reset unsent_oversize */
    pcb->unsent_oversize = 0;
  }
#endif /* TCP_OVERSIZE */

  if (pcb->nrtx < 0xFF) {
    ++pcb->nrtx;
  }

  /* Don't take any rtt measurements after retransmitting. */
  pcb->rttest = 0;

  /* Do the actual retransmission. */
  MIB2_STATS_INC(mib2.tcpretranssegs);
  /* No need to call tcp_output: we are always called from tcp_input()
     and thus tcp_output directly returns. */
  return ERR_OK;
}


/**
 * Handle retransmission after three dupacks received
 *
 * @param pcb the tcp_pcb for which to retransmit the first unacked segment
 */
void
tcp_rexmit_fast(struct tcp_pcb *pcb)
{
  LWIP_ASSERT("tcp_rexmit_fast: invalid pcb", pcb != NULL);

  if (pcb->unacked != NULL && !(pcb->flags & TF_INFR)) {
    /* This is fast retransmit. Retransmit the first unacked segment. */
    LWIP_DEBUGF(TCP_FR_DEBUG,
                ("tcp_receive: dupacks %"U16_F" (%"U32_F
                 "), fast retransmit %"U32_F"\n",
                 (u16_t)pcb->dupacks, pcb->lastack,
                 lwip_ntohl(pcb->unacked->tcphdr->seqno)));
    if (tcp_rexmit(pcb) == ERR_OK) {
      /* Set ssthresh to half of the minimum of the current
       * cwnd and the advertised window */
      pcb->ssthresh = LWIP_MIN(pcb->cwnd, pcb->snd_wnd) / 2;

      /* The minimum value for ssthresh should be 2 MSS */
      if (pcb->ssthresh < (2U * pcb->mss)) {
        LWIP_DEBUGF(TCP_FR_DEBUG,
                    ("tcp_receive: The minimum value for ssthresh %"TCPWNDSIZE_F
                     " should be min 2 mss %"U16_F"...\n",
                     pcb->ssthresh, (u16_t)(2 * pcb->mss)));
        pcb->ssthresh = 2 * pcb->mss;
      }

      pcb->cwnd = pcb->ssthresh + 3 * pcb->mss;
      tcp_set_flags(pcb, TF_INFR);

      /* Reset the retransmission timer to prevent immediate rto retransmissions */
      pcb->rtime = 0;
    }
  }
}

static struct pbuf *
tcp_output_alloc_header_common(u32_t ackno, u16_t optlen, u16_t datalen,
                        u32_t seqno_be /* already in network byte order */,
                        u16_t src_port, u16_t dst_port, u8_t flags, u16_t wnd)
{
  struct tcp_hdr *tcphdr;
  struct pbuf *p;

  p = pbuf_alloc(PBUF_IP, TCP_HLEN + optlen + datalen, PBUF_RAM);
  if (p != NULL) {
    LWIP_ASSERT("check that first pbuf can hold struct tcp_hdr",
                (p->len >= TCP_HLEN + optlen));
    tcphdr = (struct tcp_hdr *)p->payload;
    tcphdr->src = lwip_htons(src_port);
    tcphdr->dest = lwip_htons(dst_port);
    tcphdr->seqno = seqno_be;
    tcphdr->ackno = lwip_htonl(ackno);
    TCPH_HDRLEN_FLAGS_SET(tcphdr, (5 + optlen / 4), flags);
    tcphdr->wnd = lwip_htons(wnd);
    tcphdr->chksum = 0;
    tcphdr->urgp = 0;
  }
  return p;
}

/** Allocate a pbuf and create a tcphdr at p->payload, used for output
 * functions other than the default tcp_output -> tcp_output_segment
 * (e.g. tcp_send_empty_ack, etc.)
 *
 * @param pcb tcp pcb for which to send a packet (used to initialize tcp_hdr)
 * @param optlen length of header-options
 * @param datalen length of tcp data to reserve in pbuf
 * @param seqno_be seqno in network byte order (big-endian)
 * @return pbuf with p->payload being the tcp_hdr
 */
static struct pbuf *
tcp_output_alloc_header(struct tcp_pcb *pcb, u16_t optlen, u16_t datalen,
                        u32_t seqno_be /* already in network byte order */)
{
  struct pbuf *p;

  LWIP_ASSERT("tcp_output_alloc_header: invalid pcb", pcb != NULL);

  p = tcp_output_alloc_header_common(pcb->rcv_nxt, optlen, datalen,
    seqno_be, pcb->local_port, pcb->remote_port, TCP_ACK,
    TCPWND_MIN16(RCV_WND_SCALE(pcb, pcb->rcv_ann_wnd)));
  if (p != NULL) {
    /* If we're sending a packet, update the announced right window edge */
    pcb->rcv_ann_right_edge = pcb->rcv_nxt + pcb->rcv_ann_wnd;
  }
  return p;
}

/* Fill in options for control segments */
static void
tcp_output_fill_options(const struct tcp_pcb *pcb, struct pbuf *p, u8_t optflags, u8_t num_sacks)
{
  struct tcp_hdr *tcphdr;
  u32_t *opts;
  u16_t sacks_len = 0;

  LWIP_ASSERT("tcp_output_fill_options: invalid pbuf", p != NULL);

  tcphdr = (struct tcp_hdr *)p->payload;
  opts = (u32_t *)(void *)(tcphdr + 1);

  /* NB. MSS and window scale options are only sent on SYNs, so ignore them here */

#if LWIP_TCP_TIMESTAMPS
  if (optflags & TF_SEG_OPTS_TS) {
    tcp_build_timestamp_option(pcb, opts);
    opts += 3;
  }
#endif

#if LWIP_TCP_SACK_OUT
  if (pcb && (num_sacks > 0)) {
    tcp_build_sack_option(pcb, opts, num_sacks);
    /* 1 word for SACKs header (including 2xNOP), and 2 words for each SACK */
    sacks_len = 1 + num_sacks * 2;
    opts += sacks_len;
  }
#else
  LWIP_UNUSED_ARG(num_sacks);
#endif

#ifdef LWIP_HOOK_TCP_OUT_ADD_TCPOPTS
  opts = LWIP_HOOK_TCP_OUT_ADD_TCPOPTS(p, tcphdr, pcb, opts);
#endif

  LWIP_UNUSED_ARG(pcb);
  LWIP_UNUSED_ARG(sacks_len);
  LWIP_ASSERT("options not filled", (u8_t *)opts == ((u8_t *)(tcphdr + 1)) + sacks_len * 4 + LWIP_TCP_OPT_LENGTH_SEGMENT(optflags, pcb));
  LWIP_UNUSED_ARG(optflags); /* for LWIP_NOASSERT */
  LWIP_UNUSED_ARG(opts); /* for LWIP_NOASSERT */
}

/** Output a control segment pbuf to IP.
 *
 * Called from tcp_rst, tcp_send_empty_ack, tcp_keepalive and tcp_zero_window_probe,
 * this function combines selecting a netif for transmission, generating the tcp
 * header checksum and calling ip_output_if while handling netif hints and stats.
 */
static err_t
tcp_output_control_segment(const struct tcp_pcb *pcb, struct pbuf *p,
                           const ip_addr_t *src, const ip_addr_t *dst)
{
  err_t err;
  struct netif *netif;
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
  u32_t profile_start_cycles = DWT->CYCCNT;
  u32_t profile_prepare_cycles = 0U;
  u32_t profile_checksum_start_cycles = 0U;
  u32_t profile_checksum_cycles = 0U;
  u32_t profile_ip_start_cycles = 0U;
  u32_t profile_ip_cycles = 0U;
  u32_t profile_bytes = 0U;
#endif

  LWIP_ASSERT("tcp_output_control_segment: invalid pbuf", p != NULL);
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
  profile_bytes = p->tot_len;
#endif

  netif = tcp_route(pcb, src, dst);
  if (netif == NULL) {
    err = ERR_RTE;
  } else {
    u8_t ttl, tos;
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
    profile_checksum_start_cycles = DWT->CYCCNT;
#endif
#if CHECKSUM_GEN_TCP
    IF__NETIF_CHECKSUM_ENABLED(netif, NETIF_CHECKSUM_GEN_TCP) {
      struct tcp_hdr *tcphdr = (struct tcp_hdr *)p->payload;
      tcphdr->chksum = ip_chksum_pseudo(p, IP_PROTO_TCP, p->tot_len,
                                        src, dst);
    }
#endif
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
    profile_checksum_cycles = DWT->CYCCNT - profile_checksum_start_cycles;
#endif
    if (pcb != NULL) {
      NETIF_SET_HINTS(netif, LWIP_CONST_CAST(struct netif_hint*, &(pcb->netif_hints)));
      ttl = pcb->ttl;
      tos = pcb->tos;
    } else {
      /* Send output with hardcoded TTL/HL since we have no access to the pcb */
      ttl = TCP_TTL;
      tos = 0;
    }
    TCP_STATS_INC(tcp.xmit);
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
    profile_ip_start_cycles = DWT->CYCCNT;
    profile_prepare_cycles = profile_ip_start_cycles - profile_start_cycles -
                             profile_checksum_cycles;
#endif
    err = ip_output_if(p, src, dst, ttl, tos, IP_PROTO_TCP, netif);
    NETIF_RESET_HINTS(netif);
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
    profile_ip_cycles = DWT->CYCCNT - profile_ip_start_cycles;
#endif
  }
#if defined(CONFIG_PLATFORM_8195BHP) && CONFIG_TCP_OUTPUT_PROFILE
  rltk_tcp_output_profile_lwip(1U, profile_bytes, profile_prepare_cycles,
                              profile_checksum_cycles, profile_ip_cycles);
#endif
  pbuf_free(p);
  return err;
}

/**
 * Send a TCP RESET packet (empty segment with RST flag set) either to
 * abort a connection or to show that there is no matching local connection
 * for a received segment.
 *
 * Called by tcp_abort() (to abort a local connection), tcp_input() (if no
 * matching local pcb was found), tcp_listen_input() (if incoming segment
 * has ACK flag set) and tcp_process() (received segment in the wrong state)
 *
 * Since a RST segment is in most cases not sent for an active connection,
 * tcp_rst() has a number of arguments that are taken from a tcp_pcb for
 * most other segment output functions.
 *
 * @param pcb TCP pcb (may be NULL if no pcb is available)
 * @param seqno the sequence number to use for the outgoing segment
 * @param ackno the acknowledge number to use for the outgoing segment
 * @param local_ip the local IP address to send the segment from
 * @param remote_ip the remote IP address to send the segment to
 * @param local_port the local TCP port to send the segment from
 * @param remote_port the remote TCP port to send the segment to
 */
void
tcp_rst(const struct tcp_pcb *pcb, u32_t seqno, u32_t ackno,
        const ip_addr_t *local_ip, const ip_addr_t *remote_ip,
        u16_t local_port, u16_t remote_port)
{
  struct pbuf *p;
  u16_t wnd;
  u8_t optlen;

  LWIP_ASSERT("tcp_rst: invalid local_ip", local_ip != NULL);
  LWIP_ASSERT("tcp_rst: invalid remote_ip", remote_ip != NULL);

  optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(0, pcb);

#if LWIP_WND_SCALE
  wnd = PP_HTONS(((TCP_WND >> TCP_RCV_SCALE) & 0xFFFF));
#else
  wnd = PP_HTONS(TCP_WND);
#endif

  p = tcp_output_alloc_header_common(ackno, optlen, 0, lwip_htonl(seqno), local_port,
    remote_port, TCP_RST | TCP_ACK, wnd);
  if (p == NULL) {
    LWIP_DEBUGF(TCP_DEBUG, ("tcp_rst: could not allocate memory for pbuf\n"));
    return;
  }
  tcp_output_fill_options(pcb, p, 0, optlen);

  MIB2_STATS_INC(mib2.tcpoutrsts);

  tcp_output_control_segment(pcb, p, local_ip, remote_ip);
  LWIP_DEBUGF(TCP_RST_DEBUG, ("tcp_rst: seqno %"U32_F" ackno %"U32_F".\n", seqno, ackno));
}

/**
 * Send an ACK without data.
 *
 * @param pcb Protocol control block for the TCP connection to send the ACK
 */
err_t
tcp_send_empty_ack(struct tcp_pcb *pcb)
{
  err_t err;
  struct pbuf *p;
  u8_t optlen, optflags = 0;
  u8_t num_sacks = 0;

  LWIP_ASSERT("tcp_send_empty_ack: invalid pcb", pcb != NULL);

#if LWIP_TCP_TIMESTAMPS
  if (pcb->flags & TF_TIMESTAMP) {
    optflags = TF_SEG_OPTS_TS;
  }
#endif
  optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(optflags, pcb);

#if LWIP_TCP_SACK_OUT
  /* For now, SACKs are only sent with empty ACKs */
  if ((num_sacks = tcp_get_num_sacks(pcb, optlen)) > 0) {
    optlen += 4 + num_sacks * 8; /* 4 bytes for header (including 2*NOP), plus 8B for each SACK */
  }
#endif

  p = tcp_output_alloc_header(pcb, optlen, 0, lwip_htonl(pcb->snd_nxt));
  if (p == NULL) {
    /* let tcp_fasttmr retry sending this ACK */
    tcp_set_flags(pcb, TF_ACK_DELAY | TF_ACK_NOW);
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_output: (ACK) could not allocate pbuf\n"));
    return ERR_BUF;
  }
  tcp_output_fill_options(pcb, p, optflags, num_sacks);

#if LWIP_TCP_TIMESTAMPS
  pcb->ts_lastacksent = pcb->rcv_nxt;
#endif

  LWIP_DEBUGF(TCP_OUTPUT_DEBUG,
              ("tcp_output: sending ACK for %"U32_F"\n", pcb->rcv_nxt));
  err = tcp_output_control_segment(pcb, p, &pcb->local_ip, &pcb->remote_ip);
  if (err != ERR_OK) {
    /* let tcp_fasttmr retry sending this ACK */
    tcp_set_flags(pcb, TF_ACK_DELAY | TF_ACK_NOW);
  } else {
    /* remove ACK flags from the PCB, as we sent an empty ACK now */
    tcp_clear_flags(pcb, TF_ACK_DELAY | TF_ACK_NOW);
  }

  return err;
}

/**
 * Send keepalive packets to keep a connection active although
 * no data is sent over it.
 *
 * Called by tcp_slowtmr()
 *
 * @param pcb the tcp_pcb for which to send a keepalive packet
 */
err_t
tcp_keepalive(struct tcp_pcb *pcb)
{
  err_t err;
  struct pbuf *p;
  u8_t optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(0, pcb);

  LWIP_ASSERT("tcp_keepalive: invalid pcb", pcb != NULL);

  LWIP_DEBUGF(TCP_DEBUG, ("tcp_keepalive: sending KEEPALIVE probe to "));
  ip_addr_debug_print_val(TCP_DEBUG, pcb->remote_ip);
  LWIP_DEBUGF(TCP_DEBUG, ("\n"));

  LWIP_DEBUGF(TCP_DEBUG, ("tcp_keepalive: tcp_ticks %"U32_F"   pcb->tmr %"U32_F" pcb->keep_cnt_sent %"U16_F"\n",
                          tcp_ticks, pcb->tmr, (u16_t)pcb->keep_cnt_sent));

  p = tcp_output_alloc_header(pcb, optlen, 0, lwip_htonl(pcb->snd_nxt - 1));
  if (p == NULL) {
    LWIP_DEBUGF(TCP_DEBUG,
                ("tcp_keepalive: could not allocate memory for pbuf\n"));
    return ERR_MEM;
  }
  tcp_output_fill_options(pcb, p, 0, optlen);
  err = tcp_output_control_segment(pcb, p, &pcb->local_ip, &pcb->remote_ip);

  LWIP_DEBUGF(TCP_DEBUG, ("tcp_keepalive: seqno %"U32_F" ackno %"U32_F" err %d.\n",
                          pcb->snd_nxt - 1, pcb->rcv_nxt, (int)err));
  return err;
}

/**
 * Send persist timer zero-window probes to keep a connection active
 * when a window update is lost.
 *
 * Called by tcp_slowtmr()
 *
 * @param pcb the tcp_pcb for which to send a zero-window probe packet
 */
err_t
tcp_zero_window_probe(struct tcp_pcb *pcb)
{
  err_t err;
  struct pbuf *p;
  struct tcp_hdr *tcphdr;
  struct tcp_seg *seg;
  u16_t len;
  u8_t is_fin;
  u32_t snd_nxt;
  u8_t optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(0, pcb);

  LWIP_ASSERT("tcp_zero_window_probe: invalid pcb", pcb != NULL);

  LWIP_DEBUGF(TCP_DEBUG, ("tcp_zero_window_probe: sending ZERO WINDOW probe to "));
  ip_addr_debug_print_val(TCP_DEBUG, pcb->remote_ip);
  LWIP_DEBUGF(TCP_DEBUG, ("\n"));

  LWIP_DEBUGF(TCP_DEBUG,
              ("tcp_zero_window_probe: tcp_ticks %"U32_F
               "   pcb->tmr %"U32_F" pcb->keep_cnt_sent %"U16_F"\n",
               tcp_ticks, pcb->tmr, (u16_t)pcb->keep_cnt_sent));

  /* Only consider unsent, persist timer should be off when there is data in-flight */
  seg = pcb->unsent;
  if (seg == NULL) {
    /* Not expected, persist timer should be off when the send buffer is empty */
    return ERR_OK;
  }

  /* increment probe count. NOTE: we record probe even if it fails
     to actually transmit due to an error. This ensures memory exhaustion/
     routing problem doesn't leave a zero-window pcb as an indefinite zombie.
     RTO mechanism has similar behavior, see pcb->nrtx */
  if (pcb->persist_probe < 0xFF) {
    ++pcb->persist_probe;
  }

  is_fin = ((TCPH_FLAGS(seg->tcphdr) & TCP_FIN) != 0) && (seg->len == 0);
  /* we want to send one seqno: either FIN or data (no options) */
  len = is_fin ? 0 : 1;

  p = tcp_output_alloc_header(pcb, optlen, len, seg->tcphdr->seqno);
  if (p == NULL) {
    LWIP_DEBUGF(TCP_DEBUG, ("tcp_zero_window_probe: no memory for pbuf\n"));
    return ERR_MEM;
  }
  tcphdr = (struct tcp_hdr *)p->payload;

  if (is_fin) {
    /* FIN segment, no data */
    TCPH_FLAGS_SET(tcphdr, TCP_ACK | TCP_FIN);
  } else {
    /* Data segment, copy in one byte from the head of the unacked queue */
    char *d = ((char *)p->payload + TCP_HLEN);
    /* Depending on whether the segment has already been sent (unacked) or not
       (unsent), seg->p->payload points to the IP header or TCP header.
       Ensure we copy the first TCP data byte: */
    pbuf_copy_partial(seg->p, d, 1, seg->p->tot_len - seg->len);
  }

  /* The byte may be acknowledged without the window being opened. */
  snd_nxt = lwip_ntohl(seg->tcphdr->seqno) + 1;
  if (TCP_SEQ_LT(pcb->snd_nxt, snd_nxt)) {
    pcb->snd_nxt = snd_nxt;
  }
  tcp_output_fill_options(pcb, p, 0, optlen);

  err = tcp_output_control_segment(pcb, p, &pcb->local_ip, &pcb->remote_ip);

  LWIP_DEBUGF(TCP_DEBUG, ("tcp_zero_window_probe: seqno %"U32_F
                          " ackno %"U32_F" err %d.\n",
                          pcb->snd_nxt - 1, pcb->rcv_nxt, (int)err));
  return err;
}
#endif /* LWIP_TCP */
