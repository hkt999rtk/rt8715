#!/usr/bin/env python3
"""Run the production RX batch functions with a bounded host mailbox.

Only RTOS, timer and pbuf operations are simulated. The batch state machine,
enqueue function and message consumer are extracted unchanged from tcpip.c.
This checks ownership and progress after saturation, not board IRQ timing.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[5]
SOURCE = ROOT / "component/common/network/lwip/lwip_v2.1.2/src/api/tcpip.c"

STUBS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rx_handover_diag.h"
#if CONFIG_RX_HANDOVER_DIAG
uint32_t rxhd_counters[RXHD_COUNT];
#endif
typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef int err_t;
typedef int BaseType_t;
typedef int timer_id_t;
typedef int hal_timer_adapter_t;
#define CONFIG_TCPIP_RX_BATCH_STAGE1 1
#define CONFIG_TCPIP_RX_BATCH_STAGE3 1
#define CONFIG_TCPIP_RX_BATCH_TIMER_PROBE 1
#define CONFIG_TCPIP_RX_BATCH_TIMER_MBOX_PROBE 1
#define CONFIG_TCPIP_RX_BATCH_MAX_PACKETS 8U
#define CONFIG_TCPIP_RX_BATCH_TIMEOUT_US 1000U
#define CONFIG_TCPIP_RX_BATCH_PROFILE 0
#define CONFIG_NET_QUEUE_PROFILE 0
#define CONFIG_TCPIP_NCM_RX_PRIORITY 0
#define LWIP_TCPIP_CORE_LOCKING_INPUT 0
#define LWIP_TCPIP_CORE_LOCKING 1
#define LWIP_TCPIP_TIMEOUT 0
#define LWIP_TIMERS 0
#define TCPIP_THREAD_TEST 1
#define ERR_OK 0
#define ERR_MEM -1
#define pdPASS 1
#define pdFALSE 0
#define HAL_OK 0
#define MaxGTimerNum 4
#define SYS_ARCH_TIMEOUT UINT32_MAX
#define MEMP_TCPIP_MSG_INPKT 1
#define MEMP_TCPIP_MSG_API 2
#define LWIP_UNUSED_ARG(x) ((void)(x))
#define LWIP_DEBUGF(a, b) ((void)0)
#define LWIP_PLATFORM_DIAG(x) ((void)0)
#define LWIP_ASSERT(message, condition) assert(condition)
#define LOCK_TCPIP_CORE() ((void)0)
#define UNLOCK_TCPIP_CORE() ((void)0)
static int critical;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() do { assert(critical > 0); --critical; } while (0)
#define portYIELD_FROM_ISR(x) ((void)(x))
struct pbuf { unsigned id, freed, delivered; u16_t tot_len; };
struct netif { char name[2]; };
typedef err_t (*netif_input_fn)(struct pbuf *, struct netif *);
enum { TCPIP_MSG_INPKT_BATCH, TCPIP_MSG_RX_TIMER_PROBE,
       TCPIP_MSG_INPKT_STAGE1, TCPIP_MSG_INPKT,
       TCPIP_MSG_CALLBACK, TCPIP_MSG_CALLBACK_STATIC };
struct tcpip_msg {
  int type;
  union {
    void *inp_stage1;
    struct { struct pbuf *p; struct netif *netif; netif_input_fn input_fn; } inp;
    struct { void (*function)(void *); void *ctx; } cb;
  } msg;
};
struct mailbox { struct tcpip_msg *items[600]; unsigned count; };
typedef struct mailbox *sys_mbox_t;
static struct mailbox queue;
static sys_mbox_t tcpip_mbox = &queue;
static unsigned uxQueueMessagesWaiting(sys_mbox_t q) { return q->count; }
static unsigned memp_live, fail_alloc, refill_on_callback;
static unsigned timer_scheduled;
static void (*timer_callback)(void *);
static void *timer_arg;
static int xQueueSend(sys_mbox_t q, struct tcpip_msg **m, unsigned ticks) {
  assert(ticks == 0);
  if (q->count == 600) return 0;
  q->items[q->count++] = *m;
  return pdPASS;
}
static int xQueueSendFromISR(sys_mbox_t q, struct tcpip_msg **m, BaseType_t *w) {
  *w = pdFALSE;
  return xQueueSend(q, m, 0);
}
static int sys_mbox_valid_val(sys_mbox_t q) { return q != NULL; }
static err_t sys_mbox_trypost(sys_mbox_t *q, struct tcpip_msg *m) {
  return xQueueSend(*q, &m, 0) == pdPASS ? ERR_OK : ERR_MEM;
}
static u32_t sys_arch_mbox_tryfetch(sys_mbox_t *q, void **m) {
  if ((*q)->count == 0) return SYS_ARCH_TIMEOUT;
  *m = (*q)->items[0];
  --(*q)->count;
  memmove((*q)->items, (*q)->items + 1, (*q)->count * sizeof((*q)->items[0]));
  return 0;
}
static void *memp_malloc(int pool) {
  (void)pool;
  if (fail_alloc) return NULL;
  void *p = calloc(1, sizeof(struct tcpip_msg));
  assert(p);
  ++memp_live;
  return p;
}
static void memp_free(int pool, void *p) {
  (void)pool;
  assert(memp_live > 0);
  --memp_live;
  free(p);
}
static void pbuf_free(struct pbuf *p) { assert(p->freed++ == 0); }
static int hal_timer_allocate(void *x) { (void)x; return 0; }
static int hal_timer_init(hal_timer_adapter_t *t, timer_id_t id) {
  (void)t; (void)id; return HAL_OK;
}
static void hal_timer_start_one_shot(hal_timer_adapter_t *t, unsigned us,
                                   void (*cb)(void *), void *arg) {
  (void)t;
  assert(us == 1000);
  timer_scheduled = 1;
  timer_callback = cb;
  timer_arg = arg;
}
static void fire_timer(void) {
  assert(timer_scheduled);
  timer_scheduled = 0;
  timer_callback(timer_arg);
}
'''

TESTS = r'''
static struct netif wlan = { {'a', 'p'} };
static struct pbuf packets[140];
static unsigned received, last_id, reject_input;
static err_t input(struct pbuf *p, struct netif *n) {
  assert(n == &wlan);
#if CONFIG_RX_HANDOVER_DIAG
  /* pool_fallback packet 128 uses a standard envelope; it is not complete
   * until input() returns, including when input is blocked or rejects it. */
  if (p->id == 128) assert(rxhd_counters[RXHD_WLAN_DONE] == 128);
#endif
  assert(!p->freed && !p->delivered++);
  assert(!received || p->id > last_id);
  last_id = p->id;
  ++received;
  if (reject_input) return ERR_MEM;
  pbuf_free(p);
  return ERR_OK;
}
static struct tcpip_msg filler;
static void callback(void *arg) {
  (void)arg;
  if (refill_on_callback) {
    struct tcpip_msg *m = &filler;
    --refill_on_callback;
    assert(xQueueSend(tcpip_mbox, &m, 0) == pdPASS);
  }
}
static void fill_queue(void) {
  struct tcpip_msg *m = &filler;
  while (queue.count < 600) assert(xQueueSend(tcpip_mbox, &m, 0) == pdPASS);
}
static err_t send_packet(unsigned i) {
  assert(i < 140);
  packets[i].id = i;
  packets[i].tot_len = 160;
  return tcpip_inpkt(&packets[i], &wlan, input);
}
static void drain(void) {
  unsigned attempts = 0;
  while (tcpip_thread_poll_one()) assert(++attempts < 1000);
  assert(critical == 0);
}
static void expect_released(unsigned count) {
  assert(received == count);
  assert(tcpip_rx_batch_count == 0);
  assert(tcpip_rx_stage1_in_use == 0);
  assert(memp_live == 0);
#if CONFIG_RX_HANDOVER_DIAG
  assert(rxhd_counters[RXHD_WLAN_DONE] == count);
  assert(rxhd_counters[RXHD_BATCH_BAD] == 0);
  assert(rxhd_counters[RXHD_STACK_REJECT] == (reject_input ? count : 0));
#endif
  unsigned free_count = 0;
  u16_t index = tcpip_rx_stage1_free_head;
  while (index != TCPIP_RX_STAGE1_INVALID_INDEX) {
    assert(index < TCPIP_RX_STAGE1_POOL_SIZE);
    assert(tcpip_rx_stage1_pool[index].state == TCPIP_RX_STAGE1_FREE);
    index = tcpip_rx_stage1_pool[index].next_free;
    assert(++free_count <= TCPIP_RX_STAGE1_POOL_SIZE);
  }
  assert(free_count == TCPIP_RX_STAGE1_POOL_SIZE);
  for (unsigned i = 0; i < count; ++i) assert(packets[i].freed == 1);
}
int main(int argc, char **argv) {
  assert(argc == 2);
  filler.type = TCPIP_MSG_CALLBACK_STATIC;
  filler.msg.cb.function = callback;
  tcpip_rx_stage1_init();
  tcpip_rx_timer_probe_init();
  if (!strcmp(argv[1], "normal_timer")) {
    for (unsigned i = 0; i < 7; ++i) assert(send_packet(i) == ERR_OK);
    struct tcpip_msg *m = &filler;
    assert(xQueueSend(tcpip_mbox, &m, 0) == pdPASS);
    drain();
    assert(received == 0 && tcpip_rx_batch_count == 7);
    fire_timer();
    drain();
    expect_released(7);
  } else if (!strcmp(argv[1], "oversized_new_rx")) {
    fill_queue();
    for (unsigned i = 0; i < 12; ++i) assert(send_packet(i) == ERR_OK);
    fire_timer();
    assert(tcpip_rx_batch_count == 12);
    assert(tcpip_thread_poll_one());
    assert(send_packet(12) == ERR_OK);
    drain();
    if (timer_scheduled) fire_timer();
    drain();
    expect_released(13);
  } else if (!strcmp(argv[1], "pool_fallback")) {
    for (unsigned i = 0; i < 129; ++i) assert(send_packet(i) == ERR_OK);
    assert(memp_live == 1);
    drain();
    expect_released(129);
  } else if (!strcmp(argv[1], "fallback_alloc_fail")) {
    for (unsigned i = 0; i < 128; ++i) assert(send_packet(i) == ERR_OK);
    fail_alloc = 1;
    assert(send_packet(128) == ERR_MEM);
    assert(packets[128].freed == 0);
    pbuf_free(&packets[128]); /* ethernetif owns rejected packets */
    drain();
    expect_released(128);
  } else {
    unsigned count = 12;
    if (!strcmp(argv[1], "timer_full_small")) count = 3;
    else if (!strcmp(argv[1], "pool_full")) count = 128;
    else if (!strcmp(argv[1], "retry_full_again")) refill_on_callback = 3;
    else if (!strcmp(argv[1], "input_reject")) reject_input = 1;
    else assert(!strcmp(argv[1], "timer_full"));
    fill_queue();
    for (unsigned i = 0; i < count; ++i) assert(send_packet(i) == ERR_OK);
    fire_timer();
    assert(tcpip_rx_batch_count == count && tcpip_rx_timer_probe_armed == 0);
#if CONFIG_RX_HANDOVER_DIAG
    assert(rxhd_counters[RXHD_BATCH_ISR_FAIL] == 1);
#endif
    if (count == 128) {
      assert(send_packet(128) == ERR_MEM);
      assert(packets[128].freed == 0);
      pbuf_free(&packets[128]);
    }
    drain(); /* No new RX and no more timer fires: consumer must make progress. */
    if (received != count) {
      fprintf(stderr, "FAIL %s: delivered=%u expected=%u pending=%u in_use=%u mailbox=%u timer_armed=%u\n",
              argv[1], received, count, (unsigned)tcpip_rx_batch_count,
              (unsigned)tcpip_rx_stage1_in_use, queue.count,
              (unsigned)tcpip_rx_timer_probe_armed);
      return 1;
    }
    expect_released(count);
#if CONFIG_RX_HANDOVER_DIAG
    assert(rxhd_counters[RXHD_BATCH_RETRY] >= 1);
    assert(rxhd_counters[RXHD_BATCH_RECOVER] == 1);
#endif
  }
  printf("PASS %s\n", argv[1]);
  return 0;
}
'''


def extract(source):
    start = source.index("#if CONFIG_TCPIP_RX_BATCH_STAGE1 && !LWIP_TCPIP_CORE_LOCKING_INPUT\n#define TCPIP_RX_STAGE1_POOL_SIZE")
    end = source.index("\n#if LWIP_TCPIP_CORE_LOCKING\n", start)
    batch = source[start:end]
    start = source.index("static void\ntcpip_thread_handle_msg(")
    end = source.index("\n/**\n * Pass a received packet", start)
    consumer = source[start:end]
    start = source.index("err_t\ntcpip_inpkt(")
    end = source.index("\n/**\n * @ingroup lwip_os", start)
    return batch + consumer + source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="tcpip-rx-batch-") as directory:
        work = Path(directory)
        c = work / "test.c"
        exe = work / "test"
        c.write_text(STUBS + extract(args.source.read_text()) + TESTS)
        failed = []
        for diag in (0, 1):
            print(f"CONFIG_RX_HANDOVER_DIAG={diag}", flush=True)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-Wno-unused-variable",
                            "-Wno-unused-function", "-fsanitize=undefined", "-g",
                            f"-DCONFIG_RX_HANDOVER_DIAG={diag}",
                            "-I", str(Path(__file__).resolve().parents[1]),
                            str(c), "-o", str(exe)], check=True)
            for case in ("normal_timer", "oversized_new_rx", "pool_fallback",
                         "fallback_alloc_fail", "timer_full_small", "timer_full",
                         "pool_full", "retry_full_again", "input_reject"):
                result = subprocess.run([str(exe), case], timeout=5)
                if result.returncode:
                    failed.append(f"{case} (diag={diag})")
        if failed:
            raise SystemExit("Failed: " + ", ".join(failed))


if __name__ == "__main__":
    main()
