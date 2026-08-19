#include "usb_boot_profiler.h"
#include "ncm_tx_abi.h"

#include <stdint.h>

#include "diag.h"

#ifndef CONFIG_IRQ_PROFILE_USB_CH4_NCM
#define CONFIG_IRQ_PROFILE_USB_CH4_NCM 0
#endif

/*
 * These offsets are part of the unchanged customer USB host objects, not a
 * guessed public ABI.  They were verified from lib_usbsmart.a disassembly:
 *
 *   usb_host + 113/114/115/116 = speed/port/connect_seen/port_attached
 *   usb_host + 152             = pointer to the active usb_device
 *   usb_device + 44            = host enumeration state
 *   usbh_cdc_ncm_host + 176/177 = transfer/class state
 *
 * Keeping the values here lets the periodic report preserve early boot state
 * without changing the closed-source host core or printing in its IRQ path.
 */
#define USBBOOT_HOST_SPEED_OFFSET       113U
#define USBBOOT_HOST_PORT_OFFSET        114U
#define USBBOOT_HOST_CONNECT_OFFSET     115U
#define USBBOOT_HOST_ATTACHED_OFFSET    116U
#define USBBOOT_HOST_DEVICE_OFFSET      152U
#define USBBOOT_DEVICE_STATE_OFFSET      44U
#define USBBOOT_NCM_TRANSFER_OFFSET     176U
#define USBBOOT_NCM_CLASS_OFFSET        177U

extern unsigned char usbh_cdc_ncm_host[];
extern usbh_cdc_ncm_host_hal_t usbh_cdc_ncm_host_user;

extern int __real_usbh_core_connect(void *host);
extern int __real_usbh_core_disconnect(void *host);
extern void __real_cdc_ncm_connect(void);
extern void __real_cdc_ncm_set_is_ready(void);
#if !CONFIG_IRQ_PROFILE_USB_CH4_NCM
extern int __real_usbh_ctrl_request(void *host, const void *setup,
				    uint8_t *data);
#endif

static volatile uintptr_t usbboot_last_host;
static volatile uint32_t usbboot_core_connect_calls;
static volatile uint32_t usbboot_core_disconnect_calls;
static volatile int32_t usbboot_core_connect_last;
static volatile int32_t usbboot_core_disconnect_last;
static volatile uint8_t usbboot_connect_state_before = 0xffU;
static volatile uint8_t usbboot_connect_state_after = 0xffU;
static volatile uint8_t usbboot_disconnect_state_before = 0xffU;
static volatile uint8_t usbboot_disconnect_connect_before;
static volatile uint8_t usbboot_disconnect_attached_before;
static volatile uint32_t usbboot_ncm_connect_calls;
static volatile uint32_t usbboot_ncm_ready_calls;
static volatile uint32_t usbboot_ctrl_calls;
static volatile uint32_t usbboot_ctrl_ok;
static volatile uint32_t usbboot_ctrl_busy;
static volatile uint32_t usbboot_ctrl_error;
static volatile int32_t usbboot_ctrl_native_last;
static volatile int32_t usbboot_ctrl_mapped_last;
static volatile uint32_t usbboot_tx_calls;
static volatile uint32_t usbboot_tx_ok;
static volatile uint32_t usbboot_tx_error;
static volatile int32_t usbboot_tx_last;
static volatile uint32_t usbboot_all_ctrl_calls;
static volatile uint32_t usbboot_all_ctrl_ok;
static volatile uint32_t usbboot_all_ctrl_busy;
static volatile uint32_t usbboot_all_ctrl_error;
static volatile uint8_t usbboot_ctrl_request_type;
static volatile uint8_t usbboot_ctrl_request;
static volatile uint16_t usbboot_ctrl_value;
static volatile uint16_t usbboot_ctrl_index;
static volatile uint16_t usbboot_ctrl_length;
static volatile int32_t usbboot_all_ctrl_last;

static uint16_t usbboot_get_le16(const uint8_t *value)
{
	return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static uint8_t usbboot_get_core_state(uintptr_t host_address)
{
	uintptr_t device_address;

	if (host_address == 0U) {
		return 0xffU;
	}
	device_address = *(volatile const uintptr_t *)(host_address +
						 USBBOOT_HOST_DEVICE_OFFSET);
	if (device_address == 0U) {
		return 0xffU;
	}
	return *(volatile const uint8_t *)(device_address +
					 USBBOOT_DEVICE_STATE_OFFSET);
}

int __wrap_usbh_core_connect(void *host)
{
	int status;

	usbboot_last_host = (uintptr_t)host;
	usbboot_core_connect_calls++;
	usbboot_connect_state_before =
		usbboot_get_core_state((uintptr_t)host);
	status = __real_usbh_core_connect(host);
	usbboot_core_connect_last = status;
	usbboot_connect_state_after =
		usbboot_get_core_state((uintptr_t)host);
	return status;
}

int __wrap_usbh_core_disconnect(void *host)
{
	int status;

	usbboot_last_host = (uintptr_t)host;
	usbboot_core_disconnect_calls++;
	usbboot_disconnect_state_before =
		usbboot_get_core_state((uintptr_t)host);
	usbboot_disconnect_connect_before =
		((volatile const uint8_t *)host)[USBBOOT_HOST_CONNECT_OFFSET];
	usbboot_disconnect_attached_before =
		((volatile const uint8_t *)host)[USBBOOT_HOST_ATTACHED_OFFSET];
	status = __real_usbh_core_disconnect(host);
	usbboot_core_disconnect_last = status;
	return status;
}

void __wrap_cdc_ncm_connect(void)
{
	usbboot_ncm_connect_calls++;
	__real_cdc_ncm_connect();
}

void __wrap_cdc_ncm_set_is_ready(void)
{
	usbboot_ncm_ready_calls++;
	__real_cdc_ncm_set_is_ready();
}

void carbox_usb_boot_profiler_record_ctrl(const void *setup, int status)
{
	const uint8_t *request = (const uint8_t *)setup;

	usbboot_all_ctrl_calls++;
	usbboot_all_ctrl_last = status;
	if (request != NULL) {
		usbboot_ctrl_request_type = request[0];
		usbboot_ctrl_request = request[1];
		usbboot_ctrl_value = usbboot_get_le16(request + 2U);
		usbboot_ctrl_index = usbboot_get_le16(request + 4U);
		usbboot_ctrl_length = usbboot_get_le16(request + 6U);
	}
	if (status == 0) {
		usbboot_all_ctrl_ok++;
	} else if (status == 1 || status == 2) {
		usbboot_all_ctrl_busy++;
	} else {
		usbboot_all_ctrl_error++;
	}
}

#if !CONFIG_IRQ_PROFILE_USB_CH4_NCM
int __wrap_usbh_ctrl_request(void *host, const void *setup, uint8_t *data)
{
	int status = __real_usbh_ctrl_request(host, setup, data);

	carbox_usb_boot_profiler_record_ctrl(setup, status);
	return status;
}
#endif

void carbox_usb_boot_profiler_record_ncm_ctrl(int native_status,
					       int mapped_status)
{
	usbboot_ctrl_calls++;
	usbboot_ctrl_native_last = native_status;
	usbboot_ctrl_mapped_last = mapped_status;
	if (native_status == 0) {
		usbboot_ctrl_ok++;
	} else if (native_status == 1 || native_status == 2) {
		usbboot_ctrl_busy++;
	} else {
		usbboot_ctrl_error++;
	}
}

void carbox_usb_boot_profiler_record_ncm_tx(int status)
{
	usbboot_tx_calls++;
	usbboot_tx_last = status;
	if (status == 0) {
		usbboot_tx_ok++;
	} else {
		usbboot_tx_error++;
	}
}

void carbox_usb_boot_profiler_report(uint32_t sequence)
{
	uintptr_t host_address = usbboot_last_host;
	uint8_t core_state = 0xffU;
	uint8_t speed = 0xffU;
	uint8_t port = 0xffU;
	uint8_t connect_seen = 0U;
	uint8_t port_attached = 0U;

	if (host_address != 0U) {
		volatile const uint8_t *host =
			(volatile const uint8_t *)host_address;

		speed = host[USBBOOT_HOST_SPEED_OFFSET];
		port = host[USBBOOT_HOST_PORT_OFFSET];
		connect_seen = host[USBBOOT_HOST_CONNECT_OFFSET];
		port_attached = host[USBBOOT_HOST_ATTACHED_OFFSET];
		core_state = usbboot_get_core_state(host_address);
	}

	rt_printf("[USBBOOT][%lu] core connect/disconnect=%lu/%lu last=%ld/%ld "
		  "host=->%08lx state=%u port/speed=%u/%u flags connect/attached=%u/%u\r\n",
		  (unsigned long)sequence,
		  (unsigned long)usbboot_core_connect_calls,
		  (unsigned long)usbboot_core_disconnect_calls,
		  (long)usbboot_core_connect_last,
		  (long)usbboot_core_disconnect_last,
		  (unsigned long)host_address, (unsigned int)core_state,
		  (unsigned int)port, (unsigned int)speed,
		  (unsigned int)connect_seen, (unsigned int)port_attached);
	rt_printf("[USBBOOT][%lu] event_state connect before/after=%u/%u "
		  "disconnect before=%u flags_before connect/attached=%u/%u\r\n",
		  (unsigned long)sequence,
		  (unsigned int)usbboot_connect_state_before,
		  (unsigned int)usbboot_connect_state_after,
		  (unsigned int)usbboot_disconnect_state_before,
		  (unsigned int)usbboot_disconnect_connect_before,
		  (unsigned int)usbboot_disconnect_attached_before);

	rt_printf("[USBBOOT][%lu] ncm transfer/class=%u/%u events connect/ready=%lu/%lu "
		  "hal ready/hw/init/context=%u/%u/%u/%u ctrl calls/ok/busy/error=%lu/%lu/%lu/%lu "
		  "last native/mapped=%ld/%ld tx calls/ok/error/last=%lu/%lu/%lu/%ld\r\n",
		  (unsigned long)sequence,
		  (unsigned int)usbh_cdc_ncm_host[USBBOOT_NCM_TRANSFER_OFFSET],
		  (unsigned int)usbh_cdc_ncm_host[USBBOOT_NCM_CLASS_OFFSET],
		  (unsigned long)usbboot_ncm_connect_calls,
		  (unsigned long)usbboot_ncm_ready_calls,
		  (unsigned int)usbh_cdc_ncm_host_user.cdc_ncm_is_ready,
		  (unsigned int)usbh_cdc_ncm_host_user.ncm_hw_connect,
		  (unsigned int)usbh_cdc_ncm_host_user.ncm_init_success,
		  usbh_cdc_ncm_host_user.data[0] != 0UL ? 1U : 0U,
		  (unsigned long)usbboot_ctrl_calls,
		  (unsigned long)usbboot_ctrl_ok,
		  (unsigned long)usbboot_ctrl_busy,
		  (unsigned long)usbboot_ctrl_error,
		  (long)usbboot_ctrl_native_last,
		  (long)usbboot_ctrl_mapped_last,
		  (unsigned long)usbboot_tx_calls,
		  (unsigned long)usbboot_tx_ok,
		  (unsigned long)usbboot_tx_error,
		  (long)usbboot_tx_last);

	rt_printf("[USBBOOT][%lu] ctrl all/ok/busy/error=%lu/%lu/%lu/%lu "
		  "last type/request/value/index/len/status=%02x/%02x/%04x/%04x/%u/%ld\r\n",
		  (unsigned long)sequence,
		  (unsigned long)usbboot_all_ctrl_calls,
		  (unsigned long)usbboot_all_ctrl_ok,
		  (unsigned long)usbboot_all_ctrl_busy,
		  (unsigned long)usbboot_all_ctrl_error,
		  (unsigned int)usbboot_ctrl_request_type,
		  (unsigned int)usbboot_ctrl_request,
		  (unsigned int)usbboot_ctrl_value,
		  (unsigned int)usbboot_ctrl_index,
		  (unsigned int)usbboot_ctrl_length,
		  (long)usbboot_all_ctrl_last);
}
