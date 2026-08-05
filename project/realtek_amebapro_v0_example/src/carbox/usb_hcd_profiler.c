#include "usb_hcd_profiler.h"

#include <stdint.h>
#include <string.h>

#include "cmsis.h"
#include "diag.h"
#include "FreeRTOS.h"
#include "task.h"

#ifndef CONFIG_USB_HCD_PROFILE
#define CONFIG_USB_HCD_PROFILE 0
#endif

#if CONFIG_USB_HCD_PROFILE

#define USBPROF_CHANNEL_COUNT 16U
#define USBPROF_EP_TYPE_COUNT 4U
#define USBPROF_URB_STATE_COUNT 8U

typedef struct usbprof_stats_s {
	uint32_t submit_calls;
	uint32_t submit_errors;
	uint32_t submit_bytes;
	uint32_t submit_in;
	uint32_t submit_out;
	uint32_t submit_ep_type[USBPROF_EP_TYPE_COUNT];
	uint32_t submit_cycles;
	uint32_t submit_cycles_max;
	uint32_t urb_polls;
	uint32_t urb_state[USBPROF_URB_STATE_COUNT];
	uint32_t urb_state_other;
	uint32_t first_nonidle_observations;
	uint32_t transfer_size_calls;
	uint32_t transfer_bytes;
	uint32_t isr_sema_gives;
	uint32_t isr_sema_errors;
	uint32_t isr_task_wakes;
} usbprof_stats_t;

static usbprof_stats_t usbprof_live;
static usbprof_stats_t usbprof_snapshot;
static uint16_t usbprof_pending_channels;

/*
 * These signatures match the prebuilt USB host ABI.  --wrap observes calls
 * while leaving the vendor HCD implementation and return values unchanged.
 * All hot-path counters are 32-bit so task/ISR updates need no 64-bit helper.
 */
extern uint8_t __real_usbh_hcd_hc_submit_request(
	void *hcd, uint8_t channel, uint8_t direction, uint8_t ep_type,
	uint8_t token, uint8_t *buffer, uint16_t length);
extern uint8_t __real_usbh_hcd_hc_get_urb_state(void *hcd, uint8_t channel);
extern uint32_t __real_usbh_hcd_hc_get_transfer_size(void *hcd,
						     uint8_t channel);

uint8_t __wrap_usbh_hcd_hc_submit_request(
	void *hcd, uint8_t channel, uint8_t direction, uint8_t ep_type,
	uint8_t token, uint8_t *buffer, uint16_t length)
{
	uint32_t start = DWT->CYCCNT;
	uint8_t result = __real_usbh_hcd_hc_submit_request(
		hcd, channel, direction, ep_type, token, buffer, length);
	uint32_t elapsed = DWT->CYCCNT - start;

	usbprof_live.submit_calls++;
	usbprof_live.submit_bytes += length;
	usbprof_live.submit_cycles += elapsed;
	if (elapsed > usbprof_live.submit_cycles_max) {
		usbprof_live.submit_cycles_max = elapsed;
	}
	if (direction != 0U) {
		usbprof_live.submit_in++;
	} else {
		usbprof_live.submit_out++;
	}
	if (ep_type < USBPROF_EP_TYPE_COUNT) {
		usbprof_live.submit_ep_type[ep_type]++;
	}
	if (result == 0U && channel < USBPROF_CHANNEL_COUNT) {
		usbprof_pending_channels |= (uint16_t)(1U << channel);
	} else if (result != 0U) {
		usbprof_live.submit_errors++;
	}
	return result;
}

uint8_t __wrap_usbh_hcd_hc_get_urb_state(void *hcd, uint8_t channel)
{
	uint8_t state = __real_usbh_hcd_hc_get_urb_state(hcd, channel);

	usbprof_live.urb_polls++;
	if (state < USBPROF_URB_STATE_COUNT) {
		usbprof_live.urb_state[state]++;
	} else {
		usbprof_live.urb_state_other++;
	}
	if (state != 0U && channel < USBPROF_CHANNEL_COUNT &&
	    (usbprof_pending_channels & (uint16_t)(1U << channel)) != 0U) {
		usbprof_pending_channels &= (uint16_t)~(1U << channel);
		usbprof_live.first_nonidle_observations++;
	}
	return state;
}

uint32_t __wrap_usbh_hcd_hc_get_transfer_size(void *hcd, uint8_t channel)
{
	uint32_t size = __real_usbh_hcd_hc_get_transfer_size(hcd, channel);

	usbprof_live.transfer_size_calls++;
	usbprof_live.transfer_bytes += size;
	return size;
}

void usb_hcd_profiler_isr_sema_give(int success, int task_woken)
{
	usbprof_live.isr_sema_gives++;
	if (!success) {
		usbprof_live.isr_sema_errors++;
	}
	if (task_woken) {
		usbprof_live.isr_task_wakes++;
	}
}

static uint32_t usbprof_cycles_to_ns(uint32_t cycles, uint32_t count)
{
	if (count == 0U || SystemCoreClock == 0U) {
		return 0U;
	}
	return (uint32_t)(((uint64_t)cycles * 1000000000ULL) /
			  ((uint64_t)count * SystemCoreClock));
}

static uint32_t usbprof_single_cycles_to_ns(uint32_t cycles)
{
	if (SystemCoreClock == 0U) {
		return 0U;
	}
	return (uint32_t)(((uint64_t)cycles * 1000000000ULL) /
			  SystemCoreClock);
}

void usb_hcd_profiler_report(uint32_t sequence)
{
	uint32_t submit_average = 0U;
	uint32_t transfer_average = 0U;

	/* Snapshot once per existing 10-second report; measured paths stay lockless. */
	taskENTER_CRITICAL();
	usbprof_snapshot = usbprof_live;
	memset(&usbprof_live, 0, sizeof(usbprof_live));
	taskEXIT_CRITICAL();

	if (usbprof_snapshot.submit_calls != 0U) {
		submit_average = usbprof_snapshot.submit_bytes /
				 usbprof_snapshot.submit_calls;
	}
	if (usbprof_snapshot.transfer_size_calls != 0U) {
		transfer_average = usbprof_snapshot.transfer_bytes /
				   usbprof_snapshot.transfer_size_calls;
	}

	rt_printf("[USBPROF][%lu] window_ms=10000 submit=%lu/%luB avg=%luB "
		  "in/out=%lu/%lu ep=ctrl/isoc/bulk/intr:%lu/%lu/%lu/%lu "
		  "error=%lu submit_ns_avg/max=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)usbprof_snapshot.submit_calls,
		  (unsigned long)usbprof_snapshot.submit_bytes,
		  (unsigned long)submit_average,
		  (unsigned long)usbprof_snapshot.submit_in,
		  (unsigned long)usbprof_snapshot.submit_out,
		  (unsigned long)usbprof_snapshot.submit_ep_type[0],
		  (unsigned long)usbprof_snapshot.submit_ep_type[1],
		  (unsigned long)usbprof_snapshot.submit_ep_type[2],
		  (unsigned long)usbprof_snapshot.submit_ep_type[3],
		  (unsigned long)usbprof_snapshot.submit_errors,
		  (unsigned long)usbprof_cycles_to_ns(
			usbprof_snapshot.submit_cycles,
			usbprof_snapshot.submit_calls),
		  (unsigned long)usbprof_single_cycles_to_ns(
			usbprof_snapshot.submit_cycles_max));
	rt_printf("[USBPROF][%lu] urb polls=%lu states0-7="
		  "%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu other=%lu first_nonidle=%lu "
		  "xfer=%lu/%luB avg=%luB irq_give/wake/error=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)usbprof_snapshot.urb_polls,
		  (unsigned long)usbprof_snapshot.urb_state[0],
		  (unsigned long)usbprof_snapshot.urb_state[1],
		  (unsigned long)usbprof_snapshot.urb_state[2],
		  (unsigned long)usbprof_snapshot.urb_state[3],
		  (unsigned long)usbprof_snapshot.urb_state[4],
		  (unsigned long)usbprof_snapshot.urb_state[5],
		  (unsigned long)usbprof_snapshot.urb_state[6],
		  (unsigned long)usbprof_snapshot.urb_state[7],
		  (unsigned long)usbprof_snapshot.urb_state_other,
		  (unsigned long)usbprof_snapshot.first_nonidle_observations,
		  (unsigned long)usbprof_snapshot.transfer_size_calls,
		  (unsigned long)usbprof_snapshot.transfer_bytes,
		  (unsigned long)transfer_average,
		  (unsigned long)usbprof_snapshot.isr_sema_gives,
		  (unsigned long)usbprof_snapshot.isr_task_wakes,
		  (unsigned long)usbprof_snapshot.isr_sema_errors);
}

#else

void usb_hcd_profiler_isr_sema_give(int success, int task_woken)
{
	(void)success;
	(void)task_woken;
}

void usb_hcd_profiler_report(uint32_t sequence)
{
	(void)sequence;
}

#endif /* CONFIG_USB_HCD_PROFILE */
