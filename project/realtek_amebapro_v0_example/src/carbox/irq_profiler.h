#ifndef CARBOX_IRQ_PROFILER_H
#define CARBOX_IRQ_PROFILER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Install the common peripheral-vector hook synchronously, before the rest of
 * the application registers USB/WLAN/GDMA handlers.  The hot path only counts
 * the active external IRQ and branches to the original vector.
 */
void carbox_irq_profiler_init(void);

/*
 * Rotate the IRQ counter bank at the same instant as the PC sample bank.  The
 * caller already masks interrupts for its short 10-second statistics snapshot.
 */
void carbox_irq_profiler_snapshot(uint32_t profiler_irq,
				  uint32_t profiler_callbacks);

/* Print the prepared snapshot later, from the low-priority reporter task. */
void carbox_irq_profiler_report(uint32_t sequence, uint32_t window_ms);

/*
 * Optional USB IRQ-to-task handoff probes.  The compatibility wrappers call
 * these at the HCD semaphore give/take boundary; the implementation rejects
 * non-USB IRQ semaphore traffic before recording anything.
 */
void carbox_irq_profiler_usb_sema_give(void *handle, int success,
				       int higher_priority_task_woken);

/* Narrow channel-4 ordering probe called by the existing HCD submit wrapper. */
void carbox_irq_profiler_usb_ch4_submit(void);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_IRQ_PROFILER_H */
