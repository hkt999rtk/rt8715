#include <stdint.h>

#include "usb_boot_profiler.h"

/*
 * The customer cdc_ncm.o was compiled against a newer USB host status enum,
 * while the unchanged usbh core in lib_usbsmart.a still returns the original
 * RTL8195B values.  Disassembly of the old/new cdc_ncm.o pair shows these
 * corresponding values:
 *
 *     original core     customer cdc_ncm
 *           0                  0
 *           1                  2
 *           2                  1
 *           3                  4
 *           4                  3
 *
 * Only cdc_ncm.o is symbol-renamed to call this adapter.  Other USB clients
 * continue to receive the native status values from usbh_ctrl_request().
 */
extern int usbh_ctrl_request(void *host, const void *setup, uint8_t *data);

int carbox_ncm_ctrl_request(void *host, const void *setup, uint8_t *data)
{
	int status = usbh_ctrl_request(host, setup, data);
	int mapped_status;

	switch (status) {
	case 1:
		mapped_status = 2;
		break;
	case 2:
		mapped_status = 1;
		break;
	case 3:
		mapped_status = 4;
		break;
	case 4:
		mapped_status = 3;
		break;
	default:
		mapped_status = status;
		break;
	}

	carbox_usb_boot_profiler_record_ncm_ctrl(status, mapped_status);
	return mapped_status;
}
