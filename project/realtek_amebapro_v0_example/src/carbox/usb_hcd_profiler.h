#ifndef CARBOX_USB_HCD_PROFILER_H
#define CARBOX_USB_HCD_PROFILER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void usb_hcd_profiler_report(uint32_t sequence);
void usb_hcd_profiler_isr_sema_give(int success, int task_woken);
void usb_tx_lifetime_ncm_begin(const void *source, uint32_t length);
void usb_tx_lifetime_ncm_end(int result);
void usb_tx_lifetime_source_release(void);

typedef struct usb_screen_probe_stats_s {
	uint32_t submits;
	uint32_t submit_errors;
	uint32_t completions;
	uint32_t completion_cycles;
	uint32_t completion_cycles_max;
	uint32_t pending_now;
	uint32_t pending_max;
} usb_screen_probe_stats_t;

void usb_screen_probe_snapshot(usb_screen_probe_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_USB_HCD_PROFILER_H */
