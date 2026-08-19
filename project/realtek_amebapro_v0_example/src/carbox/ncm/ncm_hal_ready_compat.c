#include "ncm_tx_abi.h"
#include "usb_boot_profiler.h"
#include "diag.h"

/* Globalized only in the derived link archive; the vendor archive is intact. */
extern usbh_cdc_ncm_host_hal_t usbh_cdc_ncm_host_user;
extern int carbox_vendor_usbh_cdc_ncm_send_data(u8 *buf, u32 len);

int usbh_cdc_ncm_send_data(u8 *buf, u32 len)
{
	static u8 ready_compat_reported;
	usbh_cdc_ncm_host_hal_t *dev = &usbh_cdc_ncm_host_user;
	int status;

	/*
	 * The new HAL rejects TX until cdc_ncm_is_ready is set, but the existing
	 * link path can start lwIP before cdc_ncm_set_is_ready() runs.  Restore the
	 * former behavior only after both physical attach and the NCM context are
	 * present; entering vendor TX before ncm_hw_connect is asserted is unsafe.
	 */
	if (dev->cdc_ncm_is_ready == 0U && dev->ncm_hw_connect != 0U &&
	    dev->data[0] != 0UL) {
		dev->cdc_ncm_is_ready = 1U;
		if (ready_compat_reported == 0U) {
			ready_compat_reported = 1U;
			rt_printf("[NCMCOMPAT] accepted late ready after attach/context\r\n");
		}
	}

	status = carbox_vendor_usbh_cdc_ncm_send_data(buf, len);
	carbox_usb_boot_profiler_record_ncm_tx(status);
	return status;
}
