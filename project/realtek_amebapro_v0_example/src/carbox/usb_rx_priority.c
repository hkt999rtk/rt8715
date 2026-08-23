#include "usb_rx_priority.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "diag.h"
#include "hal_timer.h"

#ifndef CONFIG_USB_CH4_QUEUE_FRONT
#define CONFIG_USB_CH4_QUEUE_FRONT 1
#endif

#if CONFIG_USB_CH4_QUEUE_FRONT

/* Closed USB host ABI values used by the existing NCM HCD profiler. */
#define USB_RX_CHANNEL 4U
#define USB_EP_BULK 2U
#define USB_URB_DONE 1U
#define USB_URB_NOTREADY 2U
#define USB_RX_QUEUE_CAPACITY 16U
#define USB_CORE_QUEUE_OFFSET 0x7cU

typedef struct usb_rx_priority_stats_s {
	uint32_t ch4_haint;
	uint32_t ch4_notify;
	uint32_t front_attempt;
	uint32_t front_ok;
	uint32_t front_fail;
	uint32_t ch4_dequeue;
	uint32_t pending_peak;
	uint32_t timestamp_overflow;
	uint32_t dequeue_unpaired;
	uint32_t missing_haint;
	uint32_t haint_to_notify_samples;
	uint64_t haint_to_notify_total_us;
	uint32_t haint_to_notify_max_us;
	uint32_t notify_to_dequeue_samples;
	uint64_t notify_to_dequeue_total_us;
	uint32_t notify_to_dequeue_max_us;
	uint32_t rx_submit;
	uint32_t rx_submit_error;
	uint32_t rx_submit_replaced;
	uint32_t rx_first_haint;
	uint32_t rx_done;
	uint32_t rx_done_untracked;
	uint32_t rx_notready_polls;
	uint32_t rx_other_polls;
	uint32_t submit_to_haint_samples;
	uint64_t submit_to_haint_total_us;
	uint32_t submit_to_haint_max_us;
	uint32_t submit_to_done_samples;
	uint64_t submit_to_done_total_us;
	uint32_t submit_to_done_max_us;
	uint32_t submit_to_done_ge_1ms;
	uint32_t submit_to_done_ge_5ms;
	uint32_t submit_to_done_ge_10ms;
	uint32_t submit_to_done_ge_20ms;
	uint32_t haint_to_done_samples;
	uint64_t haint_to_done_total_us;
	uint32_t haint_to_done_max_us;
	uint32_t done_to_rearm_samples;
	uint64_t done_to_rearm_total_us;
	uint32_t done_to_rearm_max_us;
	uint32_t last_done_request_id;
	uint32_t haints_per_done_samples;
	uint64_t haints_per_done_total;
	uint32_t haints_per_done_max;
	uint32_t last_haint_to_done_samples;
	uint64_t last_haint_to_done_total_us;
	uint32_t last_haint_to_done_max_us;
	uint32_t slow_request_id;
	uint32_t slow_submit_to_done_us;
	uint32_t slow_haint_count;
	uint32_t slow_submit_to_first_haint_us;
	uint32_t slow_last_haint_to_done_us;
} usb_rx_priority_stats_t;

static usb_rx_priority_stats_t usb_rx_priority_live;
static uint8_t usb_rx_bulk_in_seen;
static uint8_t usb_haint_time_valid;
static uint32_t usb_haint_time_us;
static void *usb_core_queue;

/*
 * Channel-4 messages are inserted at the front, so their timestamps form a
 * stack as well.  The queue capacity is 16 in the closed USB core.
 */
static uint32_t usb_ch4_time_stack[USB_RX_QUEUE_CAPACITY];
static uint32_t usb_ch4_pending;
static uint8_t usb_rx_request_pending;
static uint8_t usb_rx_first_haint_seen;
static uint8_t usb_rx_done_time_valid;
static uint32_t usb_rx_submit_time_us;
static uint32_t usb_rx_first_haint_time_us;
static uint32_t usb_rx_last_haint_time_us;
static uint32_t usb_rx_done_time_us;
static uint32_t usb_rx_next_request_id;
static uint32_t usb_rx_request_id;
static uint32_t usb_rx_request_haint_count;
static uint8_t usb_rx_last_haint_valid;

extern uint8_t __real_usbh_hcd_hc_submit_request(
	void *hcd, uint8_t channel, uint8_t direction, uint8_t ep_type,
	uint8_t token, uint8_t *buffer, uint16_t length);
extern uint32_t __real_usbh_hal_hc_read_interrupt(void);
extern void __real_usbh_core_notify_urb_state_change(void *core,
						     uint8_t channel);
extern int __real_usb_os_queue_send(void *queue, const void *message,
				    uint32_t timeout_ms);
extern int __real_usb_os_queue_receive(void *queue, void *message,
				       uint32_t timeout_ms);
extern uint8_t __real_usbh_hcd_hc_get_urb_state(void *hcd,
						uint8_t channel);

static void usb_rx_accumulate(uint32_t elapsed_us, uint64_t *total_us,
			      uint32_t *max_us)
{
	*total_us += elapsed_us;
	if (elapsed_us > *max_us) {
		*max_us = elapsed_us;
	}
}

uint8_t __wrap_usbh_hcd_hc_submit_request(
	void *hcd, uint8_t channel, uint8_t direction, uint8_t ep_type,
	uint8_t token, uint8_t *buffer, uint16_t length)
{
	uint8_t is_rx_request = channel == USB_RX_CHANNEL && direction != 0U &&
				 ep_type == USB_EP_BULK;
	uint32_t start_us = is_rx_request != 0U ? hal_read_curtime_us() : 0U;
	uint8_t result = __real_usbh_hcd_hc_submit_request(
		hcd, channel, direction, ep_type, token, buffer, length);

	/* Direction 1 is IN in the closed HCD ABI. */
	if (is_rx_request == 0U) {
		return result;
	}

	usb_rx_priority_live.rx_submit++;
	if (result != 0U) {
		usb_rx_priority_live.rx_submit_error++;
		return result;
	}

	usb_rx_bulk_in_seen = 1U;
	if (usb_rx_request_pending != 0U) {
		usb_rx_priority_live.rx_submit_replaced++;
	}
	if (usb_rx_done_time_valid != 0U) {
		uint32_t elapsed_us = start_us - usb_rx_done_time_us;

		usb_rx_priority_live.done_to_rearm_samples++;
		usb_rx_accumulate(elapsed_us,
				  &usb_rx_priority_live.done_to_rearm_total_us,
				  &usb_rx_priority_live.done_to_rearm_max_us);
		usb_rx_done_time_valid = 0U;
	}
	usb_rx_submit_time_us = start_us;
	usb_rx_next_request_id++;
	if (usb_rx_next_request_id == 0U) {
		usb_rx_next_request_id++;
	}
	usb_rx_request_id = usb_rx_next_request_id;
	usb_rx_request_haint_count = 0U;
	usb_rx_request_pending = 1U;
	usb_rx_first_haint_seen = 0U;
	usb_rx_last_haint_valid = 0U;

	return result;
}

uint32_t __wrap_usbh_hal_hc_read_interrupt(void)
{
	uint32_t pending = __real_usbh_hal_hc_read_interrupt();

	/* Observe only; queue insertion below is the sole scheduling change. */
	if ((pending & (1UL << USB_RX_CHANNEL)) != 0U) {
		uint32_t now_us = hal_read_curtime_us();

		usb_rx_priority_live.ch4_haint++;
		usb_haint_time_us = now_us;
		usb_haint_time_valid = 1U;
		if (usb_rx_request_pending != 0U) {
			usb_rx_request_haint_count++;
			usb_rx_last_haint_time_us = now_us;
			usb_rx_last_haint_valid = 1U;
		}
		if (usb_rx_request_pending != 0U &&
		    usb_rx_first_haint_seen == 0U) {
			uint32_t elapsed_us = now_us - usb_rx_submit_time_us;

			usb_rx_first_haint_seen = 1U;
			usb_rx_first_haint_time_us = now_us;
			usb_rx_priority_live.rx_first_haint++;
			usb_rx_priority_live.submit_to_haint_samples++;
			usb_rx_accumulate(elapsed_us,
					  &usb_rx_priority_live.submit_to_haint_total_us,
					  &usb_rx_priority_live.submit_to_haint_max_us);
		}
	}

	return pending;
}

void __wrap_usbh_core_notify_urb_state_change(void *core, uint8_t channel)
{
	(void)channel;

	/*
	 * Closed usbh_core_t ABI: the event queue handle is at +0x7c.  Some HCD
	 * calls to this function are resolved internally by the archive and cannot
	 * be intercepted with --wrap, but every path still calls the external
	 * usb_os_queue_send().  Discover the queue from any call that is visible.
	 */
	if (core != NULL) {
		void *queue = *(void **)((uint8_t *)core + USB_CORE_QUEUE_OFFSET);

		if (queue != NULL) {
			usb_core_queue = queue;
		}
	}
	__real_usbh_core_notify_urb_state_change(core, channel);
}

int __wrap_usb_os_queue_send(void *queue, const void *message,
			     uint32_t timeout_ms)
{
	BaseType_t sent;
	uint32_t now_us;
	uint8_t timestamp_pushed = 0U;

	if (usb_rx_bulk_in_seen == 0U || queue != usb_core_queue ||
	    message == NULL || *(const uint32_t *)message != USB_RX_CHANNEL) {
		return __real_usb_os_queue_send(queue, message, timeout_ms);
	}

	now_us = hal_read_curtime_us();
	usb_rx_priority_live.ch4_notify++;
	if (usb_haint_time_valid != 0U) {
		usb_rx_priority_live.haint_to_notify_samples++;
		usb_rx_accumulate(now_us - usb_haint_time_us,
				  &usb_rx_priority_live.haint_to_notify_total_us,
				  &usb_rx_priority_live.haint_to_notify_max_us);
	} else {
		usb_rx_priority_live.missing_haint++;
	}
	usb_haint_time_valid = 0U;

	usb_rx_priority_live.front_attempt++;
	taskENTER_CRITICAL();
	if (usb_ch4_pending < USB_RX_QUEUE_CAPACITY) {
		usb_ch4_time_stack[usb_ch4_pending++] = now_us;
		timestamp_pushed = 1U;
		if (usb_ch4_pending > usb_rx_priority_live.pending_peak) {
			usb_rx_priority_live.pending_peak = usb_ch4_pending;
		}
	} else {
		usb_rx_priority_live.timestamp_overflow++;
	}

	/*
	 * Keep the timestamp visible before the queue item can wake and preempt to
	 * usbh_main_task.  FreeRTOS critical sections nest, so the queue API may
	 * safely take its own internal critical section here.
	 */
	sent = xQueueSendToFront((QueueHandle_t)queue, message, (TickType_t)0);
	if (sent != pdPASS && timestamp_pushed != 0U) {
		usb_ch4_pending--;
	}
	taskEXIT_CRITICAL();

	if (sent != pdPASS) {
		usb_rx_priority_live.front_fail++;
		return 4;
	}
	usb_rx_priority_live.front_ok++;
	return 0;
}

int __wrap_usb_os_queue_receive(void *queue, void *message,
				uint32_t timeout_ms)
{
	int result = __real_usb_os_queue_receive(queue, message, timeout_ms);
	uint32_t notify_us = 0U;
	uint32_t now_us;
	uint8_t paired = 0U;

	/* Fallback discovery avoids depending solely on wrapped archive calls. */
	if (result == 0 && usb_core_queue == NULL) {
		TaskHandle_t current = xTaskGetCurrentTaskHandle();
		const char *name = pcTaskGetName(current);

		if (name != NULL && strcmp(name, "usbh_main_task") == 0) {
			usb_core_queue = queue;
		}
	}

	if (result != 0 || queue != usb_core_queue || message == NULL ||
	    *(const uint32_t *)message != USB_RX_CHANNEL) {
		return result;
	}

	now_us = hal_read_curtime_us();
	usb_rx_priority_live.ch4_dequeue++;
	taskENTER_CRITICAL();
	if (usb_ch4_pending != 0U) {
		notify_us = usb_ch4_time_stack[--usb_ch4_pending];
		paired = 1U;
	} else {
		usb_rx_priority_live.dequeue_unpaired++;
	}
	taskEXIT_CRITICAL();

	if (paired != 0U) {
		usb_rx_priority_live.notify_to_dequeue_samples++;
		usb_rx_accumulate(now_us - notify_us,
				  &usb_rx_priority_live.notify_to_dequeue_total_us,
				  &usb_rx_priority_live.notify_to_dequeue_max_us);
	}
	return result;
}

uint8_t __wrap_usbh_hcd_hc_get_urb_state(void *hcd, uint8_t channel)
{
	uint8_t state = __real_usbh_hcd_hc_get_urb_state(hcd, channel);
	uint32_t now_us;
	uint32_t elapsed_us;

	if (channel != USB_RX_CHANNEL) {
		return state;
	}
	if (state == USB_URB_NOTREADY) {
		usb_rx_priority_live.rx_notready_polls++;
		return state;
	}
	if (state != USB_URB_DONE) {
		usb_rx_priority_live.rx_other_polls++;
		return state;
	}

	now_us = hal_read_curtime_us();
	if (usb_rx_request_pending == 0U) {
		usb_rx_priority_live.rx_done_untracked++;
		return state;
	}

	usb_rx_priority_live.rx_done++;
	usb_rx_priority_live.submit_to_done_samples++;
	elapsed_us = now_us - usb_rx_submit_time_us;
	usb_rx_accumulate(elapsed_us,
			  &usb_rx_priority_live.submit_to_done_total_us,
			  &usb_rx_priority_live.submit_to_done_max_us);
	if (elapsed_us >= 1000U) {
		usb_rx_priority_live.submit_to_done_ge_1ms++;
	}
	if (elapsed_us >= 5000U) {
		usb_rx_priority_live.submit_to_done_ge_5ms++;
	}
	if (elapsed_us >= 10000U) {
		usb_rx_priority_live.submit_to_done_ge_10ms++;
	}
	if (elapsed_us >= 20000U) {
		usb_rx_priority_live.submit_to_done_ge_20ms++;
	}
	if (usb_rx_first_haint_seen != 0U) {
		usb_rx_priority_live.haint_to_done_samples++;
		usb_rx_accumulate(now_us - usb_rx_first_haint_time_us,
				  &usb_rx_priority_live.haint_to_done_total_us,
				  &usb_rx_priority_live.haint_to_done_max_us);
	}
	usb_rx_priority_live.last_done_request_id = usb_rx_request_id;
	usb_rx_priority_live.haints_per_done_samples++;
	usb_rx_priority_live.haints_per_done_total +=
		usb_rx_request_haint_count;
	if (usb_rx_request_haint_count >
	    usb_rx_priority_live.haints_per_done_max) {
		usb_rx_priority_live.haints_per_done_max =
			usb_rx_request_haint_count;
	}
	if (usb_rx_last_haint_valid != 0U) {
		uint32_t last_elapsed_us = now_us - usb_rx_last_haint_time_us;

		usb_rx_priority_live.last_haint_to_done_samples++;
		usb_rx_accumulate(
			last_elapsed_us,
			&usb_rx_priority_live.last_haint_to_done_total_us,
			&usb_rx_priority_live.last_haint_to_done_max_us);
	}
	if (elapsed_us > usb_rx_priority_live.slow_submit_to_done_us) {
		usb_rx_priority_live.slow_request_id = usb_rx_request_id;
		usb_rx_priority_live.slow_submit_to_done_us = elapsed_us;
		usb_rx_priority_live.slow_haint_count =
			usb_rx_request_haint_count;
		usb_rx_priority_live.slow_submit_to_first_haint_us =
			usb_rx_first_haint_seen != 0U ?
			usb_rx_first_haint_time_us - usb_rx_submit_time_us : 0U;
		usb_rx_priority_live.slow_last_haint_to_done_us =
			usb_rx_last_haint_valid != 0U ?
			now_us - usb_rx_last_haint_time_us : 0U;
	}
	usb_rx_request_pending = 0U;
	usb_rx_done_time_us = now_us;
	usb_rx_done_time_valid = 1U;
	return state;
}

void carbox_usb_rx_priority_report(uint32_t sequence)
{
	usb_rx_priority_stats_t stats;
	uint32_t pending;

	taskENTER_CRITICAL();
	stats = usb_rx_priority_live;
	memset(&usb_rx_priority_live, 0, sizeof(usb_rx_priority_live));
	pending = usb_ch4_pending;
	taskEXIT_CRITICAL();

	rt_printf("[USBRXPRIO][%lu] haint/notify/front_try/ok/fail/deq="
		  "%lu/%lu/%lu/%lu/%lu/%lu queue ch4_now/peak/ts_overflow/unpaired="
		  "%lu/%lu/%lu/%lu active=%u\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.ch4_haint,
		  (unsigned long)stats.ch4_notify,
		  (unsigned long)stats.front_attempt,
		  (unsigned long)stats.front_ok,
		  (unsigned long)stats.front_fail,
		  (unsigned long)stats.ch4_dequeue,
		  (unsigned long)pending,
		  (unsigned long)stats.pending_peak,
		  (unsigned long)stats.timestamp_overflow,
		  (unsigned long)stats.dequeue_unpaired,
		  (unsigned)usb_rx_bulk_in_seen);
	rt_printf("[USBRXPRIO][%lu] us n/avg/max haint_to_notify="
		  "%lu/%lu/%lu notify_to_deq=%lu/%lu/%lu missing_haint=%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.haint_to_notify_samples,
		  stats.haint_to_notify_samples != 0U ?
			(unsigned long)(stats.haint_to_notify_total_us /
					stats.haint_to_notify_samples) : 0UL,
		  (unsigned long)stats.haint_to_notify_max_us,
		  (unsigned long)stats.notify_to_dequeue_samples,
		  stats.notify_to_dequeue_samples != 0U ?
			(unsigned long)(stats.notify_to_dequeue_total_us /
					stats.notify_to_dequeue_samples) : 0UL,
		  (unsigned long)stats.notify_to_dequeue_max_us,
		  (unsigned long)stats.missing_haint);
	rt_printf("[USBRXLIFE][%lu] submit/error/replaced/haint/done/untracked="
		  "%lu/%lu/%lu/%lu/%lu/%lu polls notready/other=%lu/%lu "
		  "pending=%u\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.rx_submit,
		  (unsigned long)stats.rx_submit_error,
		  (unsigned long)stats.rx_submit_replaced,
		  (unsigned long)stats.rx_first_haint,
		  (unsigned long)stats.rx_done,
		  (unsigned long)stats.rx_done_untracked,
		  (unsigned long)stats.rx_notready_polls,
		  (unsigned long)stats.rx_other_polls,
		  (unsigned)usb_rx_request_pending);
	rt_printf("[USBRXLIFE][%lu] us n/avg/max submit_to_haint=%lu/%lu/%lu "
		  "submit_to_done=%lu/%lu/%lu haint_to_done=%lu/%lu/%lu "
		  "done_to_rearm=%lu/%lu/%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.submit_to_haint_samples,
		  stats.submit_to_haint_samples != 0U ?
			(unsigned long)(stats.submit_to_haint_total_us /
					stats.submit_to_haint_samples) : 0UL,
		  (unsigned long)stats.submit_to_haint_max_us,
		  (unsigned long)stats.submit_to_done_samples,
		  stats.submit_to_done_samples != 0U ?
			(unsigned long)(stats.submit_to_done_total_us /
					stats.submit_to_done_samples) : 0UL,
		  (unsigned long)stats.submit_to_done_max_us,
		  (unsigned long)stats.haint_to_done_samples,
		  stats.haint_to_done_samples != 0U ?
			(unsigned long)(stats.haint_to_done_total_us /
					stats.haint_to_done_samples) : 0UL,
		  (unsigned long)stats.haint_to_done_max_us,
		  (unsigned long)stats.done_to_rearm_samples,
		  stats.done_to_rearm_samples != 0U ?
			(unsigned long)(stats.done_to_rearm_total_us /
					stats.done_to_rearm_samples) : 0UL,
		  (unsigned long)stats.done_to_rearm_max_us);
	rt_printf("[USBRXLIFE][%lu] submit_to_done tail >=1/5/10/20ms="
		  "%lu/%lu/%lu/%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.submit_to_done_ge_1ms,
		  (unsigned long)stats.submit_to_done_ge_5ms,
		  (unsigned long)stats.submit_to_done_ge_10ms,
		  (unsigned long)stats.submit_to_done_ge_20ms);
	rt_printf("[USBRXLIFE][%lu] request last_done_id=%lu haints_per_done "
		  "n/avg/max=%lu/%lu/%lu last_haint_to_done_us="
		  "%lu/%lu/%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.last_done_request_id,
		  (unsigned long)stats.haints_per_done_samples,
		  stats.haints_per_done_samples != 0U ?
			(unsigned long)(stats.haints_per_done_total /
				stats.haints_per_done_samples) : 0UL,
		  (unsigned long)stats.haints_per_done_max,
		  (unsigned long)stats.last_haint_to_done_samples,
		  stats.last_haint_to_done_samples != 0U ?
			(unsigned long)(stats.last_haint_to_done_total_us /
				stats.last_haint_to_done_samples) : 0UL,
		  (unsigned long)stats.last_haint_to_done_max_us);
	rt_printf("[USBRXLIFE][%lu][SLOW] id=%lu submit_to_done_us=%lu "
		  "haints=%lu submit_to_first_haint_us=%lu "
		  "last_haint_to_done_us=%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.slow_request_id,
		  (unsigned long)stats.slow_submit_to_done_us,
		  (unsigned long)stats.slow_haint_count,
		  (unsigned long)stats.slow_submit_to_first_haint_us,
		  (unsigned long)stats.slow_last_haint_to_done_us);
}

#else

void carbox_usb_rx_priority_report(uint32_t sequence)
{
	(void)sequence;
}

#endif
