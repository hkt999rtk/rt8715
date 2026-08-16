#include "usb_hcd_profiler.h"

#include <stdint.h>
#include <string.h>

#include "cmsis.h"
#include "diag.h"
#include "irq_profiler.h"
#include "FreeRTOS.h"
#include "task.h"

#ifndef CONFIG_USB_HCD_PROFILE
#define CONFIG_USB_HCD_PROFILE 0
#endif

#ifndef CONFIG_USB_HCD_CHANNEL_PROFILE
#define CONFIG_USB_HCD_CHANNEL_PROFILE 0
#endif

#ifndef CONFIG_USB_TX_LIFETIME_PROFILE
#define CONFIG_USB_TX_LIFETIME_PROFILE 0
#endif

#ifndef CONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE
#define CONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE 0
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

#if CONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE
	if (channel == 4U) {
		carbox_irq_profiler_usb_ch4_submit();
	}
#endif
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

void usb_tx_lifetime_ncm_begin(const void *source, uint32_t length)
{
	(void)source;
	(void)length;
}

void usb_tx_lifetime_ncm_end(int result)
{
	(void)result;
}

void usb_tx_lifetime_source_release(void)
{
}

#elif CONFIG_USB_TX_LIFETIME_PROFILE

/*
 * Correlate one synchronous customer NCM send with the HCD OUT requests it
 * issues.  This stage is observation-only: every wrapped function calls the
 * customer implementation exactly once and preserves its return value.
 */
#define USBTXLIFE_CHANNELS 16U
#define USBTXLIFE_STATES    8U

typedef struct usbtxlife_channel_s {
	uint32_t start_cycles;
	uint32_t scope_sequence;
	const uint8_t *buffer;
	uint16_t length;
	uint8_t active;
} usbtxlife_channel_t;

typedef struct usbtxlife_scope_s {
	const uint8_t *source;
	uint32_t source_length;
	uint32_t start_cycles;
	uint32_t end_cycles;
	uint32_t sequence;
	uint32_t submits;
	uint32_t terminals;
	uint8_t active;
	uint8_t awaiting_release;
} usbtxlife_scope_t;

typedef struct usbtxlife_stats_s {
	uint32_t logical_calls;
	uint32_t logical_ok;
	uint32_t logical_error;
	uint32_t logical_bytes;
	uint32_t logical_cycles;
	uint32_t logical_cycles_max;
	uint32_t logical_no_submit;
	uint32_t logical_one_submit;
	uint32_t logical_multi_submit;
	uint32_t logical_submits;
	uint32_t logical_terminals;
	uint32_t logical_return_pending;
	uint32_t source_releases;
	uint32_t source_release_anomaly;
	uint32_t source_release_pending;
	uint32_t source_release_cycles;
	uint32_t source_release_cycles_max;
	uint32_t submit_calls;
	uint32_t submit_errors;
	uint32_t submit_bytes;
	uint32_t submit_scoped;
	uint32_t submit_replace_pending;
	uint32_t submit_direct_exact;
	uint32_t submit_direct_range;
	uint32_t submit_internal;
	uint32_t terminal_calls;
	uint32_t terminal_state[USBTXLIFE_STATES];
	uint32_t terminal_state_other;
	uint32_t terminal_cycles;
	uint32_t terminal_cycles_max;
	uint32_t urb_polls;
	uint32_t urb_idle_polls;
	uint32_t live_now;
	uint32_t live_max;
} usbtxlife_stats_t;

static usbtxlife_channel_t usbtxlife_channel[USBTXLIFE_CHANNELS];
static usbtxlife_scope_t usbtxlife_scope;
static usbtxlife_stats_t usbtxlife_live;
static usbtxlife_stats_t usbtxlife_snapshot;

extern uint8_t __real_usbh_hcd_hc_submit_request(
	void *hcd, uint8_t channel, uint8_t direction, uint8_t ep_type,
	uint8_t token, uint8_t *buffer, uint16_t length);
extern uint8_t __real_usbh_hcd_hc_get_urb_state(void *hcd, uint8_t channel);

static uint32_t usbtxlife_cycles_to_us(uint32_t cycles, uint32_t count)
{
	uint32_t cycles_per_us = SystemCoreClock / 1000000U;

	return count != 0U && cycles_per_us != 0U ?
		cycles / cycles_per_us / count : 0U;
}

static uint32_t usbtxlife_pending_for_scope(uint32_t sequence)
{
	uint32_t channel;
	uint32_t pending = 0U;

	for (channel = 0U; channel < USBTXLIFE_CHANNELS; channel++) {
		if (usbtxlife_channel[channel].active != 0U &&
		    usbtxlife_channel[channel].scope_sequence == sequence) {
			pending++;
		}
	}
	return pending;
}

void usb_tx_lifetime_ncm_begin(const void *source, uint32_t length)
{
	taskENTER_CRITICAL();
	if (usbtxlife_scope.active != 0U ||
	    usbtxlife_scope.awaiting_release != 0U) {
		usbtxlife_live.source_release_anomaly++;
	}
	usbtxlife_scope.sequence++;
	if (usbtxlife_scope.sequence == 0U) {
		usbtxlife_scope.sequence = 1U;
	}
	usbtxlife_scope.source = (const uint8_t *)source;
	usbtxlife_scope.source_length = length;
	usbtxlife_scope.start_cycles = DWT->CYCCNT;
	usbtxlife_scope.submits = 0U;
	usbtxlife_scope.terminals = 0U;
	usbtxlife_scope.active = 1U;
	usbtxlife_scope.awaiting_release = 0U;
	taskEXIT_CRITICAL();
}

void usb_tx_lifetime_ncm_end(int result)
{
	uint32_t elapsed;
	uint32_t pending;

	taskENTER_CRITICAL();
	if (usbtxlife_scope.active == 0U) {
		usbtxlife_live.source_release_anomaly++;
		taskEXIT_CRITICAL();
		return;
	}
	elapsed = DWT->CYCCNT - usbtxlife_scope.start_cycles;
	pending = usbtxlife_pending_for_scope(usbtxlife_scope.sequence);
	usbtxlife_live.logical_calls++;
	usbtxlife_live.logical_bytes += usbtxlife_scope.source_length;
	usbtxlife_live.logical_cycles += elapsed;
	if (elapsed > usbtxlife_live.logical_cycles_max) {
		usbtxlife_live.logical_cycles_max = elapsed;
	}
	if (result == 0) {
		usbtxlife_live.logical_ok++;
	} else {
		usbtxlife_live.logical_error++;
	}
	if (usbtxlife_scope.submits == 0U) {
		usbtxlife_live.logical_no_submit++;
	} else if (usbtxlife_scope.submits == 1U) {
		usbtxlife_live.logical_one_submit++;
	} else {
		usbtxlife_live.logical_multi_submit++;
	}
	usbtxlife_live.logical_submits += usbtxlife_scope.submits;
	usbtxlife_live.logical_terminals += usbtxlife_scope.terminals;
	if (pending != 0U) {
		usbtxlife_live.logical_return_pending++;
	}
	usbtxlife_scope.end_cycles = DWT->CYCCNT;
	usbtxlife_scope.active = 0U;
	usbtxlife_scope.awaiting_release = 1U;
	taskEXIT_CRITICAL();
}

void usb_tx_lifetime_source_release(void)
{
	uint32_t elapsed;
	uint32_t pending;

	taskENTER_CRITICAL();
	if (usbtxlife_scope.awaiting_release == 0U) {
		usbtxlife_live.source_release_anomaly++;
		taskEXIT_CRITICAL();
		return;
	}
	elapsed = DWT->CYCCNT - usbtxlife_scope.end_cycles;
	pending = usbtxlife_pending_for_scope(usbtxlife_scope.sequence);
	usbtxlife_live.source_releases++;
	usbtxlife_live.source_release_cycles += elapsed;
	if (elapsed > usbtxlife_live.source_release_cycles_max) {
		usbtxlife_live.source_release_cycles_max = elapsed;
	}
	if (pending != 0U) {
		usbtxlife_live.source_release_pending++;
	}
	usbtxlife_scope.awaiting_release = 0U;
	usbtxlife_scope.source = NULL;
	usbtxlife_scope.source_length = 0U;
	taskEXIT_CRITICAL();
}

uint8_t __wrap_usbh_hcd_hc_submit_request(
	void *hcd, uint8_t channel, uint8_t direction, uint8_t ep_type,
	uint8_t token, uint8_t *buffer, uint16_t length)
{
	uint32_t start = DWT->CYCCNT;
	uint8_t result = __real_usbh_hcd_hc_submit_request(
		hcd, channel, direction, ep_type, token, buffer, length);

	/* NCM data uses host bulk OUT.  Ignore control, interrupt and all RX URBs. */
	if (direction == 0U && ep_type == 2U && channel < USBTXLIFE_CHANNELS) {
		usbtxlife_channel_t *entry = &usbtxlife_channel[channel];

		taskENTER_CRITICAL();
		usbtxlife_live.submit_calls++;
		usbtxlife_live.submit_bytes += length;
		if (result != 0U) {
			usbtxlife_live.submit_errors++;
		} else {
			if (entry->active != 0U) {
				usbtxlife_live.submit_replace_pending++;
			} else {
				usbtxlife_live.live_now++;
				if (usbtxlife_live.live_now > usbtxlife_live.live_max) {
					usbtxlife_live.live_max = usbtxlife_live.live_now;
				}
			}
			entry->start_cycles = start;
			entry->scope_sequence = usbtxlife_scope.active != 0U ?
				usbtxlife_scope.sequence : 0U;
			entry->buffer = buffer;
			entry->length = length;
			entry->active = 1U;
			if (usbtxlife_scope.active != 0U) {
				uintptr_t source = (uintptr_t)usbtxlife_scope.source;
				uintptr_t submit = (uintptr_t)buffer;

				usbtxlife_scope.submits++;
				usbtxlife_live.submit_scoped++;
				if (submit == source &&
				    (uint32_t)length == usbtxlife_scope.source_length) {
					usbtxlife_live.submit_direct_exact++;
				} else if (submit >= source &&
					   (uint32_t)length <= usbtxlife_scope.source_length &&
					   submit - source <=
					   usbtxlife_scope.source_length - (uint32_t)length) {
					usbtxlife_live.submit_direct_range++;
				} else {
					usbtxlife_live.submit_internal++;
				}
			}
		}
		taskEXIT_CRITICAL();
	}
	return result;
}

uint8_t __wrap_usbh_hcd_hc_get_urb_state(void *hcd, uint8_t channel)
{
	uint8_t state = __real_usbh_hcd_hc_get_urb_state(hcd, channel);

	if (channel < USBTXLIFE_CHANNELS &&
	    usbtxlife_channel[channel].active != 0U) {
		taskENTER_CRITICAL();
		if (usbtxlife_channel[channel].active != 0U) {
			usbtxlife_live.urb_polls++;
			if (state == 0U) {
				usbtxlife_live.urb_idle_polls++;
			} else {
				uint32_t elapsed = DWT->CYCCNT -
					usbtxlife_channel[channel].start_cycles;

				usbtxlife_live.terminal_calls++;
				usbtxlife_live.terminal_cycles += elapsed;
				if (elapsed > usbtxlife_live.terminal_cycles_max) {
					usbtxlife_live.terminal_cycles_max = elapsed;
				}
				if (state < USBTXLIFE_STATES) {
					usbtxlife_live.terminal_state[state]++;
				} else {
					usbtxlife_live.terminal_state_other++;
				}
				if (usbtxlife_scope.active != 0U &&
				    usbtxlife_channel[channel].scope_sequence ==
				    usbtxlife_scope.sequence) {
					usbtxlife_scope.terminals++;
				}
				usbtxlife_channel[channel].active = 0U;
				if (usbtxlife_live.live_now != 0U) {
					usbtxlife_live.live_now--;
				}
			}
		}
		taskEXIT_CRITICAL();
	}
	return state;
}

void usb_hcd_profiler_isr_sema_give(int success, int task_woken)
{
	(void)success;
	(void)task_woken;
}

void usb_hcd_profiler_report(uint32_t sequence)
{
	uint32_t live_now;

	taskENTER_CRITICAL();
	usbtxlife_snapshot = usbtxlife_live;
	live_now = usbtxlife_live.live_now;
	memset(&usbtxlife_live, 0, sizeof(usbtxlife_live));
	usbtxlife_live.live_now = live_now;
	usbtxlife_live.live_max = live_now;
	taskEXIT_CRITICAL();

	rt_printf("[USBTXLIFE][%lu] logical calls/ok/error=%lu/%lu/%lu bytes=%lu "
		  "time_us avg/max=%lu/%lu submit none/one/multi=%lu/%lu/%lu "
		  "nested submit/terminal=%lu/%lu "
		  "return_pending=%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)usbtxlife_snapshot.logical_calls,
		  (unsigned long)usbtxlife_snapshot.logical_ok,
		  (unsigned long)usbtxlife_snapshot.logical_error,
		  (unsigned long)usbtxlife_snapshot.logical_bytes,
		  (unsigned long)usbtxlife_cycles_to_us(
			usbtxlife_snapshot.logical_cycles,
			usbtxlife_snapshot.logical_calls),
		  (unsigned long)usbtxlife_cycles_to_us(
			usbtxlife_snapshot.logical_cycles_max, 1U),
		  (unsigned long)usbtxlife_snapshot.logical_no_submit,
		  (unsigned long)usbtxlife_snapshot.logical_one_submit,
		  (unsigned long)usbtxlife_snapshot.logical_multi_submit,
		  (unsigned long)usbtxlife_snapshot.logical_submits,
		  (unsigned long)usbtxlife_snapshot.logical_terminals,
		  (unsigned long)usbtxlife_snapshot.logical_return_pending);
	rt_printf("[USBTXLIFE][%lu] hcd submit/error=%lu/%lu bytes=%lu "
		  "source scoped/exact/range/internal=%lu/%lu/%lu/%lu replace_pending=%lu "
		  "terminal=%lu states0-7=%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu "
		  "other=%lu time_us avg/max=%lu/%lu polls/idle=%lu/%lu live/max=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)usbtxlife_snapshot.submit_calls,
		  (unsigned long)usbtxlife_snapshot.submit_errors,
		  (unsigned long)usbtxlife_snapshot.submit_bytes,
		  (unsigned long)usbtxlife_snapshot.submit_scoped,
		  (unsigned long)usbtxlife_snapshot.submit_direct_exact,
		  (unsigned long)usbtxlife_snapshot.submit_direct_range,
		  (unsigned long)usbtxlife_snapshot.submit_internal,
		  (unsigned long)usbtxlife_snapshot.submit_replace_pending,
		  (unsigned long)usbtxlife_snapshot.terminal_calls,
		  (unsigned long)usbtxlife_snapshot.terminal_state[0],
		  (unsigned long)usbtxlife_snapshot.terminal_state[1],
		  (unsigned long)usbtxlife_snapshot.terminal_state[2],
		  (unsigned long)usbtxlife_snapshot.terminal_state[3],
		  (unsigned long)usbtxlife_snapshot.terminal_state[4],
		  (unsigned long)usbtxlife_snapshot.terminal_state[5],
		  (unsigned long)usbtxlife_snapshot.terminal_state[6],
		  (unsigned long)usbtxlife_snapshot.terminal_state[7],
		  (unsigned long)usbtxlife_snapshot.terminal_state_other,
		  (unsigned long)usbtxlife_cycles_to_us(
			usbtxlife_snapshot.terminal_cycles,
			usbtxlife_snapshot.terminal_calls),
		  (unsigned long)usbtxlife_cycles_to_us(
			usbtxlife_snapshot.terminal_cycles_max, 1U),
		  (unsigned long)usbtxlife_snapshot.urb_polls,
		  (unsigned long)usbtxlife_snapshot.urb_idle_polls,
		  (unsigned long)live_now,
		  (unsigned long)usbtxlife_snapshot.live_max);
	rt_printf("[USBTXLIFE][%lu] source_release calls/anomaly/pending=%lu/%lu/%lu "
		  "after_return_us avg/max=%lu/%lu observation_only=1\r\n",
		  (unsigned long)sequence,
		  (unsigned long)usbtxlife_snapshot.source_releases,
		  (unsigned long)usbtxlife_snapshot.source_release_anomaly,
		  (unsigned long)usbtxlife_snapshot.source_release_pending,
		  (unsigned long)usbtxlife_cycles_to_us(
			usbtxlife_snapshot.source_release_cycles,
			usbtxlife_snapshot.source_releases),
		  (unsigned long)usbtxlife_cycles_to_us(
			usbtxlife_snapshot.source_release_cycles_max, 1U));
}

#elif CONFIG_USB_HCD_CHANNEL_PROFILE

/*
 * Narrow diagnostic for the NCM channels.  Unlike CONFIG_USB_HCD_PROFILE,
 * this deliberately wraps only submit_request: wrapping the very frequent
 * URB-state polling path would perturb the IRQ/CPU measurement we are trying
 * to explain.
 */
#define USBHCH_FIRST_CHANNEL 2U
#define USBHCH_LAST_CHANNEL  4U
#define USBHCH_CHANNEL_COUNT (USBHCH_LAST_CHANNEL - USBHCH_FIRST_CHANNEL + 1U)
#define USBHCH_LENGTH_BINS   5U

typedef struct usbhch_channel_stats_s {
	uint32_t calls;
	uint32_t errors;
	uint32_t bytes;
	uint32_t in_calls;
	uint32_t out_calls;
	uint32_t ep_type[4];
	uint32_t length_bins[USBHCH_LENGTH_BINS];
	uint16_t min_length;
	uint16_t max_length;
} usbhch_channel_stats_t;

static usbhch_channel_stats_t usbhch_live[USBHCH_CHANNEL_COUNT];
static usbhch_channel_stats_t usbhch_snapshot[USBHCH_CHANNEL_COUNT];

extern uint8_t __real_usbh_hcd_hc_submit_request(
	void *hcd, uint8_t channel, uint8_t direction, uint8_t ep_type,
	uint8_t token, uint8_t *buffer, uint16_t length);

static uint32_t usbhch_length_bin(uint16_t length)
{
	if (length <= 64U) {
		return 0U;
	}
	if (length <= 512U) {
		return 1U;
	}
	if (length <= 4096U) {
		return 2U;
	}
	if (length <= 16384U) {
		return 3U;
	}
	return 4U;
}

uint8_t __wrap_usbh_hcd_hc_submit_request(
	void *hcd, uint8_t channel, uint8_t direction, uint8_t ep_type,
	uint8_t token, uint8_t *buffer, uint16_t length)
{

#if CONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE
	if (channel == 4U) {
		carbox_irq_profiler_usb_ch4_submit();
	}
#endif
	uint8_t result = __real_usbh_hcd_hc_submit_request(
		hcd, channel, direction, ep_type, token, buffer, length);

	if (channel >= USBHCH_FIRST_CHANNEL && channel <= USBHCH_LAST_CHANNEL) {
		usbhch_channel_stats_t *stats =
			&usbhch_live[channel - USBHCH_FIRST_CHANNEL];

		stats->calls++;
		stats->bytes += length;
		stats->length_bins[usbhch_length_bin(length)]++;
		if (direction != 0U) {
			stats->in_calls++;
		} else {
			stats->out_calls++;
		}
		if (ep_type < 4U) {
			stats->ep_type[ep_type]++;
		}
		if (stats->calls == 1U || length < stats->min_length) {
			stats->min_length = length;
		}
		if (length > stats->max_length) {
			stats->max_length = length;
		}
		if (result != 0U) {
			stats->errors++;
		}
	}

	return result;
}

void usb_hcd_profiler_isr_sema_give(int success, int task_woken)
{
	(void)success;
	(void)task_woken;
}

void usb_hcd_profiler_report(uint32_t sequence)
{
	uint32_t i;

	taskENTER_CRITICAL();
	memcpy(usbhch_snapshot, usbhch_live, sizeof(usbhch_snapshot));
	memset(usbhch_live, 0, sizeof(usbhch_live));
	taskEXIT_CRITICAL();

	for (i = 0U; i < USBHCH_CHANNEL_COUNT; i++) {
		const usbhch_channel_stats_t *stats = &usbhch_snapshot[i];
		uint32_t average = stats->calls != 0U ?
			stats->bytes / stats->calls : 0U;

		if (stats->calls == 0U) {
			continue;
		}
		rt_printf("[USBHCH][%lu] ch=%lu submit=%lu(%lu/s)/%luB "
			  "avg/min/max=%lu/%u/%u in/out=%lu/%lu error=%lu "
			  "type=ctrl/isoc/bulk/intr:%lu/%lu/%lu/%lu "
			  "len=<=64/<=512/<=4K/<=16K/>16K:%lu/%lu/%lu/%lu/%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)(i + USBHCH_FIRST_CHANNEL),
			  (unsigned long)stats->calls,
			  (unsigned long)(stats->calls / 10U),
			  (unsigned long)stats->bytes,
			  (unsigned long)average,
			  (unsigned int)stats->min_length,
			  (unsigned int)stats->max_length,
			  (unsigned long)stats->in_calls,
			  (unsigned long)stats->out_calls,
			  (unsigned long)stats->errors,
			  (unsigned long)stats->ep_type[0],
			  (unsigned long)stats->ep_type[1],
			  (unsigned long)stats->ep_type[2],
			  (unsigned long)stats->ep_type[3],
			  (unsigned long)stats->length_bins[0],
			  (unsigned long)stats->length_bins[1],
			  (unsigned long)stats->length_bins[2],
			  (unsigned long)stats->length_bins[3],
			  (unsigned long)stats->length_bins[4]);
	}
}

void usb_tx_lifetime_ncm_begin(const void *source, uint32_t length)
{
	(void)source;
	(void)length;
}

void usb_tx_lifetime_ncm_end(int result)
{
	(void)result;
}

void usb_tx_lifetime_source_release(void)
{
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

void usb_tx_lifetime_ncm_begin(const void *source, uint32_t length)
{
	(void)source;
	(void)length;
}

void usb_tx_lifetime_ncm_end(int result)
{
	(void)result;
}

void usb_tx_lifetime_source_release(void)
{
}

#endif /* CONFIG_USB_HCD_PROFILE */
