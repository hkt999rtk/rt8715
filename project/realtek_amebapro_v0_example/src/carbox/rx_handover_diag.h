/* Bounded handover diagnostics. No allocation or UART in producer paths. */
#ifndef CARBOX_RX_HANDOVER_DIAG_H
#define CARBOX_RX_HANDOVER_DIAG_H

#include <stdint.h>

#ifndef CONFIG_RX_HANDOVER_DIAG
#define CONFIG_RX_HANDOVER_DIAG 0
#endif

#if CONFIG_RX_HANDOVER_DIAG
enum rxhd_counter {
    RXHD_WLAN_IN, RXHD_COPY, RXHD_ZC, RXHD_PBUF_FAIL, RXHD_COPY_FAIL,
    RXHD_INPUT_REJECT, RXHD_POOL_EMPTY, RXHD_MEMP_FAIL, RXHD_MBOX_FAIL,
    RXHD_BATCH_TASK_FAIL, RXHD_BATCH_ISR_FAIL, RXHD_BATCH_RETRY,
    RXHD_BATCH_RECOVER, RXHD_BATCH_BAD, RXHD_STACK_REJECT, RXHD_MSG_DONE,
    RXHD_WLAN_DONE, RXHD_NCM_IN, RXHD_NCM_DONE, RXHD_NCM_POOL_FAIL,
    RXHD_NCM_POST_FAIL, RXHD_COUNT
};
extern uint32_t rxhd_counters[RXHD_COUNT];
/* Producers include an ISR and multiple tasks. Never reset these counters.
 * Relaxed atomic increments prevent lost updates without masking interrupts. */
#define RXHD_ADD(id, n) ((void)__atomic_fetch_add(&rxhd_counters[id], (n), __ATOMIC_RELAXED))
#define RXHD_INC(id) RXHD_ADD(id, 1U)

struct rxhd_stack_state {
    uint32_t mbox, in_use, pending, timer_armed, timer_available, max_batch;
    uint32_t ncm_marker;
};
void carbox_rxhd_stack_snapshot(struct rxhd_stack_state *out);
uint32_t carbox_rxhd_zc_outstanding(void);

#define RXHD_TIMER_WAIT_SLOTS 4U
struct rxhd_timer_wait {
    void *task;
    void *timer;
    int32_t command;
    uint32_t since_tick, wait_ticks;
    char task_name[16];
};
struct rxhd_timer_state {
    uint32_t depth, capacity, processed, full_waits, self_waits, send_fail;
    uint32_t wait_overflow, callback_started, callback_done, callback_since;
    uint32_t callback_active, callback_max_ticks;
    void *callback_timer;
    uintptr_t callback_function;
    void *daemon;
    struct rxhd_timer_wait waits[RXHD_TIMER_WAIT_SLOTS];
};
/* Called only at the actual full-queue blocking point (scheduler suspended). */
void carbox_rxhd_timer_block(void *queue, const void *item, uint32_t ticks);
/* Caller holds a critical section; clears a saved opaque task handle. */
void carbox_rxhd_timer_task_deleted(void *task);
void carbox_rxhd_timer_snapshot(struct rxhd_timer_state *out);
void carbox_rxhd_report(void);
/* Only the common diagnostic task may call these reporters/logger. */
void carbox_rxhd_log(const char *fmt, ...);
void carbox_recvframe_trace_report(void);
/* Caller holds a critical section; 0 means not a registered active free pool. */
uint32_t carbox_recvframe_trace_epoch(const void *queue);
#else
#define RXHD_ADD(id, n) ((void)0)
#define RXHD_INC(id) ((void)0)
#define carbox_rxhd_report() ((void)0)
#define carbox_rxhd_log(...) ((void)0)
#define carbox_recvframe_trace_report() ((void)0)
#define carbox_recvframe_trace_epoch(queue) (0U)
#endif

#endif
