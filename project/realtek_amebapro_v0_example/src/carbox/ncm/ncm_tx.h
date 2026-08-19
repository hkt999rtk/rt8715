#ifndef __NCM_TX_H__
#define __NCM_TX_H__

/*
 * Public API for the NCM TX aggregation / GTimer flush path (ncm_tx.c).
 * Include this header to drive the TX path without the RX/unwrap code.
 */
#include "usb_ncm.h"
#include "usbh_cdc_ncm_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fill/aggregate one NTB frame. Returns a sendable buffer in *out_len,
 * or NULL if the datagram was buffered for a later timer flush. */
void *cdc_ncm_fill_tx_frame(usbh_cdc_ncm_host_hal_t *dev, void *data, size_t len, __u32 sign, size_t *out_len);

/* TX entry point: aggregate `data`/`len` and return an NTB ready to send. */
void *cdc_ncm_tx_fixup(usbh_cdc_ncm_host_hal_t *dev, void *data, size_t len, gfp_t flags, size_t *out_len);

/* Lock helpers around cdc_ncm_tx_fixup() + bulk send. */
void cdc_ncm_tx_lock(usbh_cdc_ncm_host_hal_t *dev);
void cdc_ncm_tx_unlock(usbh_cdc_ncm_host_hal_t *dev);

/* Initialize TX locks, GTimer and stat counters. Called from cdc_ncm setup. */
void ncm_timer_init(usbh_cdc_ncm_host_hal_t *dev);

/* TX send-latency accounting (USE_STAT). */
void ncm_stat_send_latency(usbh_cdc_ncm_host_hal_t *dev, u32 elapsed_us);

#ifdef __cplusplus
}
#endif

#endif /* __NCM_TX_H__ */
