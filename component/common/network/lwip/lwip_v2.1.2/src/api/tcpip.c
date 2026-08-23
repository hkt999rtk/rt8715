/**
 * @file
 * Sequential API Main thread module
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

#include "lwip/opt.h"

#if !NO_SYS /* don't build if not configured for use in lwipopts.h */

#include "lwip/priv/tcpip_priv.h"
#include "lwip/sys.h"
#include "lwip/memp.h"
#include "lwip/mem.h"
#include "lwip/init.h"
#include "lwip/ip.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#ifndef CONFIG_TCPIP_RX_BATCH_STAGE1
#define CONFIG_TCPIP_RX_BATCH_STAGE1 0
#endif

#ifndef CONFIG_TCPIP_RX_BATCH_TIMER_PROBE
#define CONFIG_TCPIP_RX_BATCH_TIMER_PROBE 0
#endif

#ifndef CONFIG_TCPIP_RX_BATCH_TIMER_MBOX_PROBE
#define CONFIG_TCPIP_RX_BATCH_TIMER_MBOX_PROBE 0
#endif

#ifndef CONFIG_TCPIP_RX_BATCH_STAGE3
#define CONFIG_TCPIP_RX_BATCH_STAGE3 0
#endif

#ifndef CONFIG_TCPIP_RX_BATCH_MAX_PACKETS
#define CONFIG_TCPIP_RX_BATCH_MAX_PACKETS 8U
#endif

#ifndef CONFIG_TCPIP_RX_BATCH_TIMEOUT_US
#define CONFIG_TCPIP_RX_BATCH_TIMEOUT_US 1000U
#endif

#ifndef CONFIG_TCPIP_RX_BATCH_PROFILE
#define CONFIG_TCPIP_RX_BATCH_PROFILE 0
#endif

#ifndef CONFIG_TCPIP_NCM_RX_PRIORITY
#define CONFIG_TCPIP_NCM_RX_PRIORITY 0
#endif

#if (CONFIG_TCPIP_RX_BATCH_STAGE1 || CONFIG_TCPIP_NCM_RX_PRIORITY) && \
    !LWIP_TCPIP_CORE_LOCKING_INPUT
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#endif

#if CONFIG_TCPIP_RX_BATCH_STAGE1 && !LWIP_TCPIP_CORE_LOCKING_INPUT
#if CONFIG_TCPIP_RX_BATCH_TIMER_PROBE
#include "hal_timer.h"
#endif
#endif

#ifndef CONFIG_NET_QUEUE_PROFILE
#define CONFIG_NET_QUEUE_PROFILE 0
#endif

#if CONFIG_NET_QUEUE_PROFILE
#include "lwip/priv/tcp_priv.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "net_queue_profiler.h"
#endif

#define TCPIP_MSG_VAR_REF(name)     API_VAR_REF(name)
#define TCPIP_MSG_VAR_DECLARE(name) API_VAR_DECLARE(struct tcpip_msg, name)
#define TCPIP_MSG_VAR_ALLOC(name)   API_VAR_ALLOC(struct tcpip_msg, MEMP_TCPIP_MSG_API, name, ERR_MEM)
#define TCPIP_MSG_VAR_FREE(name)    API_VAR_FREE(MEMP_TCPIP_MSG_API, name)

/* global variables */
static tcpip_init_done_fn tcpip_init_done;
static void *tcpip_init_done_arg;
static sys_mbox_t tcpip_mbox;

#if CONFIG_TCPIP_NCM_RX_PRIORITY && !LWIP_TCPIP_CORE_LOCKING_INPUT
#define TCPIP_NCM_RX_POOL_SIZE 128U
#define TCPIP_NCM_RX_INVALID_INDEX 0xffffU

struct tcpip_ncm_rx_entry {
  struct pbuf *p;
  struct netif *netif;
  netif_input_fn input_fn;
  u16_t next;
};

static struct tcpip_ncm_rx_entry tcpip_ncm_rx_pool[TCPIP_NCM_RX_POOL_SIZE];
static u16_t tcpip_ncm_rx_free_head = TCPIP_NCM_RX_INVALID_INDEX;
static u16_t tcpip_ncm_rx_pending_head = TCPIP_NCM_RX_INVALID_INDEX;
static u16_t tcpip_ncm_rx_pending_tail = TCPIP_NCM_RX_INVALID_INDEX;
static u8_t tcpip_ncm_rx_marker_pending;
static struct tcpip_msg tcpip_ncm_rx_marker;

static int
tcpip_ncm_rx_eligible(const struct netif *inp)
{
  return inp != NULL && inp->name[0] == 'e' && inp->name[1] == 'n';
}

static void
tcpip_ncm_rx_free_locked(u16_t index)
{
  tcpip_ncm_rx_pool[index].next = tcpip_ncm_rx_free_head;
  tcpip_ncm_rx_free_head = index;
}

static void
tcpip_ncm_rx_drain(void *arg)
{
  LWIP_UNUSED_ARG(arg);

  for (;;) {
    struct tcpip_ncm_rx_entry *entry;
    struct pbuf *p;
    struct netif *netif;
    netif_input_fn input_fn;
    u16_t index;

    taskENTER_CRITICAL();
    index = tcpip_ncm_rx_pending_head;
    if (index == TCPIP_NCM_RX_INVALID_INDEX) {
      tcpip_ncm_rx_pending_tail = TCPIP_NCM_RX_INVALID_INDEX;
      tcpip_ncm_rx_marker_pending = 0U;
      taskEXIT_CRITICAL();
      break;
    }
    entry = &tcpip_ncm_rx_pool[index];
    tcpip_ncm_rx_pending_head = entry->next;
    if (tcpip_ncm_rx_pending_head == TCPIP_NCM_RX_INVALID_INDEX) {
      tcpip_ncm_rx_pending_tail = TCPIP_NCM_RX_INVALID_INDEX;
    }
    p = entry->p;
    netif = entry->netif;
    input_fn = entry->input_fn;
    taskEXIT_CRITICAL();

    if (input_fn(p, netif) != ERR_OK) {
      pbuf_free(p);
    }

    taskENTER_CRITICAL();
    tcpip_ncm_rx_free_locked(index);
    taskEXIT_CRITICAL();
  }
}

static err_t
tcpip_ncm_rx_enqueue(struct pbuf *p, struct netif *inp,
                     netif_input_fn input_fn)
{
  struct tcpip_ncm_rx_entry *entry;
  struct tcpip_msg *marker = &tcpip_ncm_rx_marker;
  u16_t index;

  taskENTER_CRITICAL();
  index = tcpip_ncm_rx_free_head;
  if (index == TCPIP_NCM_RX_INVALID_INDEX) {
    taskEXIT_CRITICAL();
    return ERR_MEM;
  }
  entry = &tcpip_ncm_rx_pool[index];
  tcpip_ncm_rx_free_head = entry->next;
  entry->p = p;
  entry->netif = inp;
  entry->input_fn = input_fn;
  entry->next = TCPIP_NCM_RX_INVALID_INDEX;

  if (tcpip_ncm_rx_pending_tail == TCPIP_NCM_RX_INVALID_INDEX) {
    tcpip_ncm_rx_pending_head = index;
  } else {
    tcpip_ncm_rx_pool[tcpip_ncm_rx_pending_tail].next = index;
  }
  tcpip_ncm_rx_pending_tail = index;

  if (tcpip_ncm_rx_marker_pending == 0U) {
    tcpip_ncm_rx_marker_pending = 1U;
    if (xQueueSendToFront(tcpip_mbox, &marker, 0) != pdPASS) {
      tcpip_ncm_rx_pending_head = TCPIP_NCM_RX_INVALID_INDEX;
      tcpip_ncm_rx_pending_tail = TCPIP_NCM_RX_INVALID_INDEX;
      tcpip_ncm_rx_marker_pending = 0U;
      tcpip_ncm_rx_free_locked(index);
      taskEXIT_CRITICAL();
      return ERR_MEM;
    }
  }
  taskEXIT_CRITICAL();
  return ERR_OK;
}

static void
tcpip_ncm_rx_priority_init(void)
{
  u32_t i;

  for (i = 0U; i < TCPIP_NCM_RX_POOL_SIZE; i++) {
    tcpip_ncm_rx_pool[i].next =
      (i + 1U < TCPIP_NCM_RX_POOL_SIZE) ?
      (u16_t)(i + 1U) : TCPIP_NCM_RX_INVALID_INDEX;
  }
  tcpip_ncm_rx_free_head = 0U;
  tcpip_ncm_rx_pending_head = TCPIP_NCM_RX_INVALID_INDEX;
  tcpip_ncm_rx_pending_tail = TCPIP_NCM_RX_INVALID_INDEX;
  tcpip_ncm_rx_marker_pending = 0U;
  tcpip_ncm_rx_marker.type = TCPIP_MSG_CALLBACK_STATIC;
  tcpip_ncm_rx_marker.msg.cb.function = tcpip_ncm_rx_drain;
  tcpip_ncm_rx_marker.msg.cb.ctx = NULL;
  LWIP_PLATFORM_DIAG(("[NCMRXPRIO] enabled pool=%u mode=fifo-front-marker\n",
                      (unsigned)TCPIP_NCM_RX_POOL_SIZE));
}
#endif

#if CONFIG_TCPIP_RX_BATCH_STAGE1 && !LWIP_TCPIP_CORE_LOCKING_INPUT
#define TCPIP_RX_STAGE1_POOL_SIZE 128U
#define TCPIP_RX_STAGE1_INVALID_INDEX 0xffffU
#if CONFIG_TCPIP_RX_BATCH_MAX_PACKETS == 0 || \
    CONFIG_TCPIP_RX_BATCH_MAX_PACKETS > TCPIP_RX_STAGE1_POOL_SIZE
#error "CONFIG_TCPIP_RX_BATCH_MAX_PACKETS must be in range 1..128"
#endif

enum tcpip_rx_stage1_state {
  TCPIP_RX_STAGE1_FREE = 0,
  TCPIP_RX_STAGE1_QUEUED
};

struct tcpip_rx_stage1_msg {
  struct tcpip_msg msg;
  struct pbuf *p;
  struct netif *netif;
  netif_input_fn input_fn;
  u32_t sequence;
  u32_t enqueue_cycles;
  u16_t batch_count;
  u16_t next_free;
  u16_t next_pending;
  u8_t state;
};

static struct tcpip_rx_stage1_msg
  tcpip_rx_stage1_pool[TCPIP_RX_STAGE1_POOL_SIZE];
static u16_t tcpip_rx_stage1_free_head = TCPIP_RX_STAGE1_INVALID_INDEX;
static u32_t tcpip_rx_stage1_sequence;
static u32_t tcpip_rx_stage1_in_use;
#if CONFIG_TCPIP_RX_BATCH_STAGE3
static u16_t tcpip_rx_batch_head = TCPIP_RX_STAGE1_INVALID_INDEX;
static u16_t tcpip_rx_batch_tail = TCPIP_RX_STAGE1_INVALID_INDEX;
static volatile u32_t tcpip_rx_batch_count;

static int
tcpip_rx_batch_post_task_locked(u32_t trigger)
{
  u16_t head = tcpip_rx_batch_head;
  u16_t tail = tcpip_rx_batch_tail;
  u32_t count = tcpip_rx_batch_count;
  struct tcpip_rx_stage1_msg *head_entry;
  struct tcpip_msg *msg;

  if (head == TCPIP_RX_STAGE1_INVALID_INDEX || count == 0U) {
    return 1;
  }
  head_entry = &tcpip_rx_stage1_pool[head];
  head_entry->batch_count = (u16_t)count;
  head_entry->msg.type = TCPIP_MSG_INPKT_BATCH;
  head_entry->msg.msg.inp_stage1 = head_entry;
  msg = &head_entry->msg;
  tcpip_rx_batch_head = TCPIP_RX_STAGE1_INVALID_INDEX;
  tcpip_rx_batch_tail = TCPIP_RX_STAGE1_INVALID_INDEX;
  tcpip_rx_batch_count = 0U;
  if (xQueueSend(tcpip_mbox, &msg, 0) != pdPASS) {
    tcpip_rx_batch_head = head;
    tcpip_rx_batch_tail = tail;
    tcpip_rx_batch_count = count;
#if CONFIG_TCPIP_RX_BATCH_PROFILE
    net_queue_profiler_rx_batch_post_fail(trigger);
#endif
    return 0;
  }
#if CONFIG_TCPIP_RX_BATCH_PROFILE
  net_queue_profiler_rx_batch_post(trigger);
#endif
  return 1;
}
#endif

#if CONFIG_TCPIP_RX_BATCH_TIMER_PROBE
#if CONFIG_TCPIP_RX_BATCH_STAGE3
#define TCPIP_RX_TIMER_PROBE_US CONFIG_TCPIP_RX_BATCH_TIMEOUT_US
#else
#define TCPIP_RX_TIMER_PROBE_US 1000U
#endif
static hal_timer_adapter_t tcpip_rx_timer_probe;
static volatile u32_t tcpip_rx_timer_probe_armed;
static volatile u32_t tcpip_rx_timer_probe_start_cycles;
static u32_t tcpip_rx_timer_probe_available;
static timer_id_t tcpip_rx_timer_probe_id = MaxGTimerNum;
#if CONFIG_TCPIP_RX_BATCH_TIMER_MBOX_PROBE
static struct tcpip_msg tcpip_rx_timer_probe_msg;
#endif

static void
tcpip_rx_timer_probe_callback(void *arg)
{
#if CONFIG_TCPIP_RX_BATCH_PROFILE
  u32_t elapsed_cycles = DWT->CYCCNT - tcpip_rx_timer_probe_start_cycles;
  u32_t elapsed_us = SystemCoreClock != 0U ?
    (u32_t)(((uint64_t)elapsed_cycles * 1000000U) / SystemCoreClock) : 0U;
#endif
#if CONFIG_TCPIP_RX_BATCH_TIMER_MBOX_PROBE || CONFIG_TCPIP_RX_BATCH_STAGE3
  struct tcpip_msg *msg;
  BaseType_t task_woken = pdFALSE;
#endif

  LWIP_UNUSED_ARG(arg);
#if CONFIG_TCPIP_RX_BATCH_PROFILE
  net_queue_profiler_rx_timer_fire(elapsed_us);
#endif
#if CONFIG_TCPIP_RX_BATCH_STAGE3
  {
    u16_t head = tcpip_rx_batch_head;
    u16_t tail = tcpip_rx_batch_tail;
    u32_t count = tcpip_rx_batch_count;

  tcpip_rx_timer_probe_armed = 0U;
  if (head != TCPIP_RX_STAGE1_INVALID_INDEX && count != 0U) {
    struct tcpip_rx_stage1_msg *head_entry = &tcpip_rx_stage1_pool[head];

    head_entry->batch_count = (u16_t)count;
    head_entry->msg.type = TCPIP_MSG_INPKT_BATCH;
    head_entry->msg.msg.inp_stage1 = head_entry;
    msg = &head_entry->msg;
    tcpip_rx_batch_head = TCPIP_RX_STAGE1_INVALID_INDEX;
    tcpip_rx_batch_tail = TCPIP_RX_STAGE1_INVALID_INDEX;
    tcpip_rx_batch_count = 0U;
    if (xQueueSendFromISR(tcpip_mbox, &msg, &task_woken) == pdPASS) {
#if CONFIG_TCPIP_RX_BATCH_PROFILE
      net_queue_profiler_rx_batch_post(1U);
#endif
      portYIELD_FROM_ISR(task_woken);
    } else {
      tcpip_rx_batch_head = head;
      tcpip_rx_batch_tail = tail;
      tcpip_rx_batch_count = count;
#if CONFIG_TCPIP_RX_BATCH_PROFILE
      net_queue_profiler_rx_batch_post_fail(1U);
#endif
    }
  } else {
#if CONFIG_TCPIP_RX_BATCH_PROFILE
    net_queue_profiler_rx_batch_timer_empty();
#endif
  }
  }
#elif CONFIG_TCPIP_RX_BATCH_TIMER_MBOX_PROBE
  msg = &tcpip_rx_timer_probe_msg;
  if (xQueueSendFromISR(tcpip_mbox, &msg, &task_woken) == pdPASS) {
#if CONFIG_TCPIP_RX_BATCH_PROFILE
    net_queue_profiler_rx_timer_mbox_post(task_woken != pdFALSE);
#endif
    portYIELD_FROM_ISR(task_woken);
  } else {
    tcpip_rx_timer_probe_armed = 0U;
#if CONFIG_TCPIP_RX_BATCH_PROFILE
    net_queue_profiler_rx_timer_mbox_fail();
#endif
  }
#else
  tcpip_rx_timer_probe_armed = 0U;
#endif
}

static void
tcpip_rx_timer_probe_arm_locked(void)
{
  if (tcpip_rx_timer_probe_available == 0U) {
    return;
  }
  if (tcpip_rx_timer_probe_armed != 0U) {
#if CONFIG_TCPIP_RX_BATCH_PROFILE
    net_queue_profiler_rx_timer_skip();
#endif
    return;
  }
  tcpip_rx_timer_probe_armed = 1U;
#if CONFIG_TCPIP_RX_BATCH_PROFILE
  tcpip_rx_timer_probe_start_cycles = DWT->CYCCNT;
  net_queue_profiler_rx_timer_arm();
#endif
  hal_timer_start_one_shot(&tcpip_rx_timer_probe, TCPIP_RX_TIMER_PROBE_US,
                           tcpip_rx_timer_probe_callback, NULL);
}

static void
tcpip_rx_timer_probe_init(void)
{
#if CONFIG_TCPIP_RX_BATCH_PROFILE
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif
  tcpip_rx_timer_probe_id = hal_timer_allocate(NULL);
  if (tcpip_rx_timer_probe_id >= MaxGTimerNum ||
      hal_timer_init(&tcpip_rx_timer_probe, tcpip_rx_timer_probe_id) != HAL_OK) {
#if CONFIG_TCPIP_RX_BATCH_PROFILE
    net_queue_profiler_rx_timer_init((u32_t)tcpip_rx_timer_probe_id, 0U);
#endif
    LWIP_PLATFORM_DIAG(("[RXBATCH2] ERROR: no GTimer available; probe disabled\n"));
    return;
  }
  tcpip_rx_timer_probe_available = 1U;
#if CONFIG_TCPIP_RX_BATCH_TIMER_MBOX_PROBE
  tcpip_rx_timer_probe_msg.type = TCPIP_MSG_RX_TIMER_PROBE;
#endif
#if CONFIG_TCPIP_RX_BATCH_PROFILE
  net_queue_profiler_rx_timer_init((u32_t)tcpip_rx_timer_probe_id, 1U);
#endif
  LWIP_PLATFORM_DIAG(("[RXBATCH2] enabled timer=%u delay_us=%u; callback=%s\n",
                      (unsigned)tcpip_rx_timer_probe_id,
                      (unsigned)TCPIP_RX_TIMER_PROBE_US,
#if CONFIG_TCPIP_RX_BATCH_STAGE3
                      "batch-dispatch"
#else
                      "diagnostic-only"
#endif
                      ));
}
#endif

static int
tcpip_rx_stage1_eligible(const struct netif *inp)
{
  /* Stage 1 only replaces the WLAN mailbox envelope.  NCM and every other
   * netif retain the original MEMP_TCPIP_MSG_INPKT path. */
  return inp != NULL &&
         ((inp->name[0] == 'a' && inp->name[1] == 'p') ||
          (inp->name[0] == 'w' && inp->name[1] == 'l'));
}

static struct tcpip_rx_stage1_msg *
tcpip_rx_stage1_alloc_locked(void)
{
  struct tcpip_rx_stage1_msg *entry;
  u16_t index = tcpip_rx_stage1_free_head;

  if (index == TCPIP_RX_STAGE1_INVALID_INDEX) {
    return NULL;
  }
  entry = &tcpip_rx_stage1_pool[index];
  tcpip_rx_stage1_free_head = entry->next_free;
  entry->next_free = TCPIP_RX_STAGE1_INVALID_INDEX;
  entry->state = TCPIP_RX_STAGE1_QUEUED;
  tcpip_rx_stage1_in_use++;
  return entry;
}

static void
tcpip_rx_stage1_free_locked(struct tcpip_rx_stage1_msg *entry)
{
  u16_t index = (u16_t)(entry - tcpip_rx_stage1_pool);

  entry->state = TCPIP_RX_STAGE1_FREE;
  entry->next_free = tcpip_rx_stage1_free_head;
  tcpip_rx_stage1_free_head = index;
  if (tcpip_rx_stage1_in_use > 0U) {
    tcpip_rx_stage1_in_use--;
  }
}

static void
tcpip_rx_stage1_init(void)
{
  u32_t i;

  for (i = 0U; i < TCPIP_RX_STAGE1_POOL_SIZE; i++) {
    tcpip_rx_stage1_pool[i].state = TCPIP_RX_STAGE1_FREE;
    tcpip_rx_stage1_pool[i].next_pending = TCPIP_RX_STAGE1_INVALID_INDEX;
    tcpip_rx_stage1_pool[i].next_free =
      (i + 1U < TCPIP_RX_STAGE1_POOL_SIZE) ?
      (u16_t)(i + 1U) : TCPIP_RX_STAGE1_INVALID_INDEX;
  }
  tcpip_rx_stage1_free_head = 0U;
#if CONFIG_TCPIP_RX_BATCH_STAGE3
  LWIP_PLATFORM_DIAG(("[RXBATCH3] enabled pool=%u max_packets=%u timeout_us=%u; pointer-only, no payload copy\n",
                      (unsigned)TCPIP_RX_STAGE1_POOL_SIZE,
                      (unsigned)CONFIG_TCPIP_RX_BATCH_MAX_PACKETS,
                      (unsigned)CONFIG_TCPIP_RX_BATCH_TIMEOUT_US));
#else
  LWIP_PLATFORM_DIAG(("[RXBATCH1] enabled mode=single-immediate pool=%u; no aggregation, delay, or timer\n",
                      (unsigned)TCPIP_RX_STAGE1_POOL_SIZE));
#endif
}
#endif

#if LWIP_TCPIP_CORE_LOCKING
/** The global semaphore to lock the stack. */
sys_mutex_t lock_tcpip_core;
#endif /* LWIP_TCPIP_CORE_LOCKING */

static void tcpip_thread_handle_msg(struct tcpip_msg *msg);

#if CONFIG_NET_QUEUE_PROFILE
static u32_t
tcpip_queue_profile_netif_id(const struct netif *netif)
{
  if (netif == NULL) {
    return 0U;
  }
  return (u32_t)netif_get_index(netif) |
         ((u32_t)netif->num << 8) |
         ((u32_t)(u8_t)netif->name[0] << 16) |
         ((u32_t)(u8_t)netif->name[1] << 24);
}
#endif

#if CONFIG_NET_QUEUE_PROFILE && LWIP_TCP
/* Sampling once per 256 handled messages keeps list-walking overhead small
 * while still providing several snapshots per second under streaming load. */
static void
tcpip_queue_profile_sample(void)
{
  static u8_t divider;
  struct tcp_pcb *pcb;
  u32_t active_pcbs = 0;
  u32_t snd_queue_len = 0;
  u32_t unsent_segments = 0;
  u32_t unacked_segments = 0;
  u32_t queued_bytes = 0;
  u32_t refused_pbufs = 0;
  u32_t refused_bytes = 0;

  divider++;
  if (divider != 0U) {
    return;
  }

  for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
    struct tcp_seg *seg;
    struct pbuf *p;

    active_pcbs++;
    snd_queue_len += pcb->snd_queuelen;
    for (seg = pcb->unsent; seg != NULL; seg = seg->next) {
      unsent_segments++;
      queued_bytes += seg->len;
    }
    for (seg = pcb->unacked; seg != NULL; seg = seg->next) {
      unacked_segments++;
      queued_bytes += seg->len;
    }
    for (p = pcb->refused_data; p != NULL; p = p->next) {
      refused_pbufs++;
      refused_bytes += p->len;
    }
  }

  net_queue_profiler_tcp_sample(active_pcbs, snd_queue_len,
                                unsent_segments, unacked_segments,
                                queued_bytes, refused_pbufs, refused_bytes);
}
#else
#define tcpip_queue_profile_sample() do { } while (0)
#endif

#if !LWIP_TIMERS
/* wait for a message with timers disabled (e.g. pass a timer-check trigger into tcpip_thread) */
#define TCPIP_MBOX_FETCH(mbox, msg) sys_mbox_fetch(mbox, msg)
#else /* !LWIP_TIMERS */
/* wait for a message, timeouts are processed while waiting */
#define TCPIP_MBOX_FETCH(mbox, msg) tcpip_timeouts_mbox_fetch(mbox, msg)
/**
 * Wait (forever) for a message to arrive in an mbox.
 * While waiting, timeouts are processed.
 *
 * @param mbox the mbox to fetch the message from
 * @param msg the place to store the message
 */
static void
tcpip_timeouts_mbox_fetch(sys_mbox_t *mbox, void **msg)
{
  u32_t sleeptime, res;

again:
  LWIP_ASSERT_CORE_LOCKED();

  sleeptime = sys_timeouts_sleeptime();
  if (sleeptime == SYS_TIMEOUTS_SLEEPTIME_INFINITE) {
    UNLOCK_TCPIP_CORE();
    sys_arch_mbox_fetch(mbox, msg, 0);
    LOCK_TCPIP_CORE();
    return;
  } else if (sleeptime == 0) {
    sys_check_timeouts();
    /* We try again to fetch a message from the mbox. */
    goto again;
  }

  UNLOCK_TCPIP_CORE();
  res = sys_arch_mbox_fetch(mbox, msg, sleeptime);
  LOCK_TCPIP_CORE();
  if (res == SYS_ARCH_TIMEOUT) {
    /* If a SYS_ARCH_TIMEOUT value is returned, a timeout occurred
       before a message could be fetched. */
    sys_check_timeouts();
    /* We try again to fetch a message from the mbox. */
    goto again;
  }
}
#endif /* !LWIP_TIMERS */

/**
 * The main lwIP thread. This thread has exclusive access to lwIP core functions
 * (unless access to them is not locked). Other threads communicate with this
 * thread using message boxes.
 *
 * It also starts all the timers to make sure they are running in the right
 * thread context.
 *
 * @param arg unused argument
 */
static void
tcpip_thread(void *arg)
{
  struct tcpip_msg *msg;
  LWIP_UNUSED_ARG(arg);

  LWIP_MARK_TCPIP_THREAD();

  LOCK_TCPIP_CORE();
  if (tcpip_init_done != NULL) {
    tcpip_init_done(tcpip_init_done_arg);
  }

  while (1) {                          /* MAIN Loop */
    LWIP_TCPIP_THREAD_ALIVE();
    /* wait for a message, timeouts are processed while waiting */
    TCPIP_MBOX_FETCH(&tcpip_mbox, (void **)&msg);
    if (msg == NULL) {
      LWIP_DEBUGF(TCPIP_DEBUG, ("tcpip_thread: invalid message: NULL\n"));
      LWIP_ASSERT("tcpip_thread: invalid message", 0);
      continue;
    }
    tcpip_thread_handle_msg(msg);
    tcpip_queue_profile_sample();
  }
}

/* Handle a single tcpip_msg
 * This is in its own function for access by tests only.
 */
static void
tcpip_thread_handle_msg(struct tcpip_msg *msg)
{
  switch (msg->type) {
#if !LWIP_TCPIP_CORE_LOCKING
    case TCPIP_MSG_API:
      LWIP_DEBUGF(TCPIP_DEBUG, ("tcpip_thread: API message %p\n", (void *)msg));
      msg->msg.api_msg.function(msg->msg.api_msg.msg);
      break;
    case TCPIP_MSG_API_CALL:
      LWIP_DEBUGF(TCPIP_DEBUG, ("tcpip_thread: API CALL message %p\n", (void *)msg));
      msg->msg.api_call.arg->err = msg->msg.api_call.function(msg->msg.api_call.arg);
      sys_sem_signal(msg->msg.api_call.sem);
      break;
#endif /* !LWIP_TCPIP_CORE_LOCKING */

#if !LWIP_TCPIP_CORE_LOCKING_INPUT
    case TCPIP_MSG_INPKT_BATCH:
#if CONFIG_TCPIP_RX_BATCH_STAGE3
    {
      struct tcpip_rx_stage1_msg *head_entry =
        (struct tcpip_rx_stage1_msg *)msg->msg.inp_stage1;
      u16_t index;
      u32_t packets;
      u32_t processed = 0U;
      u32_t pending_remaining;
      u32_t oldest_age_us = 0U;

      if (head_entry == NULL ||
          (uintptr_t)head_entry < (uintptr_t)&tcpip_rx_stage1_pool[0] ||
          (uintptr_t)head_entry >=
            (uintptr_t)&tcpip_rx_stage1_pool[TCPIP_RX_STAGE1_POOL_SIZE]) {
#if CONFIG_TCPIP_RX_BATCH_PROFILE
        net_queue_profiler_rx_stage1_state_error();
#endif
        LWIP_ASSERT("Invalid sealed RX batch", 0);
        break;
      }
      index = (u16_t)(head_entry - tcpip_rx_stage1_pool);
      packets = head_entry->batch_count;
      if (packets == 0U || packets > CONFIG_TCPIP_RX_BATCH_MAX_PACKETS) {
#if CONFIG_TCPIP_RX_BATCH_PROFILE
        net_queue_profiler_rx_stage1_state_error();
#endif
        LWIP_ASSERT("Invalid sealed RX batch count", 0);
        break;
      }
      taskENTER_CRITICAL();
      pending_remaining = tcpip_rx_batch_count;
#if CONFIG_TCPIP_RX_BATCH_PROFILE
      if (SystemCoreClock != 0U) {
        u32_t age_cycles = DWT->CYCCNT -
          head_entry->enqueue_cycles;
        oldest_age_us = (u32_t)(((uint64_t)age_cycles * 1000000U) /
                                SystemCoreClock);
      }
#endif
      taskEXIT_CRITICAL();
#if CONFIG_TCPIP_RX_BATCH_PROFILE
      net_queue_profiler_rx_batch_dispatch(packets, oldest_age_us,
                                           pending_remaining);
#else
      LWIP_UNUSED_ARG(packets);
      LWIP_UNUSED_ARG(oldest_age_us);
      LWIP_UNUSED_ARG(pending_remaining);
#endif

      while (index != TCPIP_RX_STAGE1_INVALID_INDEX && processed < packets) {
        struct tcpip_rx_stage1_msg *entry = &tcpip_rx_stage1_pool[index];
        u16_t next = entry->next_pending;
        struct pbuf *p = entry->p;
        struct netif *netif = entry->netif;
        u32_t sequence = entry->sequence;
        u32_t in_use;

        if (entry->state != TCPIP_RX_STAGE1_QUEUED) {
#if CONFIG_TCPIP_RX_BATCH_PROFILE
          net_queue_profiler_rx_stage1_state_error();
#endif
          LWIP_ASSERT("Invalid sealed RX entry state", 0);
          break;
        }

#if CONFIG_NET_QUEUE_PROFILE
        net_queue_profiler_rx_consume(tcpip_queue_profile_netif_id(netif),
                                      p->tot_len,
                                      uxQueueMessagesWaiting(tcpip_mbox));
#endif
        if (entry->input_fn(p, netif) != ERR_OK) {
          pbuf_free(p);
        }
        taskENTER_CRITICAL();
        tcpip_rx_stage1_free_locked(entry);
        in_use = tcpip_rx_stage1_in_use;
        taskEXIT_CRITICAL();
#if CONFIG_TCPIP_RX_BATCH_PROFILE
        net_queue_profiler_rx_stage1_consume(sequence, in_use);
#else
        LWIP_UNUSED_ARG(sequence);
        LWIP_UNUSED_ARG(in_use);
#endif
        index = next;
        processed++;
      }
      if (processed != packets || index != TCPIP_RX_STAGE1_INVALID_INDEX) {
#if CONFIG_TCPIP_RX_BATCH_PROFILE
        net_queue_profiler_rx_stage1_state_error();
#endif
        LWIP_ASSERT("Sealed RX batch length mismatch", 0);
      }
      break;
    }
#else
      LWIP_ASSERT("RX batch message while disabled", 0);
      break;
#endif

    case TCPIP_MSG_RX_TIMER_PROBE:
#if CONFIG_TCPIP_RX_BATCH_TIMER_PROBE && CONFIG_TCPIP_RX_BATCH_TIMER_MBOX_PROBE
      /* Stage 2B deliberately carries no packet. It validates only that a
       * one-shot GTimer ISR can wake and dispatch through tcpip_mbox. */
      tcpip_rx_timer_probe_armed = 0U;
#if CONFIG_TCPIP_RX_BATCH_PROFILE
      net_queue_profiler_rx_timer_mbox_dispatch();
#endif
#else
      LWIP_ASSERT("RX timer probe message while disabled", 0);
#endif
      break;

    case TCPIP_MSG_INPKT_STAGE1:
#if CONFIG_TCPIP_RX_BATCH_STAGE1
    {
      struct tcpip_rx_stage1_msg *entry =
        (struct tcpip_rx_stage1_msg *)msg->msg.inp_stage1;
      struct pbuf *p;
      struct netif *netif;
      netif_input_fn input_fn;
      u32_t sequence;
      u32_t in_use;

      if (entry == NULL || entry->state != TCPIP_RX_STAGE1_QUEUED) {
#if CONFIG_TCPIP_RX_BATCH_PROFILE
        net_queue_profiler_rx_stage1_state_error();
#endif
        LWIP_ASSERT("Invalid stage-1 RX message", 0);
        break;
      }
      p = entry->p;
      netif = entry->netif;
      input_fn = entry->input_fn;
      sequence = entry->sequence;
#if CONFIG_NET_QUEUE_PROFILE
      net_queue_profiler_rx_consume(tcpip_queue_profile_netif_id(netif),
                                    p->tot_len,
                                    uxQueueMessagesWaiting(tcpip_mbox));
#endif
      if (input_fn(p, netif) != ERR_OK) {
        pbuf_free(p);
      }
      taskENTER_CRITICAL();
      tcpip_rx_stage1_free_locked(entry);
      in_use = tcpip_rx_stage1_in_use;
      taskEXIT_CRITICAL();
#if CONFIG_TCPIP_RX_BATCH_PROFILE
      net_queue_profiler_rx_stage1_consume(sequence, in_use);
#else
      LWIP_UNUSED_ARG(sequence);
      LWIP_UNUSED_ARG(in_use);
#endif
      break;
    }
#else
      LWIP_ASSERT("Stage-1 RX message while disabled", 0);
      break;
#endif

    case TCPIP_MSG_INPKT:
      LWIP_DEBUGF(TCPIP_DEBUG, ("tcpip_thread: PACKET %p\n", (void *)msg));
#if CONFIG_NET_QUEUE_PROFILE
      net_queue_profiler_rx_consume(
                                    tcpip_queue_profile_netif_id(msg->msg.inp.netif),
                                    msg->msg.inp.p->tot_len,
                                    uxQueueMessagesWaiting(tcpip_mbox));
#endif
      if (msg->msg.inp.input_fn(msg->msg.inp.p, msg->msg.inp.netif) != ERR_OK) {
        pbuf_free(msg->msg.inp.p);
      }
      memp_free(MEMP_TCPIP_MSG_INPKT, msg);
      break;
#endif /* !LWIP_TCPIP_CORE_LOCKING_INPUT */

#if LWIP_TCPIP_TIMEOUT && LWIP_TIMERS
    case TCPIP_MSG_TIMEOUT:
      LWIP_DEBUGF(TCPIP_DEBUG, ("tcpip_thread: TIMEOUT %p\n", (void *)msg));
      sys_timeout(msg->msg.tmo.msecs, msg->msg.tmo.h, msg->msg.tmo.arg);
      memp_free(MEMP_TCPIP_MSG_API, msg);
      break;
    case TCPIP_MSG_UNTIMEOUT:
      LWIP_DEBUGF(TCPIP_DEBUG, ("tcpip_thread: UNTIMEOUT %p\n", (void *)msg));
      sys_untimeout(msg->msg.tmo.h, msg->msg.tmo.arg);
      memp_free(MEMP_TCPIP_MSG_API, msg);
      break;
#endif /* LWIP_TCPIP_TIMEOUT && LWIP_TIMERS */

    case TCPIP_MSG_CALLBACK:
      LWIP_DEBUGF(TCPIP_DEBUG, ("tcpip_thread: CALLBACK %p\n", (void *)msg));
      msg->msg.cb.function(msg->msg.cb.ctx);
      memp_free(MEMP_TCPIP_MSG_API, msg);
      break;

    case TCPIP_MSG_CALLBACK_STATIC:
      LWIP_DEBUGF(TCPIP_DEBUG, ("tcpip_thread: CALLBACK_STATIC %p\n", (void *)msg));
      msg->msg.cb.function(msg->msg.cb.ctx);
      break;

    default:
      LWIP_DEBUGF(TCPIP_DEBUG, ("tcpip_thread: invalid message: %d\n", msg->type));
      LWIP_ASSERT("tcpip_thread: invalid message", 0);
      break;
  }
}

#ifdef TCPIP_THREAD_TEST
/** Work on queued items in single-threaded test mode */
int
tcpip_thread_poll_one(void)
{
  int ret = 0;
  struct tcpip_msg *msg;

  if (sys_arch_mbox_tryfetch(&tcpip_mbox, (void **)&msg) != SYS_ARCH_TIMEOUT) {
    LOCK_TCPIP_CORE();
    if (msg != NULL) {
      tcpip_thread_handle_msg(msg);
      ret = 1;
    }
    UNLOCK_TCPIP_CORE();
  }
  return ret;
}
#endif

/**
 * Pass a received packet to tcpip_thread for input processing
 *
 * @param p the received packet
 * @param inp the network interface on which the packet was received
 * @param input_fn input function to call
 */
err_t
tcpip_inpkt(struct pbuf *p, struct netif *inp, netif_input_fn input_fn)
{
#if LWIP_TCPIP_CORE_LOCKING_INPUT
  err_t ret;
  LWIP_DEBUGF(TCPIP_DEBUG, ("tcpip_inpkt: PACKET %p/%p\n", (void *)p, (void *)inp));
  LOCK_TCPIP_CORE();
  ret = input_fn(p, inp);
  UNLOCK_TCPIP_CORE();
  return ret;
#else /* LWIP_TCPIP_CORE_LOCKING_INPUT */
  struct tcpip_msg *msg;
#if CONFIG_NET_QUEUE_PROFILE
  u32_t netq_if_id = tcpip_queue_profile_netif_id(inp);
  u32_t netq_bytes = p->tot_len;
#endif

  LWIP_ASSERT("Invalid mbox", sys_mbox_valid_val(tcpip_mbox));

#if CONFIG_TCPIP_NCM_RX_PRIORITY
  if (tcpip_ncm_rx_eligible(inp)) {
    return tcpip_ncm_rx_enqueue(p, inp, input_fn);
  }
#endif

#if CONFIG_TCPIP_RX_BATCH_STAGE1
  if (tcpip_rx_stage1_eligible(inp)) {
    struct tcpip_rx_stage1_msg *entry;
    struct tcpip_msg *stage1_msg;
    u32_t sequence;
    u32_t in_use;

    taskENTER_CRITICAL();
    entry = tcpip_rx_stage1_alloc_locked();
    if (entry != NULL) {
      sequence = ++tcpip_rx_stage1_sequence;
      entry->p = p;
      entry->netif = inp;
      entry->input_fn = input_fn;
      entry->sequence = sequence;
#if CONFIG_TCPIP_RX_BATCH_PROFILE
      entry->enqueue_cycles = DWT->CYCCNT;
#else
      entry->enqueue_cycles = 0U;
#endif
      entry->next_pending = TCPIP_RX_STAGE1_INVALID_INDEX;
      entry->msg.type = TCPIP_MSG_INPKT_STAGE1;
      entry->msg.msg.inp_stage1 = entry;
      stage1_msg = &entry->msg;
#if CONFIG_NET_QUEUE_PROFILE
      net_queue_profiler_rx_post_begin(netq_if_id);
#endif
#if CONFIG_TCPIP_RX_BATCH_STAGE3
      if (tcpip_rx_timer_probe_available != 0U) {
        u16_t entry_index = (u16_t)(entry - tcpip_rx_stage1_pool);

        if (tcpip_rx_batch_tail == TCPIP_RX_STAGE1_INVALID_INDEX) {
          tcpip_rx_batch_head = entry_index;
        } else {
          tcpip_rx_stage1_pool[tcpip_rx_batch_tail].next_pending = entry_index;
        }
        tcpip_rx_batch_tail = entry_index;
        tcpip_rx_batch_count++;
        in_use = tcpip_rx_stage1_in_use;
#if CONFIG_NET_QUEUE_PROFILE
        net_queue_profiler_rx_post_commit(
          netq_if_id, netq_bytes, uxQueueMessagesWaiting(tcpip_mbox));
#endif
#if CONFIG_TCPIP_RX_BATCH_PROFILE
        net_queue_profiler_rx_stage1_post(sequence, in_use);
        net_queue_profiler_rx_batch_enqueue(tcpip_rx_batch_count);
#else
        LWIP_UNUSED_ARG(in_use);
#endif
        if (tcpip_rx_batch_count >= CONFIG_TCPIP_RX_BATCH_MAX_PACKETS) {
          (void)tcpip_rx_batch_post_task_locked(0U);
        }
        tcpip_rx_timer_probe_arm_locked();
        taskEXIT_CRITICAL();
        return ERR_OK;
      }
#endif
      if (xQueueSend(tcpip_mbox, &stage1_msg, 0) == pdPASS) {
#if CONFIG_TCPIP_RX_BATCH_TIMER_PROBE && !CONFIG_TCPIP_RX_BATCH_STAGE3
        tcpip_rx_timer_probe_arm_locked();
#endif
        in_use = tcpip_rx_stage1_in_use;
#if CONFIG_NET_QUEUE_PROFILE
        net_queue_profiler_rx_post_commit(
          netq_if_id, netq_bytes, uxQueueMessagesWaiting(tcpip_mbox));
#endif
#if CONFIG_TCPIP_RX_BATCH_PROFILE
        net_queue_profiler_rx_stage1_post(sequence, in_use);
#else
        LWIP_UNUSED_ARG(in_use);
#endif
        taskEXIT_CRITICAL();
        return ERR_OK;
      }
#if CONFIG_NET_QUEUE_PROFILE
      net_queue_profiler_rx_post_abort(netq_if_id);
#endif
#if CONFIG_TCPIP_RX_BATCH_PROFILE
      net_queue_profiler_rx_stage1_post_fail();
#endif
      /* The producer is still inside the critical section, so no later
       * stage-1 message can have consumed this sequence value yet.  Reclaim
       * it to keep a queue-full failure from looking like message loss. */
      tcpip_rx_stage1_sequence--;
      tcpip_rx_stage1_free_locked(entry);
      taskEXIT_CRITICAL();
      return ERR_MEM;
    }
#if CONFIG_TCPIP_RX_BATCH_STAGE3
    /* Preserve FIFO order if the fixed entry pool is ever exhausted: queue
     * the older accumulated packets before falling back to lwIP's envelope. */
    if (tcpip_rx_batch_count != 0U) {
      if (!tcpip_rx_batch_post_task_locked(0U)) {
        taskEXIT_CRITICAL();
#if CONFIG_TCPIP_RX_BATCH_PROFILE
        net_queue_profiler_rx_stage1_pool_fallback();
#endif
        return ERR_MEM;
      }
    }
#endif
    taskEXIT_CRITICAL();
#if CONFIG_TCPIP_RX_BATCH_PROFILE
    net_queue_profiler_rx_stage1_pool_fallback();
#endif
  }
#endif

  msg = (struct tcpip_msg *)memp_malloc(MEMP_TCPIP_MSG_INPKT);
  if (msg == NULL) {
#if CONFIG_NET_QUEUE_PROFILE
    net_queue_profiler_rx_alloc_fail(netq_if_id);
#endif
    return ERR_MEM;
  }

  msg->type = TCPIP_MSG_INPKT;
  msg->msg.inp.p = p;
  msg->msg.inp.netif = inp;
  msg->msg.inp.input_fn = input_fn;
#if CONFIG_NET_QUEUE_PROFILE
  net_queue_profiler_rx_post_begin(netq_if_id);
#endif
  if (sys_mbox_trypost(&tcpip_mbox, msg) != ERR_OK) {
#if CONFIG_NET_QUEUE_PROFILE
    net_queue_profiler_rx_post_abort(netq_if_id);
#endif
    memp_free(MEMP_TCPIP_MSG_INPKT, msg);
    return ERR_MEM;
  }
#if CONFIG_NET_QUEUE_PROFILE
  net_queue_profiler_rx_post_commit(netq_if_id, netq_bytes,
                                    uxQueueMessagesWaiting(tcpip_mbox));
#endif
  return ERR_OK;
#endif /* LWIP_TCPIP_CORE_LOCKING_INPUT */
}

/**
 * @ingroup lwip_os
 * Pass a received packet to tcpip_thread for input processing with
 * ethernet_input or ip_input. Don't call directly, pass to netif_add()
 * and call netif->input().
 *
 * @param p the received packet, p->payload pointing to the Ethernet header or
 *          to an IP header (if inp doesn't have NETIF_FLAG_ETHARP or
 *          NETIF_FLAG_ETHERNET flags)
 * @param inp the network interface on which the packet was received
 */
err_t
tcpip_input(struct pbuf *p, struct netif *inp)
{
#if LWIP_ETHERNET
  if (inp->flags & (NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET)) {
    return tcpip_inpkt(p, inp, ethernet_input);
  } else
#endif /* LWIP_ETHERNET */
    return tcpip_inpkt(p, inp, ip_input);
}

/**
 * @ingroup lwip_os
 * Call a specific function in the thread context of
 * tcpip_thread for easy access synchronization.
 * A function called in that way may access lwIP core code
 * without fearing concurrent access.
 * Blocks until the request is posted.
 * Must not be called from interrupt context!
 *
 * @param function the function to call
 * @param ctx parameter passed to f
 * @return ERR_OK if the function was called, another err_t if not
 *
 * @see tcpip_try_callback
 */
err_t
tcpip_callback(tcpip_callback_fn function, void *ctx)
{
  struct tcpip_msg *msg;

  LWIP_ASSERT("Invalid mbox", sys_mbox_valid_val(tcpip_mbox));

  msg = (struct tcpip_msg *)memp_malloc(MEMP_TCPIP_MSG_API);
  if (msg == NULL) {
    return ERR_MEM;
  }

  msg->type = TCPIP_MSG_CALLBACK;
  msg->msg.cb.function = function;
  msg->msg.cb.ctx = ctx;

  sys_mbox_post(&tcpip_mbox, msg);
  return ERR_OK;
}

/**
 * @ingroup lwip_os
 * Call a specific function in the thread context of
 * tcpip_thread for easy access synchronization.
 * A function called in that way may access lwIP core code
 * without fearing concurrent access.
 * Does NOT block when the request cannot be posted because the
 * tcpip_mbox is full, but returns ERR_MEM instead.
 * Can be called from interrupt context.
 *
 * @param function the function to call
 * @param ctx parameter passed to f
 * @return ERR_OK if the function was called, another err_t if not
 *
 * @see tcpip_callback
 */
err_t
tcpip_try_callback(tcpip_callback_fn function, void *ctx)
{
  struct tcpip_msg *msg;

  LWIP_ASSERT("Invalid mbox", sys_mbox_valid_val(tcpip_mbox));

  msg = (struct tcpip_msg *)memp_malloc(MEMP_TCPIP_MSG_API);
  if (msg == NULL) {
    return ERR_MEM;
  }

  msg->type = TCPIP_MSG_CALLBACK;
  msg->msg.cb.function = function;
  msg->msg.cb.ctx = ctx;

  if (sys_mbox_trypost(&tcpip_mbox, msg) != ERR_OK) {
    memp_free(MEMP_TCPIP_MSG_API, msg);
    return ERR_MEM;
  }
  return ERR_OK;
}

#if LWIP_TCPIP_TIMEOUT && LWIP_TIMERS
/**
 * call sys_timeout in tcpip_thread
 *
 * @param msecs time in milliseconds for timeout
 * @param h function to be called on timeout
 * @param arg argument to pass to timeout function h
 * @return ERR_MEM on memory error, ERR_OK otherwise
 */
err_t
tcpip_timeout(u32_t msecs, sys_timeout_handler h, void *arg)
{
  struct tcpip_msg *msg;

  LWIP_ASSERT("Invalid mbox", sys_mbox_valid_val(tcpip_mbox));

  msg = (struct tcpip_msg *)memp_malloc(MEMP_TCPIP_MSG_API);
  if (msg == NULL) {
    return ERR_MEM;
  }

  msg->type = TCPIP_MSG_TIMEOUT;
  msg->msg.tmo.msecs = msecs;
  msg->msg.tmo.h = h;
  msg->msg.tmo.arg = arg;
  sys_mbox_post(&tcpip_mbox, msg);
  return ERR_OK;
}

/**
 * call sys_untimeout in tcpip_thread
 *
 * @param h function to be called on timeout
 * @param arg argument to pass to timeout function h
 * @return ERR_MEM on memory error, ERR_OK otherwise
 */
err_t
tcpip_untimeout(sys_timeout_handler h, void *arg)
{
  struct tcpip_msg *msg;

  LWIP_ASSERT("Invalid mbox", sys_mbox_valid_val(tcpip_mbox));

  msg = (struct tcpip_msg *)memp_malloc(MEMP_TCPIP_MSG_API);
  if (msg == NULL) {
    return ERR_MEM;
  }

  msg->type = TCPIP_MSG_UNTIMEOUT;
  msg->msg.tmo.h = h;
  msg->msg.tmo.arg = arg;
  sys_mbox_post(&tcpip_mbox, msg);
  return ERR_OK;
}
#endif /* LWIP_TCPIP_TIMEOUT && LWIP_TIMERS */


/**
 * Sends a message to TCPIP thread to call a function. Caller thread blocks on
 * on a provided semaphore, which ist NOT automatically signalled by TCPIP thread,
 * this has to be done by the user.
 * It is recommended to use LWIP_TCPIP_CORE_LOCKING since this is the way
 * with least runtime overhead.
 *
 * @param fn function to be called from TCPIP thread
 * @param apimsg argument to API function
 * @param sem semaphore to wait on
 * @return ERR_OK if the function was called, another err_t if not
 */
err_t
tcpip_send_msg_wait_sem(tcpip_callback_fn fn, void *apimsg, sys_sem_t *sem)
{
#if LWIP_TCPIP_CORE_LOCKING
  LWIP_UNUSED_ARG(sem);
  LOCK_TCPIP_CORE();
  fn(apimsg);
  UNLOCK_TCPIP_CORE();
  return ERR_OK;
#else /* LWIP_TCPIP_CORE_LOCKING */
  TCPIP_MSG_VAR_DECLARE(msg);

  LWIP_ASSERT("semaphore not initialized", sys_sem_valid(sem));
  LWIP_ASSERT("Invalid mbox", sys_mbox_valid_val(tcpip_mbox));

  TCPIP_MSG_VAR_ALLOC(msg);
  TCPIP_MSG_VAR_REF(msg).type = TCPIP_MSG_API;
  TCPIP_MSG_VAR_REF(msg).msg.api_msg.function = fn;
  TCPIP_MSG_VAR_REF(msg).msg.api_msg.msg = apimsg;
  sys_mbox_post(&tcpip_mbox, &TCPIP_MSG_VAR_REF(msg));
  sys_arch_sem_wait(sem, 0);
  TCPIP_MSG_VAR_FREE(msg);
  return ERR_OK;
#endif /* LWIP_TCPIP_CORE_LOCKING */
}

/**
 * Synchronously calls function in TCPIP thread and waits for its completion.
 * It is recommended to use LWIP_TCPIP_CORE_LOCKING (preferred) or
 * LWIP_NETCONN_SEM_PER_THREAD.
 * If not, a semaphore is created and destroyed on every call which is usually
 * an expensive/slow operation.
 * @param fn Function to call
 * @param call Call parameters
 * @return Return value from tcpip_api_call_fn
 */
err_t
tcpip_api_call(tcpip_api_call_fn fn, struct tcpip_api_call_data *call)
{
#if LWIP_TCPIP_CORE_LOCKING
  err_t err;
  LOCK_TCPIP_CORE();
  err = fn(call);
  UNLOCK_TCPIP_CORE();
  return err;
#else /* LWIP_TCPIP_CORE_LOCKING */
  TCPIP_MSG_VAR_DECLARE(msg);

#if !LWIP_NETCONN_SEM_PER_THREAD
  err_t err = sys_sem_new(&call->sem, 0);
  if (err != ERR_OK) {
    return err;
  }
#endif /* LWIP_NETCONN_SEM_PER_THREAD */

  LWIP_ASSERT("Invalid mbox", sys_mbox_valid_val(tcpip_mbox));

  TCPIP_MSG_VAR_ALLOC(msg);
  TCPIP_MSG_VAR_REF(msg).type = TCPIP_MSG_API_CALL;
  TCPIP_MSG_VAR_REF(msg).msg.api_call.arg = call;
  TCPIP_MSG_VAR_REF(msg).msg.api_call.function = fn;
#if LWIP_NETCONN_SEM_PER_THREAD
  TCPIP_MSG_VAR_REF(msg).msg.api_call.sem = LWIP_NETCONN_THREAD_SEM_GET();
#else /* LWIP_NETCONN_SEM_PER_THREAD */
  TCPIP_MSG_VAR_REF(msg).msg.api_call.sem = &call->sem;
#endif /* LWIP_NETCONN_SEM_PER_THREAD */
  sys_mbox_post(&tcpip_mbox, &TCPIP_MSG_VAR_REF(msg));
  sys_arch_sem_wait(TCPIP_MSG_VAR_REF(msg).msg.api_call.sem, 0);
  TCPIP_MSG_VAR_FREE(msg);

#if !LWIP_NETCONN_SEM_PER_THREAD
  sys_sem_free(&call->sem);
#endif /* LWIP_NETCONN_SEM_PER_THREAD */

  return call->err;
#endif /* LWIP_TCPIP_CORE_LOCKING */
}

/**
 * @ingroup lwip_os
 * Allocate a structure for a static callback message and initialize it.
 * The message has a special type such that lwIP never frees it.
 * This is intended to be used to send "static" messages from interrupt context,
 * e.g. the message is allocated once and posted several times from an IRQ
 * using tcpip_callbackmsg_trycallback().
 * Example usage: Trigger execution of an ethernet IRQ DPC routine in lwIP thread context.
 * 
 * @param function the function to call
 * @param ctx parameter passed to function
 * @return a struct pointer to pass to tcpip_callbackmsg_trycallback().
 *
 * @see tcpip_callbackmsg_trycallback()
 * @see tcpip_callbackmsg_delete()
 */
struct tcpip_callback_msg *
tcpip_callbackmsg_new(tcpip_callback_fn function, void *ctx)
{
  struct tcpip_msg *msg = (struct tcpip_msg *)memp_malloc(MEMP_TCPIP_MSG_API);
  if (msg == NULL) {
    return NULL;
  }
  msg->type = TCPIP_MSG_CALLBACK_STATIC;
  msg->msg.cb.function = function;
  msg->msg.cb.ctx = ctx;
  return (struct tcpip_callback_msg *)msg;
}

/**
 * @ingroup lwip_os
 * Free a callback message allocated by tcpip_callbackmsg_new().
 *
 * @param msg the message to free
 *
 * @see tcpip_callbackmsg_new()
 */
void
tcpip_callbackmsg_delete(struct tcpip_callback_msg *msg)
{
  memp_free(MEMP_TCPIP_MSG_API, msg);
}

/**
 * @ingroup lwip_os
 * Try to post a callback-message to the tcpip_thread tcpip_mbox.
 *
 * @param msg pointer to the message to post
 * @return sys_mbox_trypost() return code
 *
 * @see tcpip_callbackmsg_new()
 */
err_t
tcpip_callbackmsg_trycallback(struct tcpip_callback_msg *msg)
{
  LWIP_ASSERT("Invalid mbox", sys_mbox_valid_val(tcpip_mbox));
  return sys_mbox_trypost(&tcpip_mbox, msg);
}

/**
 * @ingroup lwip_os
 * Try to post a callback-message to the tcpip_thread mbox.
 * Same as @ref tcpip_callbackmsg_trycallback but calls sys_mbox_trypost_fromisr(),
 * mainly to help FreeRTOS, where calls differ between task level and ISR level.
 *
 * @param msg pointer to the message to post
 * @return sys_mbox_trypost_fromisr() return code (without change, so this
 *         knowledge can be used to e.g. propagate "bool needs_scheduling")
 *
 * @see tcpip_callbackmsg_new()
 */
err_t
tcpip_callbackmsg_trycallback_fromisr(struct tcpip_callback_msg *msg)
{
  LWIP_ASSERT("Invalid mbox", sys_mbox_valid_val(tcpip_mbox));
  return sys_mbox_trypost_fromisr(&tcpip_mbox, msg);
}

/**
 * @ingroup lwip_os
 * Initialize this module:
 * - initialize all sub modules
 * - start the tcpip_thread
 *
 * @param initfunc a function to call when tcpip_thread is running and finished initializing
 * @param arg argument to pass to initfunc
 */
void
tcpip_init(tcpip_init_done_fn initfunc, void *arg)
{
  lwip_init();

  tcpip_init_done = initfunc;
  tcpip_init_done_arg = arg;
  if (sys_mbox_new(&tcpip_mbox, TCPIP_MBOX_SIZE) != ERR_OK) {
    LWIP_ASSERT("failed to create tcpip_thread mbox", 0);
  }
#if CONFIG_TCPIP_NCM_RX_PRIORITY && !LWIP_TCPIP_CORE_LOCKING_INPUT
  tcpip_ncm_rx_priority_init();
#endif
#if CONFIG_TCPIP_RX_BATCH_STAGE1 && !LWIP_TCPIP_CORE_LOCKING_INPUT
  tcpip_rx_stage1_init();
#if CONFIG_TCPIP_RX_BATCH_TIMER_PROBE
  tcpip_rx_timer_probe_init();
#endif
#endif
#if LWIP_TCPIP_CORE_LOCKING
  if (sys_mutex_new(&lock_tcpip_core) != ERR_OK) {
    LWIP_ASSERT("failed to create lock_tcpip_core", 0);
  }
#endif /* LWIP_TCPIP_CORE_LOCKING */

//Realtek add
#if CONFIG_USE_TCM_HEAP
  sys_thread_new_tcm(TCPIP_THREAD_NAME, tcpip_thread, NULL, TCPIP_THREAD_STACKSIZE, TCPIP_THREAD_PRIO);
#else
  sys_thread_new(TCPIP_THREAD_NAME, tcpip_thread, NULL, TCPIP_THREAD_STACKSIZE, TCPIP_THREAD_PRIO);
#endif
//Realtek add end
}

/**
 * Simple callback function used with tcpip_callback to free a pbuf
 * (pbuf_free has a wrong signature for tcpip_callback)
 *
 * @param p The pbuf (chain) to be dereferenced.
 */
static void
pbuf_free_int(void *p)
{
  struct pbuf *q = (struct pbuf *)p;
  pbuf_free(q);
}

/**
 * A simple wrapper function that allows you to free a pbuf from interrupt context.
 *
 * @param p The pbuf (chain) to be dereferenced.
 * @return ERR_OK if callback could be enqueued, an err_t if not
 */
err_t
pbuf_free_callback(struct pbuf *p)
{
  return tcpip_try_callback(pbuf_free_int, p);
}

/**
 * A simple wrapper function that allows you to free heap memory from
 * interrupt context.
 *
 * @param m the heap memory to free
 * @return ERR_OK if callback could be enqueued, an err_t if not
 */
err_t
mem_free_callback(void *m)
{
  return tcpip_try_callback(mem_free, m);
}

#endif /* !NO_SYS */
