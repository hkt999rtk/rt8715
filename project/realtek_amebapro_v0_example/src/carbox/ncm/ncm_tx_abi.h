#ifndef __NCM_TX_ABI_H__
#define __NCM_TX_ABI_H__

/*
 * Self-contained ABI for the firmware-side ncm_tx.c.
 *
 * The firmware must be able to compile ncm_tx.c without including anything
 * under component/usb_lib (that tree is not shipped to third parties). This
 * header mirrors the exact layouts/typedefs that ncm_tx.c depends on, taken
 * from:
 *   component/usb_lib/common/usb_os.h
 *   component/usb_lib/host/cdc_ncm/usbh_cdc_ncm_hal.h
 *   component/usb_lib/lib/ncm/usb_ncm.h
 *
 * Keep these definitions in sync with those headers. The struct field order
 * is an ABI contract with lib_usbsmart.a (which allocates struct cdc_ncm_ctx
 * and usbh_cdc_ncm_host_hal_t).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* --- basic types (same as usb_lib basic_types.h; re-typedef is harmless) --- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef uint32_t gfp_t;

/* --- USB OS abstraction (usb_os.h) --------------------------------------- */
/* usb_os_lock_t / usb_os_sema_t == FreeRTOS SemaphoreHandle_t == void* */
typedef void *usb_os_lock_t;
typedef void *usb_os_sema_t;

#define USB_OS_SEMA_TIMEOUT 0xFFFFFFFFUL

/* GTimer (mbed, USE_TIMER) — resolved from component/common/mbed targets/hal dirs */
#ifdef USE_TIMER
#include "objects.h"
#include "timer_api.h"
#endif

/* --- CDC NCM transfer descriptors (usb_ncm.h, packed) --------------------- */
struct usb_cdc_ncm_ntb_parameters {
	u16	wLength;
	u16	bmNtbFormatsSupported;
	u32	dwNtbInMaxSize;
	u16	wNdpInDivisor;
	u16	wNdpInPayloadRemainder;
	u16	wNdpInAlignment;
	u16	wPadding1;
	u32	dwNtbOutMaxSize;
	u16	wNdpOutDivisor;
	u16	wNdpOutPayloadRemainder;
	u16	wNdpOutAlignment;
	u16	wNtbOutMaxDatagrams;
} __attribute__ ((packed));

#define USB_CDC_NCM_NTH16_SIGN		0x484D434E /* NCMH */
#define USB_CDC_NCM_NDP16_NOCRC_SIGN	0x304D434E /* NCM0 */

struct usb_cdc_ncm_nth16 {
	u32	dwSignature;
	u16	wHeaderLength;
	u16	wSequence;
	u16	wBlockLength;
	u16	wNdpIndex;
} __attribute__ ((packed));

struct usb_cdc_ncm_dpe16 {
	u16	wDatagramIndex;
	u16	wDatagramLength;
} __attribute__((__packed__));

struct usb_cdc_ncm_ndp16 {
	u32	dwSignature;
	u16	wLength;
	u16	wNextNdpIndex;
	struct	usb_cdc_ncm_dpe16 dpe16[0];
} __attribute__ ((packed));

/* --- driver flags / tuning (usb_ncm.h) ------------------------------------ */
#define ALIGN(val, align)     (((val) + ((align) - 1)) & ~((align) - 1))

#define CDC_NCM_DPT_DATAGRAMS_MAX		2//40
#define CDC_NCM_RESTART_TIMER_DATAGRAM_CNT	3
#define CDC_NCM_TIMER_PENDING_CNT		2
#define CDC_NCM_FLAG_NDP_TO_END			0x02

/* --- forward decls for pointer-only members ------------------------------- */
struct usb_cdc_ncm_desc;
struct usb_cdc_mbim_desc;
struct usb_cdc_mbim_extended_desc;
struct usb_cdc_ether_desc;
struct usb_interface;
struct usb_host;

/* --- usbh_cdc_ncm_host_hal_t (usbh_cdc_ncm_hal.h, pack(1)) ---------------- */
struct usbh_cdc_ncm_host_hal_t;
typedef void (*usb_report_data)(u8 *buf, u32 len);
typedef int (*usb_process_data)(struct usbh_cdc_ncm_host_hal_t *dev, u8 *buf, u32 len);

#pragma pack(push)
#pragma pack(1)
typedef struct {
	usb_report_data             report_data;
	usb_process_data            process_data;
	usb_os_sema_t               cdc_ncm_tx_sema;
	volatile u8                 cdc_ncm_is_ready;
	volatile u8                 ncm_hw_connect;
	volatile u8                 ncm_init_success;
	unsigned long		data[5];
	struct usb_host       *host;
	u8 *ncm_out_buf;
	u8 *out_buf;
	u8 *in_buf;
	u32 usblen;
	u32 datalen;
	struct usb_interface	 *pusb_intf;
} usbh_cdc_ncm_host_hal_t;
#pragma pack(pop)

/* --- struct cdc_ncm_ctx (usb_ncm.h, field order is ABI) ------------------- */
struct cdc_ncm_ctx {
	struct usb_cdc_ncm_ntb_parameters ncm_parm;

	const struct usb_cdc_ncm_desc *func_desc;
	const struct usb_cdc_mbim_desc *mbim_desc;
	const struct usb_cdc_mbim_extended_desc *mbim_extended_desc;
	const struct usb_cdc_ether_desc *ether_desc;

	struct usb_interface *control;
	struct usb_interface *data;
#ifdef USE_TIMER
	gtimer_t send_timer;
	usb_os_sema_t bh_sema;		/* wake tx_bh thread */
	u8    bh_need_wake;		/* set by ISR, consumed at ISR tail for portEND_SWITCHING_ISR */
	u8    timer_ok;			/* gtimer init success flag */
#endif

#ifdef USE_THREAD
	u8    bh_created;		/* thread creation flag */
#endif
	void *tx_rem_data;
	void *tx_curr_data;

	u32 tx_curr_len;
	u32 tx_rem_len;
	u32 tx_rem_sign;

	usb_os_lock_t mtx;
	usb_os_lock_t net;
	int drvflags;
	u32 timer_interval;
	u32 max_ndp_size;
	struct usb_cdc_ncm_ndp16 *delayed_ndp16;

	u32 tx_timer_pending;
	u32 tx_curr_frame_num;
	u32 rx_max;
	u32 tx_max;
	u32 tx_curr_size;
	u32 max_datagram_size;
	u16 tx_max_datagrams;
	u16 tx_remainder;
	u16 tx_modulus;
	u16 tx_ndp_modulus;
	u16 tx_seq;
	u16 rx_seq;
	u16 min_tx_pkt;
	u16 maxpacket;			/* USB bulk out wMaxPacketSize */

	/* statistics */
	u32 tx_curr_frame_payload;
	u32 tx_reason_ntb_full;
	u32 tx_reason_ndp_full;
	u32 tx_reason_timeout;
	u32 tx_reason_max_datagram;
	__u64 tx_overhead;
	__u64 tx_ntbs;
	__u64 rx_overhead;
	__u64 rx_ntbs;

#ifdef USE_STAT
	__u64 tx_datagrams;
	__u64 tx_payload_bytes;
	u64   tx_last_ts;
	u32   tx_call_cnt;
	u32   tx_interval_sum;
	u32   tx_interval_max;
	u32   tx_proc_sum;
	u32   tx_proc_max;
	u32   tx_send_sum;
	u32   tx_send_max;
	u8    stat_started;
#endif

#ifdef USE_TIMER
	u32 tx_timer_call_cnt;
	u64 tx_timer_last;
	u32 tx_timer_interval_sum;
	u32 tx_timer_interval_max;
	u64 t_timer_send;
#endif
	u32   tx_exit;
};

/* --- USB OS functions provided by lib_usbsmart.a (usb_os.o) --------------- */
int usb_os_lock_create(usb_os_lock_t *lock);
int usb_os_lock_delete(usb_os_lock_t lock);
int usb_os_lock(usb_os_lock_t lock);
int usb_os_unlock(usb_os_lock_t lock);
int usb_os_sema_create(usb_os_sema_t *sema);
int usb_os_sema_take(usb_os_sema_t sema, u32 timeout_ms);

#endif /* __NCM_TX_ABI_H__ */
