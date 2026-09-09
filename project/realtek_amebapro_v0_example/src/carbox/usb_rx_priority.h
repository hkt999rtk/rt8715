#ifndef CARBOX_USB_RX_PRIORITY_H
#define CARBOX_USB_RX_PRIORITY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void carbox_usb_rx_priority_report(uint32_t sequence);
void carbox_usb_rx_priority_set_car_flow(uint16_t local_port,
					 uint16_t peer_port);
void carbox_usb_rx_priority_mark_hid(uint32_t parser_time_us);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_USB_RX_PRIORITY_H */
