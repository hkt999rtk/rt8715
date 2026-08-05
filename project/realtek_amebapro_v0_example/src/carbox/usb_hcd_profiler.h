#ifndef CARBOX_USB_HCD_PROFILER_H
#define CARBOX_USB_HCD_PROFILER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void usb_hcd_profiler_report(uint32_t sequence);
void usb_hcd_profiler_isr_sema_give(int success, int task_woken);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_USB_HCD_PROFILER_H */
