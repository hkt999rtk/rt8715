#!/usr/bin/env python3
"""Exercise the real trace implementation with deterministic host RTOS/TCP mocks."""
from pathlib import Path
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[2] / "car_ack_timestamp.c"
HEADERS = {
    "FreeRTOS.h": """
#pragma once
#include <stdint.h>
typedef void *TaskHandle_t;
typedef int BaseType_t;
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define pdMS_TO_TICKS(x) (x)
""",
    "queue.h": "#include <stdint.h>\nuint32_t uxQueueMessagesWaiting(void *);\n",
    "task.h": """
#pragma once
#include "FreeRTOS.h"
TaskHandle_t xTaskGetCurrentTaskHandle(void);
const char *pcTaskGetName(TaskHandle_t);
void vTaskDelay(uint32_t);
int xTaskCreate(void (*fn)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
""",
    "FreeRTOS_POSIX/time.h": "#include <time.h>\n",
    "diag.h": "int rt_printf(const char *, ...);\n",
    "hal_timer.h": "#include <stdint.h>\nuint32_t hal_read_curtime_us(void);\n",
    "lwip/tcp.h": """
#pragma once
#include <stdint.h>
typedef int err_t;
typedef uint16_t u16_t;
typedef uint8_t u8_t;
#define ERR_OK 0
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { uint32_t addr[4]; } ip6_addr_t;
typedef struct { union { ip4_addr_t v4; ip6_addr_t v6; } u; int v6; } ip_addr_t;
#define IP_IS_V6(p) ((p)->v6)
#define ip_2_ip4(p) (&(p)->u.v4)
#define ip_2_ip6(p) (&(p)->u.v6)
struct tcp_pcb { uint32_t snd_lbb; uint16_t local_port, remote_port; ip_addr_t local_ip, remote_ip; };
""",
    "lwip/pbuf.h": """
#pragma once
#include <stdint.h>
struct pbuf { uint16_t tot_len, len; const void *payload; struct pbuf *next; };
uint16_t pbuf_copy_partial(const struct pbuf *, void *, uint16_t, uint16_t);
""",
}

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include "TRACE_SOURCE"
static uint32_t test_ms, test_us;
static TaskHandle_t current = (void *)1;
static int write_error;
static unsigned hal_calls;
static uint32_t queue_data[16], queue_head, queue_count, elapsed;
static uint8_t urb;
const char *pcTaskGetName(TaskHandle_t t)
{ (void)t; return current == (void *)1 ? "TCPClient" : "usbh_main_task"; }
uint32_t uxQueueMessagesWaiting(void *q) { (void)q; return queue_count; }
int __real_usb_os_queue_send(void *q, const void *msg, uint32_t timeout)
{
    (void)q; (void)timeout;
    if (queue_count == 16) return 4;
    queue_data[(queue_head + queue_count++) % 16] = *(const uint32_t *)msg; return 0;
}
int __real_usb_os_queue_receive(void *q, void *msg, uint32_t timeout)
{
    (void)q; (void)timeout;
    if (!queue_count) return 4;
    *(uint32_t *)msg = queue_data[queue_head++ % 16]; queue_count--; return 0;
}
uint8_t __real_usbh_get_urb_state(void *h, uint8_t ch) { (void)h; (void)ch; return urb; }
uint32_t __real_usbh_get_elapsed_ticks(void *h, uint32_t base) { (void)h; (void)base; return elapsed; }
uint8_t __real_usbh_check_nak_timeout(void *h, uint8_t ch, uint8_t limit)
{ (void)h; (void)ch; (void)limit; return 1; }
void __real_usbh_trigger_rexfer(void *h, uint8_t ch) { (void)h; (void)ch; }
void __real_usbh_enable_nak_interrupt(void *h, uint8_t ch) { (void)h; (void)ch; }

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current; }
void vTaskDelay(uint32_t t) { (void)t; }
int xTaskCreate(void (*f)(void *), const char *n, unsigned s, void *a, unsigned p, TaskHandle_t *h)
{ (void)f; (void)n; (void)s; (void)a; (void)p; *h = (void *)2; return 1; }
int rt_printf(const char *s, ...) { (void)s; return 0; }
int ackts_test_clock_gettime(clockid_t c, struct timespec *t)
{ assert(c == CLOCK_MONOTONIC); t->tv_sec = test_ms / 1000; t->tv_nsec = (test_ms % 1000) * 1000000; return 0; }
uint32_t hal_read_curtime_us(void) { return test_us; }
err_t __real_tcp_write(struct tcp_pcb *p, const void *b, u16_t n, u8_t f)
{ (void)b; (void)f; if (!write_error) p->snd_lbb += n; return write_error; }
uint8_t __real_usbh_hal_hc_start_transfer(void *h, uint8_t dma)
{ (void)h; (void)dma; hal_calls++; test_ms++; test_us += 30; return 0; }
uint16_t pbuf_copy_partial(const struct pbuf *p, void *out, uint16_t n, uint16_t off)
{
    uint16_t copied = 0;
    while (p && copied < n) {
        if (off >= p->len) { off -= p->len; p = p->next; continue; }
        uint16_t take = p->len - off;
        if (take > n - copied) take = n - copied;
        memcpy((uint8_t *)out + copied, (const uint8_t *)p->payload + off, take);
        copied += take; off = 0; p = p->next;
    }
    return copied;
}
static void reset(void)
{
    memset(responses, 0, sizeof(responses)); memset(packets, 0, sizeof(packets));
    next_response = active_response = next_packet = 0;
    flow_local_port = flow_peer_port = 0;
    active_usb_packet = 0; active_usb_buffer = NULL;
    dropped = ambiguous = response_overrun = 0;
    current = (void *)1; response_task = NULL; write_error = 0;
    memset(rx_records, 0, sizeof(rx_records)); memset(recoveries, 0, sizeof(recoveries));
    memset(core_events, 0, sizeof(core_events));
    core_queue = NULL; core_task = http_task = NULL; rx_hc = NULL;
    core_head = core_count = rx_generation = rx_notified = rx_context_generation = 0;
    rx_next = rx_drop = recovery_drop = core_desync = core_send_fail = 0;
    core_synced = have_http_done = rx_context_valid = last_urb_valid = last_elapsed_valid = 0;
    queue_head = queue_count = elapsed = 0; urb = 0;
    test_ms = 1234; test_us = 1234567; hal_calls = 0;
}
static struct tcp_pcb pcb(uint32_t seq, int v6)
{
    struct tcp_pcb p = {0}; p.snd_lbb = seq; p.local_port = 1234; p.remote_port = 5678;
    p.local_ip.v6 = p.remote_ip.v6 = v6;
    memset(&p.local_ip.u, 0x11, v6 ? 16 : 4); memset(&p.remote_ip.u, 0x22, v6 ? 16 : 4);
    return p;
}
static void put16(uint8_t *p, uint32_t n) { p[0] = n >> 8; p[1] = n; }
static void put32(uint8_t *p, uint32_t n) { put16(p, n >> 16); put16(p + 2, n); }
static struct pbuf frame(uint8_t *b, uint32_t seq, uint32_t n, int v6)
{
    uint32_t tcp = v6 ? 54 : 34; memset(b, 0, 256);
    put16(b + 12, v6 ? 0x86dd : 0x0800); b[14] = v6 ? 0x60 : 0x45;
    if (v6) { b[20] = 6; put16(b + 18, 20 + n); memset(b + 22, 0x11, 16); memset(b + 38, 0x22, 16); }
    else { b[23] = 6; put16(b + 16, 40 + n); memset(b + 26, 0x11, 4); memset(b + 30, 0x22, 4); }
    put16(b + tcp, 1234); put16(b + tcp + 2, 5678); put32(b + tcp + 4, seq);
    b[tcp + 12] = 0x50; b[tcp + 13] = 0x18;
    struct pbuf p = {tcp + 20 + n, tcp + 20 + n, b, NULL}; return p;
}
int main(void)
{
    uint8_t b[256]; uint32_t id, k; struct tcp_pcb c; struct pbuf p;
    reset(); c = pcb(100, 0); id = car_ack_timestamp_begin();
    assert(__wrap_tcp_write(&c, "x", 40, 0) == 0);
    car_ack_timestamp_end(id, 0); /* output delayed until after write returned */
    p = frame(b, 100, 40, 0); test_ms += 10; test_us += 10000;
    k = car_ack_timestamp_ncm(&p); assert(k && packets[k % ACKTS_SLOTS].response == id);
    assert(packets[k % ACKTS_SLOTS].ncm.us - packets[k % ACKTS_SLOTS].enter.us == 10000);
    union { void *alignment; uint8_t bytes[128]; } hc = {0};
    *(void **)hc.bytes = b; hc.bytes[0x2a] = 2; hc.bytes[0x2f] = 0;
    car_ack_timestamp_usb_begin(k, b);
    hc.bytes[0x2f] = 1; __wrap_usbh_hal_hc_start_transfer(hc.bytes, 1);
    assert(!packets[k % ACKTS_SLOTS].have_hw); /* IN must not match */
    hc.bytes[0x2f] = 0; __wrap_usbh_hal_hc_start_transfer(hc.bytes, 1);
    __wrap_usbh_hal_hc_start_transfer(hc.bytes, 1); /* retry */
    assert(hal_calls == 3 && packets[k % ACKTS_SLOTS].submits == 2);
    assert(packets[k % ACKTS_SLOTS].hw_end.us - packets[k % ACKTS_SLOTS].hw_begin.us == 30);
    car_ack_timestamp_usb_end(k, 0); assert(packets[k % ACKTS_SLOTS].complete);

    reset(); c = pcb(0xfffffff0U, 1); id = car_ack_timestamp_begin();
    __wrap_tcp_write(&c, "x", 32, 0); car_ack_timestamp_end(id, 0);
    p = frame(b, 0, 16, 1); /* sequence wrap + fragmented pbuf */
    struct pbuf tail = {p.tot_len - 31, p.len - 31, b + 31, NULL}; p.len = 31; p.next = &tail;
    assert(car_ack_timestamp_ncm(&p));
    b[38] ^= 1; assert(!car_ack_timestamp_ncm(&p)); /* wrong peer */
    b[38] ^= 1; test_ms += ACKTS_EXPIRE_MS; assert(!car_ack_timestamp_ncm(&p));

    reset(); c = pcb(100, 0); id = car_ack_timestamp_begin();
    write_error = -1; __wrap_tcp_write(&c, "x", 20, 0);
    assert(!responses[id % ACKTS_SLOTS].have_seq);
    write_error = 0; current = (void *)3; __wrap_tcp_write(&c, "x", 20, 0);
    assert(!responses[id % ACKTS_SLOTS].have_seq); /* another task */
    current = (void *)1; __wrap_tcp_write(&c, "x", 20, 0); car_ack_timestamp_end(id, 0);
    id = car_ack_timestamp_begin(); __wrap_tcp_write(&c, "x", 20, 0); car_ack_timestamp_end(id, 0);
    p = frame(b, 120, 40, 0); assert(!car_ack_timestamp_ncm(&p)); assert(ambiguous == 1);
    p = frame(b, 140, 0, 0); assert(!car_ack_timestamp_ncm(&p)); /* pure ACK */

    reset(); c = pcb(100, 0); id = car_ack_timestamp_begin();
    __wrap_tcp_write(&c, "x", 20, 0); car_ack_timestamp_end(id, 0); p = frame(b, 100, 20, 0);
    for (unsigned i = 0; i < ACKTS_SLOTS; i++) assert(car_ack_timestamp_ncm(&p));
    assert(!car_ack_timestamp_ncm(&p)); assert(dropped == 1); /* bounded, nonblocking */
    ackts_time_t earlier = {12, 999, 0}, later = {13, 0, 0};
    assert(ackts_age_ms(earlier, later) == -1); /* concurrent creation after reporter snapshot */
    assert(ackts_age_ms(later, earlier) == 1);
    reset(); car_ack_timestamp_http_done(-1); assert(!have_http_done);
    car_ack_timestamp_http_done(0); test_us += 150;
    c = pcb(100, 0); id = car_ack_timestamp_begin();
    assert(responses[id % ACKTS_SLOTS].have_http && !have_http_done);
    assert(responses[id % ACKTS_SLOTS].enter.us - responses[id % ACKTS_SLOTS].http_done.us == 150);
    __wrap_tcp_write(&c, "x", 20, 0); car_ack_timestamp_end(id, 0);
    car_ack_timestamp_hid_stage(0); test_us += 70; car_ack_timestamp_hid_stage(1);
    assert(responses[id % ACKTS_SLOTS].hid_end.us - responses[id % ACKTS_SLOTS].hid_begin.us == 70);
    p = frame(b, 100, 20, 0); k = car_ack_timestamp_ncm(&p);
    car_ack_timestamp_queue(k, 2, 1, 3, 1); assert(packets[k % ACKTS_SLOTS].input_depth == 2);
    car_ack_timestamp_usb_begin(k, b);
    current = (void *)4; uint32_t msg = 3, received;
    void *q = (void *)5;
    assert(__wrap_usb_os_queue_receive(q, &received, 0) == 4); /* discovers empty queue */
    assert(core_synced && core_task == current);
    assert(__wrap_usb_os_queue_send(q, &msg, 0) == 0);
    test_us += 10000;
    assert(__wrap_usb_os_queue_receive(q, &received, 0) == 0 && received == msg);
    assert(packets[k % ACKTS_SLOTS].core_wait_max_us == 10000);
    for (unsigned i = 0; i < 16; i++) assert(!__wrap_usb_os_queue_send(q, &msg, 0));
    assert(__wrap_usb_os_queue_send(q, &msg, 0) == 4 && core_send_fail == 1 && core_count == 16);
    for (unsigned i = 0; i < 16; i++) assert(!__wrap_usb_os_queue_receive(q, &received, 0));
    assert(!core_count);
    /* An untracked queue write fails closed, then resynchronizes once empty. */
    assert(!__real_usb_os_queue_send(q, &msg, 0));
    assert(!__wrap_usb_os_queue_receive(q, &received, 0));
    assert(core_desync == 1 && core_synced && !core_count);
    hc.bytes[0x25] = 4; hc.bytes[0x2a] = 2; hc.bytes[0x2f] = 1;
    __wrap_usbh_hal_hc_start_transfer(hc.bytes, 1); hc.bytes[0x28] = 1; msg = 4;
    assert(!__wrap_usb_os_queue_send(q, &msg, 0)); test_us += 1000;
    assert(!__wrap_usb_os_queue_receive(q, &received, 0));
    assert(rx_context_valid && rx_context_generation == rx_generation);
    p = frame(b, 500, 20, 0); memset(b + 26, 0x22, 4); memset(b + 30, 0x11, 4);
    put16(b + 34, 5678); put16(b + 36, 1234);
    car_ack_timestamp_rx(&p); assert(rx_records[0].occupied && rx_records[0].valid_usb);
    b[26] ^= 1; car_ack_timestamp_rx(&p); assert(rx_next == 1); b[26] ^= 1;
    __wrap_usbh_hal_hc_start_transfer(hc.bytes, 1);
    car_ack_timestamp_rx(&p); assert(rx_records[1].occupied && !rx_records[1].valid_usb);
    urb = 0; elapsed = 80;
    __wrap_usbh_get_urb_state(NULL, 4); __wrap_usbh_get_elapsed_ticks(NULL, 200);
    __wrap_usbh_get_urb_state(NULL, 4); /* RX IDLE recheck preserves the gate */
    __wrap_usbh_trigger_rexfer(NULL, 4); __wrap_usbh_trigger_rexfer(NULL, 4);
    assert(recoveries[0].occupied && recoveries[0].elapsed == 80 && recoveries[0].count == 2);
    assert(recoveries[0].packet == 0); /* RX coalescing while TX active */
    urb = 2; __wrap_usbh_get_urb_state(NULL, 3);
    __wrap_usbh_check_nak_timeout(NULL, 3, 5);
    assert(recoveries[1].elapsed == UINT32_MAX); /* no stale RX elapsed */
    __wrap_usbh_get_elapsed_ticks(NULL, 300); __wrap_usbh_enable_nak_interrupt(NULL, 3);
    assert(recoveries[2].elapsed == 80 && recoveries[2].packet == k);
    assert(!__wrap_usb_os_queue_send(q, &msg, 0));
    assert(!__wrap_usb_os_queue_receive(q, &received, 0));
    assert(!last_urb_valid && !last_elapsed_valid);
    ackts_report_extra(ackts_now()); assert(!rx_records[0].occupied && !recoveries[0].occupied);
    puts("PASS: HTTP/HID stages, queue wait/failure/resync, RX generations/flow, recovery gates");
    puts("PASS: delayed send, sequence wrap, IPv6/chained pbuf, flow isolation, HAL/retry, errors, coalescing, overflow, clock boundary");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="ackts-test-") as temporary:
        root = Path(temporary)
        for name, contents in HEADERS.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        source = root / "test.c"
        source.write_text(HARNESS.replace("TRACE_SOURCE", str(SOURCE)))
        binary = root / "test"
        subprocess.run([
            "gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-fno-pie", "-no-pie",
            "-DCONFIG_CAR_ACK_TIMESTAMP=1", "-DCONFIG_NCM_TX_ASYNC=1",
            "-DCONFIG_NCM_TX_PIPELINE=1", "-DCONFIG_NCM_TX_BATCH_MAX=1",
            "-DLWIP_TCPIP_CORE_LOCKING=1", "-Dclock_gettime=ackts_test_clock_gettime",
            "-I", str(root), str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    main()
