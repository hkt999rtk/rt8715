#include "rx_handover_diag.h"

#if CONFIG_RX_HANDOVER_DIAG
#include <stdarg.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "rt_printf.h"

uint32_t rxhd_counters[RXHD_COUNT];

extern SemaphoreHandle_t printf_lock_handle(void);

/* Only the existing wlan_rx_diag task calls the reporter. A low-priority
 * printf can still hold up higher-priority logging tasks via its mutex.
 * Format into a bounded stack buffer outside that mutex, then emit short
 * complete UART lines with a zero-wait take and a tick between lines.
 * Byte-bounded output is not a wall-time guarantee (UART and preemption).
 * This deliberately goes only to UART, not CONFIG_ADD_LOG's storage sink. */
#define RXHD_LINE_BYTES 160U /* includes CRLF */
struct rxhd_log_buffer {
    char text[768];
    unsigned used, overflow;
};
static uint32_t rxhd_log_skipped;

static int rxhd_buffer_putc(void *arg, char c)
{
    struct rxhd_log_buffer *b = arg;
    if (b->used < sizeof(b->text) - 1U)
        b->text[b->used++] = c;
    else
        b->overflow = 1U;
    return 1;
}

void carbox_rxhd_log(const char *fmt, ...)
{
    struct rxhd_log_buffer b;
    SemaphoreHandle_t lock = printf_lock_handle();
    const char *prefix_end, *next, *end;
    unsigned prefix, i;
    va_list ap;

    /* No early-boot critical-section fallback, and no blocking lock wait.
     * This first probe also avoids formatting when another logger is busy. */
    if (lock == NULL || xSemaphoreTake(lock, 0) != pdPASS) {
        rxhd_log_skipped++;
        return;
    }
    xSemaphoreGive(lock);
    b.used = b.overflow = 0U;
    va_start(ap, fmt);
#if defined(CONFIG_BUILD_SECURE)
    stdio_printf_stubs.printf_corel(rxhd_buffer_putc, &b, fmt, ap);
#else
    stdio_printf_stubs.printf_core(rxhd_buffer_putc, &b, fmt, ap);
#endif
    va_end(ap);
    b.text[b.used] = '\0';
    if (b.overflow) {
        rxhd_log_skipped++;
        return;
    }

    /* Call sites start with "[TAG] tick=N " or "[TAG] id=N ". Repeat that prefix on
     * continuation lines, splitting only at spaces so fields stay intact. */
    prefix_end = strchr(b.text, ' ');
    if (prefix_end != NULL) prefix_end = strchr(prefix_end + 1, ' ');
    if (prefix_end == NULL || (unsigned)(prefix_end - b.text) >= RXHD_LINE_BYTES - 3U) {
        rxhd_log_skipped++;
        return;
    }
    prefix = (unsigned)(prefix_end + 1 - b.text);
    next = b.text + prefix;
    end = b.text + b.used;
    while (end > next && (end[-1] == '\n' || end[-1] == '\r')) --end;
    while (next < end) {
        const char *stop = end;
        unsigned room = RXHD_LINE_BYTES - prefix - 2U;
        if ((unsigned)(end - next) > room) {
            stop = next + room;
            while (stop > next && *stop != ' ') --stop;
            if (stop == next) {
                rxhd_log_skipped++;
                return;
            }
        }
        if (xSemaphoreTake(lock, 0) != pdPASS) {
            rxhd_log_skipped++;
            return;
        }
        for (i = 0; i < prefix; ++i)
            stdio_printf_stubs.stdio_port_sputc(NULL, b.text[i]);
        while (next < stop)
            stdio_printf_stubs.stdio_port_sputc(NULL, *next++);
        stdio_printf_stubs.stdio_port_sputc(NULL, '\r');
        stdio_printf_stubs.stdio_port_sputc(NULL, '\n');
        xSemaphoreGive(lock);
        vTaskDelay(1);
        while (next < end && *next == ' ') ++next;
    }
}

static unsigned long ticks_ms(uint32_t ticks)
{
    return (unsigned long)(((uint64_t)ticks * 1000U) / configTICK_RATE_HZ);
}

/* Called by the existing priority-1 WLAN diagnostics task every two seconds.
 * Snapshots are individually consistent, but are not a packet-by-packet trace.
 * Totals never reset; the delta fields cover the indicated sample_ms only. */
void carbox_rxhd_report(void)
{
    static uint32_t previous[RXHD_COUNT], previous_tick, previous_processed;
    static uint32_t previous_full, previous_fail, previous_self, previous_overflow;
    static uint32_t previous_cb_max, quiet, initialized;
    static uint32_t previous_in_use, previous_ncm_marker;
    static const unsigned errors[] = {
        RXHD_PBUF_FAIL, RXHD_COPY_FAIL, RXHD_INPUT_REJECT, RXHD_POOL_EMPTY,
        RXHD_MEMP_FAIL, RXHD_MBOX_FAIL, RXHD_BATCH_TASK_FAIL, RXHD_BATCH_ISR_FAIL,
        RXHD_BATCH_RETRY, RXHD_BATCH_BAD, RXHD_STACK_REJECT,
        RXHD_NCM_POOL_FAIL, RXHD_NCM_POST_FAIL
    };
    uint32_t c[RXHD_COUNT], tick, zc, cb_age, waiting = 0U;
    struct rxhd_stack_state stack;
    struct rxhd_timer_state timer;
    unsigned i, event = 0U, stalled;

    for (i = 0; i < RXHD_COUNT; ++i)
        c[i] = __atomic_load_n(&rxhd_counters[i], __ATOMIC_RELAXED);
    carbox_rxhd_stack_snapshot(&stack);
    zc = carbox_rxhd_zc_outstanding();
    carbox_rxhd_timer_snapshot(&timer);
    /* Read time after all snapshots, avoiding unsigned age underflow if a
     * producer runs between sampling time and copying its newer timestamp. */
    tick = xTaskGetTickCount();
    cb_age = timer.callback_active ? (uint32_t)(tick - timer.callback_since) : 0U;
    for (i = 0; i < RXHD_TIMER_WAIT_SLOTS; ++i)
        if (timer.waits[i].task != NULL) ++waiting;
    stalled = initialized &&
        ((previous_in_use && stack.in_use &&
          c[RXHD_WLAN_DONE] == previous[RXHD_WLAN_DONE]) ||
         (previous_ncm_marker && stack.ncm_marker &&
          c[RXHD_NCM_DONE] == previous[RXHD_NCM_DONE]));
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
        if (c[errors[i]] != previous[errors[i]]) event = 1U;
    event |= waiting || stalled || ticks_ms(cb_age) >= 100U ||
        timer.full_waits != previous_full || timer.self_waits != previous_self ||
        timer.send_fail != previous_fail || timer.wait_overflow != previous_overflow ||
        (timer.callback_max_ticks != previous_cb_max &&
         ticks_ms(timer.callback_max_ticks) >= 100U);

    if (!initialized || event || ++quiet >= 5U) {
#define C(id) ((unsigned long)c[RXHD_##id])
#define S(field) ((unsigned long)stack.field)
#define T(field) ((unsigned long)timer.field)
        carbox_rxhd_log("[RX_HANDOVER] tick=%lu sample_ms=%lu delta_in/out/msg/timer=%lu/%lu/%lu/%lu no_progress=%u totals_in/copy/zc/out=%lu/%lu/%lu/%lu pbuf_fail=%lu copy_fail=%lu input_reject=%lu zc_held=%lu log_skip=%lu\n",
               (unsigned long)tick, ticks_ms(tick - previous_tick),
               (unsigned long)(c[RXHD_WLAN_IN] - previous[RXHD_WLAN_IN]),
               (unsigned long)(c[RXHD_WLAN_DONE] - previous[RXHD_WLAN_DONE]),
               (unsigned long)(c[RXHD_MSG_DONE] - previous[RXHD_MSG_DONE]),
               (unsigned long)(timer.processed - previous_processed), stalled,
               C(WLAN_IN), C(COPY), C(ZC), C(WLAN_DONE), C(PBUF_FAIL),
               C(COPY_FAIL), C(INPUT_REJECT), (unsigned long)zc,
               (unsigned long)rxhd_log_skipped);
        carbox_rxhd_log("[RX_BATCH] tick=%lu mbox_now=%lu in_use=%lu pending=%lu armed/available=%lu/%lu max_batch=%lu totals_pool_empty/memp_fail/mbox_fail=%lu/%lu/%lu post_fail_task/isr=%lu/%lu retry/ok=%lu/%lu bad=%lu stack_reject=%lu\n",
               (unsigned long)tick, S(mbox), S(in_use), S(pending), S(timer_armed),
               S(timer_available), S(max_batch), C(POOL_EMPTY), C(MEMP_FAIL),
               C(MBOX_FAIL), C(BATCH_TASK_FAIL), C(BATCH_ISR_FAIL),
               C(BATCH_RETRY), C(BATCH_RECOVER), C(BATCH_BAD), C(STACK_REJECT));
        carbox_rxhd_log("[RX_NCM] tick=%lu totals_in/out=%lu/%lu pool_fail=%lu post_fail=%lu marker=%lu\n",
               (unsigned long)tick, C(NCM_IN), C(NCM_DONE), C(NCM_POOL_FAIL),
               C(NCM_POST_FAIL), S(ncm_marker));
        carbox_rxhd_log("[RX_TIMERQ] tick=%lu depth=%lu/%lu processed=%lu full_wait/self/fail=%lu/%lu/%lu wait_overflow=%lu waiting=%lu cb_start/done=%lu/%lu cb_active=%lu cb_age/max_ms=%lu/%lu timer=%p function=%p daemon=%p\n",
               (unsigned long)tick, T(depth), T(capacity), T(processed), T(full_waits),
               T(self_waits), T(send_fail), T(wait_overflow), (unsigned long)waiting,
               T(callback_started), T(callback_done), T(callback_active),
               ticks_ms(cb_age), ticks_ms(timer.callback_max_ticks),
               timer.callback_timer, (void *)timer.callback_function, timer.daemon);
        for (i = 0; i < RXHD_TIMER_WAIT_SLOTS; ++i) {
            const struct rxhd_timer_wait *w = &timer.waits[i];
            if (w->task == NULL) continue;
            carbox_rxhd_log("[RX_TIMER_WAIT] tick=%lu task=%s/%p cmd=%ld timer=%p age_ms=%lu wait_ticks=%lu self=%u\n",
                   (unsigned long)tick, w->task_name, w->task, (long)w->command,
                   w->timer, ticks_ms(tick - w->since_tick),
                   (unsigned long)w->wait_ticks, w->task == timer.daemon);
        }
        quiet = 0U;
#undef C
#undef S
#undef T
    }
    memcpy(previous, c, sizeof(previous));
    previous_tick = tick;
    previous_processed = timer.processed;
    previous_full = timer.full_waits;
    previous_self = timer.self_waits;
    previous_fail = timer.send_fail;
    previous_overflow = timer.wait_overflow;
    previous_cb_max = timer.callback_max_ticks;
    previous_in_use = stack.in_use;
    previous_ncm_marker = stack.ncm_marker;
    initialized = 1U;
}
#endif
