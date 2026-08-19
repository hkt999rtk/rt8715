#ifndef CARBOX_USB_BOOT_PROFILER_H
#define CARBOX_USB_BOOT_PROFILER_H

#include <stdint.h>

void carbox_usb_boot_profiler_report(uint32_t sequence);
void carbox_usb_boot_profiler_record_ncm_ctrl(int native_status,
					       int mapped_status);
void carbox_usb_boot_profiler_record_ctrl(const void *setup, int status);
void carbox_usb_boot_profiler_record_ncm_tx(int status);

#endif /* CARBOX_USB_BOOT_PROFILER_H */
