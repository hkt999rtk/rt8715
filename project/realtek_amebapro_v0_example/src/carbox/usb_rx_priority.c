#include "usb_rx_priority.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "diag.h"
#include "hal_timer.h"

#ifndef CONFIG_USB_CH4_QUEUE_FRONT
#define CONFIG_USB_CH4_QUEUE_FRONT 0
#endif

#ifndef CONFIG_USB_PROFILE_REPORT
#define CONFIG_USB_PROFILE_REPORT 0
#endif

#if CONFIG_USB_CH4_QUEUE_FRONT

/* Closed USB host ABI values used by the existing NCM HCD profiler. */
#define USB_RX_CHANNEL 4U
#define USB_EP_BULK 2U
#define USB_URB_DONE 1U
#define USB_URB_NOTREADY 2U
#define USB_RX_QUEUE_CAPACITY 16U
#define USB_CORE_QUEUE_OFFSET 0x7cU
#define USB_HCINT_BASE 0x400C0508U
#define USB_HCINT_STRIDE 0x20U
#define USB_HCINTMSK_OFFSET 0x04U
#define USB_HCTSIZ_OFFSET 0x08U
#define USB_HCTSIZ_XFERSIZE_MASK 0x7FFFFU
#define USB_HCINT_XFERCOMP (1U << 0)
#define USB_HCINT_CHHLTD (1U << 1)
#define USB_HCINT_NAK (1U << 4)
#define USB_HCINT_ACK (1U << 5)
#define USB_HCINT_NYET (1U << 6)
#define USB_HCINT_ERROR_MASK ((1U << 2) | (1U << 3) | (1U << 7) | \
			      (1U << 8) | (1U << 9) | (1U << 10) | \
			      (1U << 11) | (1U << 12) | (1U << 13))
#define USB_HCCHAR_OFFSET_FROM_HCINT ((uint32_t)-8)
#define USB_HCCHAR_CHENA (1U << 31)
#define USB_HCCHAR_CHDIS (1U << 30)
#define USB_NCM_NTH16_SIGNATURE 0x484D434EU
#define USB_NCM_NDP16_SIGNATURE_MASK 0x00FFFFFFU
#define USB_NCM_NDP16_SIGNATURE_BASE 0x004D434EU
#define USB_ETHERTYPE_IPV4 0x0800U
#define USB_ETHERTYPE_VLAN 0x8100U
#define USB_ETHERTYPE_IPV6 0x86DDU
#define USB_IPPROTO_TCP 6U
#define USB_TCP_FLAG_FIN 0x01U
#define USB_TCP_FLAG_SYN 0x02U
#define USB_TCP_FLAG_RST 0x04U
#define USB_TCP_FLAG_ACK 0x10U
#define USB_HID_PACKET_MAX_AGE_US 100000U

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
	uint32_t cause_xfercomp;
	uint32_t cause_chhltd;
	uint32_t cause_nak;
	uint32_t cause_ack;
	uint32_t cause_nyet;
	uint32_t cause_error;
	uint32_t cause_other;
	uint32_t progress_events;
	uint32_t no_progress_events;
	uint32_t remaining_resets;
	uint64_t progress_bytes;
	uint32_t event_gap_max_us;
	uint32_t slow_cause_xfercomp;
	uint32_t slow_cause_chhltd;
	uint32_t slow_cause_nak;
	uint32_t slow_cause_ack;
	uint32_t slow_cause_nyet;
	uint32_t slow_cause_error;
	uint32_t slow_cause_other;
	uint32_t slow_progress_events;
	uint32_t slow_no_progress_events;
	uint32_t slow_remaining_resets;
	uint32_t slow_progress_bytes;
	uint32_t slow_first_progress_us;
	uint32_t slow_last_progress_to_done_us;
	uint32_t slow_event_gap_max_us;
	uint32_t slow_out_submits;
	uint32_t slow_out_bytes;
	uint32_t slow_initial_remaining;
	uint32_t slow_final_remaining;
	uint32_t halt_calls;
	uint32_t nak_to_halt_samples;
	uint64_t nak_to_halt_total_us;
	uint32_t nak_to_halt_max_us;
	uint32_t retry_chena;
	uint32_t retry_not_enabled;
	uint32_t nak_to_retry_samples;
	uint64_t nak_to_retry_total_us;
	uint32_t nak_to_retry_max_us;
	uint32_t retry_to_event_samples;
	uint64_t retry_to_event_total_us;
	uint32_t retry_to_event_max_us;
	uint32_t done_short;
	uint32_t done_full;
	uint64_t done_actual_bytes;
	uint32_t done_actual_max;
	uint32_t slow_halt_calls;
	uint32_t slow_nak_to_halt_max_us;
	uint32_t slow_retry_chena;
	uint32_t slow_retry_not_enabled;
	uint32_t slow_nak_to_retry_max_us;
	uint32_t slow_retry_to_event_max_us;
	uint32_t slow_actual_bytes;
	uint32_t slow_short_packet;
	uint32_t nak_active;
	uint32_t nak_halting;
	uint32_t nak_inactive;
	uint32_t slow_nak_active;
	uint32_t slow_nak_halting;
	uint32_t slow_nak_inactive;
	uint32_t ntb_calls;
	uint32_t ntb_invalid;
	uint32_t ntb_length_mismatch;
	uint32_t ntb_datagrams_0;
	uint32_t ntb_datagrams_1;
	uint32_t ntb_datagrams_2;
	uint32_t ntb_datagrams_3;
	uint32_t ntb_datagrams_ge4;
	uint64_t ntb_datagram_bytes;
	uint32_t ntb_wait_samples;
	uint64_t ntb_wait_total_us;
	uint32_t ntb_wait_max_us;
	uint32_t ntb_wait_one_samples;
	uint64_t ntb_wait_one_total_us;
	uint32_t ntb_wait_one_max_us;
	uint32_t ntb_wait_multi_samples;
	uint64_t ntb_wait_multi_total_us;
	uint32_t ntb_wait_multi_max_us;
	uint32_t slow_ntb_request_id;
	uint32_t slow_ntb_wait_us;
	uint32_t slow_ntb_actual;
	uint32_t slow_ntb_block_length;
	uint32_t slow_ntb_datagrams;
	uint32_t slow_ntb_datagram_bytes;
	uint32_t flow_updates;
	uint32_t flow_tcp_packets;
	uint32_t flow_tcp_payload_packets;
	uint64_t flow_tcp_payload_bytes;
	uint32_t flow_wait_samples;
	uint64_t flow_wait_total_us;
	uint32_t flow_wait_max_us;
	uint32_t flow_wait_ge_5ms;
	uint32_t flow_wait_ge_10ms;
	uint32_t flow_wait_ge_20ms;
	uint32_t flow_wait_ge_100ms;
	uint32_t hid_marks;
	uint32_t hid_matched;
	uint32_t hid_no_flow;
	uint32_t hid_reused;
	uint32_t hid_stale;
	uint32_t hid_wait_samples;
	uint64_t hid_wait_total_us;
	uint32_t hid_wait_max_us;
	uint32_t hid_wait_ge_5ms;
	uint32_t hid_wait_ge_10ms;
	uint32_t hid_wait_ge_20ms;
	uint32_t hid_wait_ge_100ms;
	uint32_t hid_parser_age_samples;
	uint64_t hid_parser_age_total_us;
	uint32_t hid_parser_age_max_us;
	uint32_t hid_parser_age_ge_1ms;
	uint32_t hid_parser_age_ge_5ms;
	uint32_t hid_parser_age_ge_10ms;
	uint32_t hid_parser_age_ge_20ms;
	uint32_t slow_hid_request_id;
	uint32_t slow_hid_wait_us;
	uint32_t slow_hid_naks;
	uint32_t slow_hid_frame_bytes;
	uint32_t slow_hid_tcp_payload_bytes;
	uint32_t slow_hid_parser_age_us;
	uint32_t slow_hid_tcp_flags;
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
static uint32_t usb_rx_request_length;
static uint32_t usb_rx_last_remaining;
static uint32_t usb_rx_initial_remaining;
static uint32_t usb_rx_request_cause_xfercomp;
static uint32_t usb_rx_request_cause_chhltd;
static uint32_t usb_rx_request_cause_nak;
static uint32_t usb_rx_request_cause_ack;
static uint32_t usb_rx_request_cause_nyet;
static uint32_t usb_rx_request_cause_error;
static uint32_t usb_rx_request_cause_other;
static uint32_t usb_rx_request_progress_events;
static uint32_t usb_rx_request_no_progress_events;
static uint32_t usb_rx_request_remaining_resets;
static uint32_t usb_rx_request_progress_bytes;
static uint8_t usb_rx_request_progress_seen;
static uint32_t usb_rx_first_progress_time_us;
static uint32_t usb_rx_last_progress_time_us;
static uint32_t usb_rx_request_event_gap_max_us;
static uint32_t usb_rx_request_out_submits;
static uint32_t usb_rx_request_out_bytes;
static uint8_t usb_rx_nak_time_valid;
static uint32_t usb_rx_nak_time_us;
static uint8_t usb_rx_nak_chhltd_seen;
static uint8_t usb_rx_service_event_valid;
static uint32_t usb_rx_service_reasons;
static uint8_t usb_rx_retry_time_valid;
static uint32_t usb_rx_retry_time_us;
static uint32_t usb_rx_request_halt_calls;
static uint32_t usb_rx_request_nak_to_halt_max_us;
static uint32_t usb_rx_request_retry_chena;
static uint32_t usb_rx_request_retry_not_enabled;
static uint32_t usb_rx_request_nak_to_retry_max_us;
static uint32_t usb_rx_request_retry_to_event_max_us;
static uint32_t usb_rx_request_nak_active;
static uint32_t usb_rx_request_nak_halting;
static uint32_t usb_rx_request_nak_inactive;
static uint8_t usb_rx_last_done_valid;
static uint32_t usb_rx_last_done_id;
static uint32_t usb_rx_last_done_elapsed_us;
static uint32_t usb_rx_last_done_actual;
static uint32_t usb_rx_last_done_naks;
static uint8_t usb_rx_car_flow_valid;
static uint16_t usb_rx_car_local_port;
static uint16_t usb_rx_car_peer_port;
static uint32_t usb_rx_car_generation;
static uint32_t usb_rx_car_marked_generation;
static uint32_t usb_rx_car_arrival_us;
static uint32_t usb_rx_car_request_id;
static uint32_t usb_rx_car_wait_us;
static uint32_t usb_rx_car_naks;
static uint32_t usb_rx_car_frame_bytes;
static uint32_t usb_rx_car_tcp_payload_bytes;
static uint8_t usb_rx_car_tcp_flags;

extern void __real_ncm_proc_data(void *context, uint8_t *buffer,
				 uint32_t length);

static uint32_t usb_rx_hc_reg(uint32_t channel, uint32_t offset)
{
	return *(volatile uint32_t *)(uintptr_t)(USB_HCINT_BASE +
		channel * USB_HCINT_STRIDE + offset);
}

static uint16_t usb_rx_get_le16(const uint8_t *data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t usb_rx_get_le32(const uint8_t *data)
{
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint16_t usb_rx_get_be16(const uint8_t *data)
{
	return ((uint16_t)data[0] << 8) | (uint16_t)data[1];
}

typedef struct usb_rx_tcp_frame_s {
	uint16_t source_port;
	uint16_t destination_port;
	uint32_t payload_bytes;
	uint8_t flags;
} usb_rx_tcp_frame_t;

static int usb_rx_parse_tcp_frame(const uint8_t *frame, uint32_t length,
				  usb_rx_tcp_frame_t *tcp)
{
	uint32_t network = 14U;
	uint32_t transport;
	uint32_t network_end;
	uint32_t tcp_header;
	uint16_t ether_type;

	if (frame == NULL || tcp == NULL || length < network) {
		return 0;
	}
	ether_type = usb_rx_get_be16(frame + 12U);
	if (ether_type == USB_ETHERTYPE_VLAN) {
		if (length < 18U) {
			return 0;
		}
		ether_type = usb_rx_get_be16(frame + 16U);
		network = 18U;
	}
	if (ether_type == USB_ETHERTYPE_IPV4) {
		uint32_t ip_header;
		uint32_t total_length;

		if (length < network + 20U || (frame[network] >> 4) != 4U ||
		    frame[network + 9U] != USB_IPPROTO_TCP) {
			return 0;
		}
		ip_header = (uint32_t)(frame[network] & 0x0fU) * 4U;
		total_length = usb_rx_get_be16(frame + network + 2U);
		if (ip_header < 20U || total_length < ip_header ||
		    total_length > length - network) {
			return 0;
		}
		transport = network + ip_header;
		network_end = network + total_length;
	} else if (ether_type == USB_ETHERTYPE_IPV6) {
		uint32_t payload_length;

		/* The vehicle event flow currently has no IPv6 extension header. */
		if (length < network + 40U || (frame[network] >> 4) != 6U ||
		    frame[network + 6U] != USB_IPPROTO_TCP) {
			return 0;
		}
		payload_length = usb_rx_get_be16(frame + network + 4U);
		if (payload_length < 20U ||
		    payload_length > length - network - 40U) {
			return 0;
		}
		transport = network + 40U;
		network_end = transport + payload_length;
	} else {
		return 0;
	}
	if (network_end < transport + 20U) {
		return 0;
	}
	tcp_header = (uint32_t)(frame[transport + 12U] >> 4) * 4U;
	if (tcp_header < 20U || tcp_header > network_end - transport) {
		return 0;
	}
	tcp->source_port = usb_rx_get_be16(frame + transport);
	tcp->destination_port = usb_rx_get_be16(frame + transport + 2U);
	tcp->payload_bytes = network_end - transport - tcp_header;
	tcp->flags = frame[transport + 13U];
	return 1;
}

static void usb_rx_tail_count(uint32_t wait_us, uint32_t *ge_5ms,
			      uint32_t *ge_10ms, uint32_t *ge_20ms,
			      uint32_t *ge_100ms)
{
	if (wait_us >= 5000U) {
		(*ge_5ms)++;
	}
	if (wait_us >= 10000U) {
		(*ge_10ms)++;
	}
	if (wait_us >= 20000U) {
		(*ge_20ms)++;
	}
	if (wait_us >= 100000U) {
		(*ge_100ms)++;
	}
}

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
extern void __real_usbh_hal_hc_halt(uint8_t channel);
extern void __real_usb_hal_enable_interrupt(void);

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

	if (result == 0U && channel == 3U && direction == 0U &&
	    ep_type == USB_EP_BULK && usb_rx_request_pending != 0U) {
		usb_rx_request_out_submits++;
		usb_rx_request_out_bytes += length;
	}

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
	usb_rx_request_length = length;
	usb_rx_initial_remaining = usb_rx_hc_reg(
		USB_RX_CHANNEL, USB_HCTSIZ_OFFSET) & USB_HCTSIZ_XFERSIZE_MASK;
	usb_rx_last_remaining = usb_rx_initial_remaining;
	usb_rx_request_cause_xfercomp = 0U;
	usb_rx_request_cause_chhltd = 0U;
	usb_rx_request_cause_nak = 0U;
	usb_rx_request_cause_ack = 0U;
	usb_rx_request_cause_nyet = 0U;
	usb_rx_request_cause_error = 0U;
	usb_rx_request_cause_other = 0U;
	usb_rx_request_progress_events = 0U;
	usb_rx_request_no_progress_events = 0U;
	usb_rx_request_remaining_resets = 0U;
	usb_rx_request_progress_bytes = 0U;
	usb_rx_request_progress_seen = 0U;
	usb_rx_request_event_gap_max_us = 0U;
	usb_rx_request_out_submits = 0U;
	usb_rx_request_out_bytes = 0U;
	usb_rx_nak_time_valid = 0U;
	usb_rx_nak_chhltd_seen = 0U;
	usb_rx_service_event_valid = 0U;
	usb_rx_retry_time_valid = 0U;
	usb_rx_request_halt_calls = 0U;
	usb_rx_request_nak_to_halt_max_us = 0U;
	usb_rx_request_retry_chena = 0U;
	usb_rx_request_retry_not_enabled = 0U;
	usb_rx_request_nak_to_retry_max_us = 0U;
	usb_rx_request_retry_to_event_max_us = 0U;
	usb_rx_request_nak_active = 0U;
	usb_rx_request_nak_halting = 0U;
	usb_rx_request_nak_inactive = 0U;

	return result;
}

uint32_t __wrap_usbh_hal_hc_read_interrupt(void)
{
	uint32_t pending = __real_usbh_hal_hc_read_interrupt();

	/* Observe only; queue insertion below is the sole scheduling change. */
	if ((pending & (1UL << USB_RX_CHANNEL)) != 0U) {
		uint32_t now_us = hal_read_curtime_us();
		uint32_t reasons = usb_rx_hc_reg(USB_RX_CHANNEL, 0U) &
			usb_rx_hc_reg(USB_RX_CHANNEL, USB_HCINTMSK_OFFSET);
		uint32_t remaining = usb_rx_hc_reg(
			USB_RX_CHANNEL, USB_HCTSIZ_OFFSET) &
			USB_HCTSIZ_XFERSIZE_MASK;

		if (usb_rx_retry_time_valid != 0U) {
			uint32_t retry_us = now_us - usb_rx_retry_time_us;

			usb_rx_priority_live.retry_to_event_samples++;
			usb_rx_accumulate(retry_us,
				&usb_rx_priority_live.retry_to_event_total_us,
				&usb_rx_priority_live.retry_to_event_max_us);
			if (retry_us > usb_rx_request_retry_to_event_max_us) {
				usb_rx_request_retry_to_event_max_us = retry_us;
			}
			usb_rx_retry_time_valid = 0U;
		}

		usb_rx_priority_live.ch4_haint++;
		usb_haint_time_us = now_us;
		usb_haint_time_valid = 1U;
		if (usb_rx_request_pending != 0U) {
			uint32_t known = USB_HCINT_XFERCOMP | USB_HCINT_CHHLTD |
				USB_HCINT_NAK | USB_HCINT_ACK | USB_HCINT_NYET |
				USB_HCINT_ERROR_MASK;

			usb_rx_request_haint_count++;
			if (usb_rx_last_haint_valid != 0U) {
				uint32_t gap_us = now_us - usb_rx_last_haint_time_us;

				if (gap_us > usb_rx_request_event_gap_max_us) {
					usb_rx_request_event_gap_max_us = gap_us;
				}
				if (gap_us > usb_rx_priority_live.event_gap_max_us) {
					usb_rx_priority_live.event_gap_max_us = gap_us;
				}
			}
			usb_rx_last_haint_time_us = now_us;
			usb_rx_last_haint_valid = 1U;
			if ((reasons & USB_HCINT_XFERCOMP) != 0U) {
				usb_rx_request_cause_xfercomp++;
				usb_rx_priority_live.cause_xfercomp++;
			}
			if ((reasons & USB_HCINT_CHHLTD) != 0U) {
				usb_rx_request_cause_chhltd++;
				usb_rx_priority_live.cause_chhltd++;
			}
			if ((reasons & USB_HCINT_NAK) != 0U) {
				usb_rx_request_cause_nak++;
				usb_rx_priority_live.cause_nak++;
			}
			if ((reasons & USB_HCINT_ACK) != 0U) {
				usb_rx_request_cause_ack++;
				usb_rx_priority_live.cause_ack++;
			}
			if ((reasons & USB_HCINT_NYET) != 0U) {
				usb_rx_request_cause_nyet++;
				usb_rx_priority_live.cause_nyet++;
			}
			if ((reasons & USB_HCINT_ERROR_MASK) != 0U) {
				usb_rx_request_cause_error++;
				usb_rx_priority_live.cause_error++;
			}
			if ((reasons & ~known) != 0U || reasons == 0U) {
				usb_rx_request_cause_other++;
				usb_rx_priority_live.cause_other++;
			}
			if (remaining < usb_rx_last_remaining) {
				uint32_t bytes = usb_rx_last_remaining - remaining;

				usb_rx_request_progress_events++;
				usb_rx_request_progress_bytes += bytes;
				usb_rx_priority_live.progress_events++;
				usb_rx_priority_live.progress_bytes += bytes;
				if (usb_rx_request_progress_seen == 0U) {
					usb_rx_request_progress_seen = 1U;
					usb_rx_first_progress_time_us = now_us;
				}
				usb_rx_last_progress_time_us = now_us;
			} else if (remaining > usb_rx_last_remaining) {
				usb_rx_request_remaining_resets++;
				usb_rx_priority_live.remaining_resets++;
			} else {
				usb_rx_request_no_progress_events++;
				usb_rx_priority_live.no_progress_events++;
			}
			usb_rx_last_remaining = remaining;
			if ((reasons & USB_HCINT_NAK) != 0U) {
				usb_rx_nak_time_us = now_us;
				usb_rx_nak_time_valid = 1U;
				usb_rx_nak_chhltd_seen = 0U;
			}
		}
		usb_rx_service_reasons = reasons;
		usb_rx_service_event_valid = 1U;
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

void __wrap_usbh_hal_hc_halt(uint8_t channel)
{
	if (channel == USB_RX_CHANNEL && usb_rx_request_pending != 0U) {
		uint32_t now_us = hal_read_curtime_us();

		usb_rx_priority_live.halt_calls++;
		usb_rx_request_halt_calls++;
		if (usb_rx_nak_time_valid != 0U) {
			uint32_t elapsed_us = now_us - usb_rx_nak_time_us;

			usb_rx_priority_live.nak_to_halt_samples++;
			usb_rx_accumulate(elapsed_us,
				&usb_rx_priority_live.nak_to_halt_total_us,
				&usb_rx_priority_live.nak_to_halt_max_us);
			if (elapsed_us > usb_rx_request_nak_to_halt_max_us) {
				usb_rx_request_nak_to_halt_max_us = elapsed_us;
			}
		}
	}
	__real_usbh_hal_hc_halt(channel);
}

void __wrap_usb_hal_enable_interrupt(void)
{
	/* The HCD task calls this after it has serviced the current HC event. */
	if (usb_rx_request_pending != 0U && usb_rx_nak_time_valid != 0U) {
		uint32_t now_us = hal_read_curtime_us();
		uint32_t hcchar = usb_rx_hc_reg(
			USB_RX_CHANNEL, USB_HCCHAR_OFFSET_FROM_HCINT);

		if (usb_rx_service_event_valid != 0U &&
		    (usb_rx_service_reasons & USB_HCINT_NAK) != 0U) {
			if ((hcchar & USB_HCCHAR_CHENA) == 0U) {
				usb_rx_priority_live.nak_inactive++;
				usb_rx_request_nak_inactive++;
			} else if ((hcchar & USB_HCCHAR_CHDIS) != 0U) {
				usb_rx_priority_live.nak_halting++;
				usb_rx_request_nak_halting++;
			} else {
				usb_rx_priority_live.nak_active++;
				usb_rx_request_nak_active++;
			}
		}
		if (usb_rx_service_event_valid != 0U &&
		    (usb_rx_service_reasons & USB_HCINT_CHHLTD) != 0U) {
			usb_rx_nak_chhltd_seen = 1U;
		}
		if (usb_rx_nak_chhltd_seen != 0U &&
		    (hcchar & USB_HCCHAR_CHENA) != 0U) {
			uint32_t elapsed_us = now_us - usb_rx_nak_time_us;

			usb_rx_priority_live.retry_chena++;
			usb_rx_request_retry_chena++;
			usb_rx_priority_live.nak_to_retry_samples++;
			usb_rx_accumulate(elapsed_us,
				&usb_rx_priority_live.nak_to_retry_total_us,
				&usb_rx_priority_live.nak_to_retry_max_us);
			if (elapsed_us > usb_rx_request_nak_to_retry_max_us) {
				usb_rx_request_nak_to_retry_max_us = elapsed_us;
			}
			usb_rx_retry_time_us = now_us;
			usb_rx_retry_time_valid = 1U;
			usb_rx_nak_time_valid = 0U;
			usb_rx_nak_chhltd_seen = 0U;
		} else if (usb_rx_service_event_valid != 0U &&
			   (usb_rx_service_reasons & USB_HCINT_CHHLTD) != 0U) {
			usb_rx_priority_live.retry_not_enabled++;
			usb_rx_request_retry_not_enabled++;
		}
	}
	usb_rx_service_event_valid = 0U;
	__real_usb_hal_enable_interrupt();
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
	uint32_t final_remaining;
	uint32_t actual_bytes;

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
	final_remaining = usb_rx_hc_reg(
		USB_RX_CHANNEL, USB_HCTSIZ_OFFSET) & USB_HCTSIZ_XFERSIZE_MASK;
	actual_bytes = final_remaining <= usb_rx_request_length ?
		usb_rx_request_length - final_remaining : 0U;
	usb_rx_priority_live.done_actual_bytes += actual_bytes;
	if (actual_bytes > usb_rx_priority_live.done_actual_max) {
		usb_rx_priority_live.done_actual_max = actual_bytes;
	}
	if (actual_bytes < usb_rx_request_length) {
		usb_rx_priority_live.done_short++;
	} else {
		usb_rx_priority_live.done_full++;
	}
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
	usb_rx_last_done_valid = 1U;
	usb_rx_last_done_id = usb_rx_request_id;
	usb_rx_last_done_elapsed_us = elapsed_us;
	usb_rx_last_done_actual = actual_bytes;
	usb_rx_last_done_naks = usb_rx_request_cause_nak;
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
		usb_rx_priority_live.slow_cause_xfercomp =
			usb_rx_request_cause_xfercomp;
		usb_rx_priority_live.slow_cause_chhltd =
			usb_rx_request_cause_chhltd;
		usb_rx_priority_live.slow_cause_nak = usb_rx_request_cause_nak;
		usb_rx_priority_live.slow_cause_ack = usb_rx_request_cause_ack;
		usb_rx_priority_live.slow_cause_nyet = usb_rx_request_cause_nyet;
		usb_rx_priority_live.slow_cause_error =
			usb_rx_request_cause_error;
		usb_rx_priority_live.slow_cause_other =
			usb_rx_request_cause_other;
		usb_rx_priority_live.slow_progress_events =
			usb_rx_request_progress_events;
		usb_rx_priority_live.slow_no_progress_events =
			usb_rx_request_no_progress_events;
		usb_rx_priority_live.slow_remaining_resets =
			usb_rx_request_remaining_resets;
		usb_rx_priority_live.slow_progress_bytes =
			usb_rx_request_progress_bytes;
		usb_rx_priority_live.slow_first_progress_us =
			usb_rx_request_progress_seen != 0U ?
			usb_rx_first_progress_time_us - usb_rx_submit_time_us : 0U;
		usb_rx_priority_live.slow_last_progress_to_done_us =
			usb_rx_request_progress_seen != 0U ?
			now_us - usb_rx_last_progress_time_us : 0U;
		usb_rx_priority_live.slow_event_gap_max_us =
			usb_rx_request_event_gap_max_us;
		usb_rx_priority_live.slow_out_submits =
			usb_rx_request_out_submits;
		usb_rx_priority_live.slow_out_bytes = usb_rx_request_out_bytes;
		usb_rx_priority_live.slow_initial_remaining =
			usb_rx_initial_remaining != 0U ?
			usb_rx_initial_remaining : usb_rx_request_length;
		usb_rx_priority_live.slow_final_remaining = final_remaining;
		usb_rx_priority_live.slow_halt_calls = usb_rx_request_halt_calls;
		usb_rx_priority_live.slow_nak_to_halt_max_us =
			usb_rx_request_nak_to_halt_max_us;
		usb_rx_priority_live.slow_retry_chena =
			usb_rx_request_retry_chena;
		usb_rx_priority_live.slow_retry_not_enabled =
			usb_rx_request_retry_not_enabled;
		usb_rx_priority_live.slow_nak_to_retry_max_us =
			usb_rx_request_nak_to_retry_max_us;
		usb_rx_priority_live.slow_retry_to_event_max_us =
			usb_rx_request_retry_to_event_max_us;
		usb_rx_priority_live.slow_actual_bytes = actual_bytes;
		usb_rx_priority_live.slow_short_packet =
			actual_bytes < usb_rx_request_length ? 1U : 0U;
		usb_rx_priority_live.slow_nak_active = usb_rx_request_nak_active;
		usb_rx_priority_live.slow_nak_halting =
			usb_rx_request_nak_halting;
		usb_rx_priority_live.slow_nak_inactive =
			usb_rx_request_nak_inactive;
	}
	usb_rx_request_pending = 0U;
	usb_rx_done_time_us = now_us;
	usb_rx_done_time_valid = 1U;
	return state;
}

void __wrap_ncm_proc_data(void *context, uint8_t *buffer, uint32_t length)
{
	uint32_t block_length = 0U;
	uint32_t datagrams = 0U;
	uint32_t datagram_bytes = 0U;
	uint32_t ndp_index;
	uint32_t chain_limit = 4U;
	uint16_t car_local_port;
	uint16_t car_peer_port;
	uint8_t car_flow_valid;
	uint8_t valid = 1U;

	taskENTER_CRITICAL();
	car_flow_valid = usb_rx_car_flow_valid;
	car_local_port = usb_rx_car_local_port;
	car_peer_port = usb_rx_car_peer_port;
	taskEXIT_CRITICAL();

	usb_rx_priority_live.ntb_calls++;
	if (buffer == NULL || length < 12U ||
	    usb_rx_get_le32(buffer) != USB_NCM_NTH16_SIGNATURE) {
		valid = 0U;
		ndp_index = 0U;
	} else {
		block_length = usb_rx_get_le16(buffer + 8U);
		ndp_index = usb_rx_get_le16(buffer + 10U);
		if (block_length != length) {
			usb_rx_priority_live.ntb_length_mismatch++;
		}
	}

	while (valid != 0U && ndp_index != 0U && chain_limit != 0U) {
		uint32_t ndp_length;
		uint32_t next_ndp;
		uint32_t entry;

		chain_limit--;

		if (ndp_index > length - 8U ||
		    (usb_rx_get_le32(buffer + ndp_index) &
		     USB_NCM_NDP16_SIGNATURE_MASK) !=
		    USB_NCM_NDP16_SIGNATURE_BASE) {
			valid = 0U;
			break;
		}
		ndp_length = usb_rx_get_le16(buffer + ndp_index + 4U);
		next_ndp = usb_rx_get_le16(buffer + ndp_index + 6U);
		if (ndp_length < 12U || ndp_length > length - ndp_index) {
			valid = 0U;
			break;
		}
		for (entry = ndp_index + 8U;
		     entry + 4U <= ndp_index + ndp_length; entry += 4U) {
			uint32_t index = usb_rx_get_le16(buffer + entry);
			uint32_t size = usb_rx_get_le16(buffer + entry + 2U);
			usb_rx_tcp_frame_t tcp;

			if (index == 0U || size == 0U) {
				break;
			}
			if (index > length || size > length - index) {
				valid = 0U;
				break;
			}
			datagrams++;
			datagram_bytes += size;
			if (car_flow_valid != 0U &&
			    usb_rx_parse_tcp_frame(buffer + index, size, &tcp) &&
			    tcp.source_port == car_peer_port &&
			    tcp.destination_port == car_local_port) {
				usb_rx_priority_live.flow_tcp_packets++;
				if (tcp.payload_bytes != 0U) {
					uint32_t now_us = hal_read_curtime_us();

					usb_rx_priority_live.flow_tcp_payload_packets++;
					usb_rx_priority_live.flow_tcp_payload_bytes +=
						tcp.payload_bytes;
					if (usb_rx_last_done_valid != 0U &&
					    usb_rx_last_done_actual == length) {
						usb_rx_priority_live.flow_wait_samples++;
						usb_rx_accumulate(
							usb_rx_last_done_elapsed_us,
							&usb_rx_priority_live.flow_wait_total_us,
							&usb_rx_priority_live.flow_wait_max_us);
						usb_rx_tail_count(
							usb_rx_last_done_elapsed_us,
							&usb_rx_priority_live.flow_wait_ge_5ms,
							&usb_rx_priority_live.flow_wait_ge_10ms,
							&usb_rx_priority_live.flow_wait_ge_20ms,
							&usb_rx_priority_live.flow_wait_ge_100ms);
					}
					taskENTER_CRITICAL();
					usb_rx_car_generation++;
					usb_rx_car_arrival_us = now_us;
					usb_rx_car_request_id = usb_rx_last_done_id;
					usb_rx_car_wait_us = usb_rx_last_done_elapsed_us;
					usb_rx_car_naks = usb_rx_last_done_naks;
					usb_rx_car_frame_bytes = size;
					usb_rx_car_tcp_payload_bytes = tcp.payload_bytes;
					usb_rx_car_tcp_flags = tcp.flags;
					taskEXIT_CRITICAL();
				}
			}
		}
		ndp_index = next_ndp;
	}
	if (ndp_index != 0U && chain_limit == 0U) {
		valid = 0U;
	}

	if (valid == 0U) {
		usb_rx_priority_live.ntb_invalid++;
	} else {
		usb_rx_priority_live.ntb_datagram_bytes += datagram_bytes;
		if (datagrams == 0U) {
			usb_rx_priority_live.ntb_datagrams_0++;
		} else if (datagrams == 1U) {
			usb_rx_priority_live.ntb_datagrams_1++;
		} else if (datagrams == 2U) {
			usb_rx_priority_live.ntb_datagrams_2++;
		} else if (datagrams == 3U) {
			usb_rx_priority_live.ntb_datagrams_3++;
		} else {
			usb_rx_priority_live.ntb_datagrams_ge4++;
		}
	}

	if (usb_rx_last_done_valid != 0U &&
	    usb_rx_last_done_actual == length) {
		usb_rx_priority_live.ntb_wait_samples++;
		usb_rx_accumulate(usb_rx_last_done_elapsed_us,
			&usb_rx_priority_live.ntb_wait_total_us,
			&usb_rx_priority_live.ntb_wait_max_us);
		if (valid != 0U && datagrams == 1U) {
			usb_rx_priority_live.ntb_wait_one_samples++;
			usb_rx_accumulate(usb_rx_last_done_elapsed_us,
				&usb_rx_priority_live.ntb_wait_one_total_us,
				&usb_rx_priority_live.ntb_wait_one_max_us);
		} else if (valid != 0U && datagrams > 1U) {
			usb_rx_priority_live.ntb_wait_multi_samples++;
			usb_rx_accumulate(usb_rx_last_done_elapsed_us,
				&usb_rx_priority_live.ntb_wait_multi_total_us,
				&usb_rx_priority_live.ntb_wait_multi_max_us);
		}
		if (usb_rx_last_done_elapsed_us >
		    usb_rx_priority_live.slow_ntb_wait_us) {
			usb_rx_priority_live.slow_ntb_request_id =
				usb_rx_last_done_id;
			usb_rx_priority_live.slow_ntb_wait_us =
				usb_rx_last_done_elapsed_us;
			usb_rx_priority_live.slow_ntb_actual = length;
			usb_rx_priority_live.slow_ntb_block_length = block_length;
			usb_rx_priority_live.slow_ntb_datagrams = datagrams;
			usb_rx_priority_live.slow_ntb_datagram_bytes =
				datagram_bytes;
		}
	}
	usb_rx_last_done_valid = 0U;

	__real_ncm_proc_data(context, buffer, length);
}

void carbox_usb_rx_priority_set_car_flow(uint16_t local_port,
					 uint16_t peer_port)
{
	if (local_port == 0U || peer_port == 0U) {
		return;
	}
	taskENTER_CRITICAL();
	if (usb_rx_car_flow_valid == 0U ||
	    usb_rx_car_local_port != local_port ||
	    usb_rx_car_peer_port != peer_port) {
		usb_rx_car_local_port = local_port;
		usb_rx_car_peer_port = peer_port;
		usb_rx_car_flow_valid = 1U;
		usb_rx_car_marked_generation = usb_rx_car_generation;
		usb_rx_priority_live.flow_updates++;
	}
	taskEXIT_CRITICAL();
}

void carbox_usb_rx_priority_mark_hid(uint32_t parser_time_us)
{
	uint32_t age_us;

	taskENTER_CRITICAL();
	usb_rx_priority_live.hid_marks++;
	if (usb_rx_car_flow_valid == 0U || usb_rx_car_generation == 0U) {
		usb_rx_priority_live.hid_no_flow++;
		taskEXIT_CRITICAL();
		return;
	}
	if (usb_rx_car_marked_generation == usb_rx_car_generation) {
		usb_rx_priority_live.hid_reused++;
		taskEXIT_CRITICAL();
		return;
	}
	age_us = parser_time_us - usb_rx_car_arrival_us;
	if (age_us > USB_HID_PACKET_MAX_AGE_US) {
		usb_rx_priority_live.hid_stale++;
		taskEXIT_CRITICAL();
		return;
	}
	usb_rx_car_marked_generation = usb_rx_car_generation;
	usb_rx_priority_live.hid_matched++;
	usb_rx_priority_live.hid_wait_samples++;
	usb_rx_accumulate(usb_rx_car_wait_us,
		&usb_rx_priority_live.hid_wait_total_us,
		&usb_rx_priority_live.hid_wait_max_us);
	usb_rx_tail_count(usb_rx_car_wait_us,
		&usb_rx_priority_live.hid_wait_ge_5ms,
		&usb_rx_priority_live.hid_wait_ge_10ms,
		&usb_rx_priority_live.hid_wait_ge_20ms,
		&usb_rx_priority_live.hid_wait_ge_100ms);
	usb_rx_priority_live.hid_parser_age_samples++;
	usb_rx_accumulate(age_us,
		&usb_rx_priority_live.hid_parser_age_total_us,
		&usb_rx_priority_live.hid_parser_age_max_us);
	if (age_us >= 1000U) {
		usb_rx_priority_live.hid_parser_age_ge_1ms++;
	}
	if (age_us >= 5000U) {
		usb_rx_priority_live.hid_parser_age_ge_5ms++;
	}
	if (age_us >= 10000U) {
		usb_rx_priority_live.hid_parser_age_ge_10ms++;
	}
	if (age_us >= 20000U) {
		usb_rx_priority_live.hid_parser_age_ge_20ms++;
	}
	if (usb_rx_car_wait_us > usb_rx_priority_live.slow_hid_wait_us) {
		usb_rx_priority_live.slow_hid_request_id = usb_rx_car_request_id;
		usb_rx_priority_live.slow_hid_wait_us = usb_rx_car_wait_us;
		usb_rx_priority_live.slow_hid_naks = usb_rx_car_naks;
		usb_rx_priority_live.slow_hid_frame_bytes =
			usb_rx_car_frame_bytes;
		usb_rx_priority_live.slow_hid_tcp_payload_bytes =
			usb_rx_car_tcp_payload_bytes;
		usb_rx_priority_live.slow_hid_parser_age_us = age_us;
		usb_rx_priority_live.slow_hid_tcp_flags = usb_rx_car_tcp_flags;
	}
	taskEXIT_CRITICAL();
}

void carbox_usb_rx_priority_report(uint32_t sequence)
{
#if CONFIG_USB_PROFILE_REPORT
	usb_rx_priority_stats_t stats;
	uint32_t pending;
	uint16_t car_local_port;
	uint16_t car_peer_port;
	uint8_t car_flow_valid;

	taskENTER_CRITICAL();
	stats = usb_rx_priority_live;
	memset(&usb_rx_priority_live, 0, sizeof(usb_rx_priority_live));
	pending = usb_ch4_pending;
	car_flow_valid = usb_rx_car_flow_valid;
	car_local_port = usb_rx_car_local_port;
	car_peer_port = usb_rx_car_peer_port;
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
	rt_printf("[USBRXCAUSE][%lu] event xfer/halt/nak/ack/nyet/error/other="
		  "%lu/%lu/%lu/%lu/%lu/%lu/%lu progress/no_progress/reset="
		  "%lu/%lu/%lu bytes=%llu event_gap_max_us=%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.cause_xfercomp,
		  (unsigned long)stats.cause_chhltd,
		  (unsigned long)stats.cause_nak,
		  (unsigned long)stats.cause_ack,
		  (unsigned long)stats.cause_nyet,
		  (unsigned long)stats.cause_error,
		  (unsigned long)stats.cause_other,
		  (unsigned long)stats.progress_events,
		  (unsigned long)stats.no_progress_events,
		  (unsigned long)stats.remaining_resets,
		  (unsigned long long)stats.progress_bytes,
		  (unsigned long)stats.event_gap_max_us);
	rt_printf("[USBRXCAUSE][%lu][SLOW] id=%lu reason xfer/halt/nak/ack/nyet/"
		  "error/other=%lu/%lu/%lu/%lu/%lu/%lu/%lu progress/no/reset/bytes="
		  "%lu/%lu/%lu/%lu first_us/last_to_done_us=%lu/%lu "
		  "event_gap_max_us=%lu out submit/bytes=%lu/%lu buffer/actual/short="
		  "%lu/%lu/%lu rem_final=%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.slow_request_id,
		  (unsigned long)stats.slow_cause_xfercomp,
		  (unsigned long)stats.slow_cause_chhltd,
		  (unsigned long)stats.slow_cause_nak,
		  (unsigned long)stats.slow_cause_ack,
		  (unsigned long)stats.slow_cause_nyet,
		  (unsigned long)stats.slow_cause_error,
		  (unsigned long)stats.slow_cause_other,
		  (unsigned long)stats.slow_progress_events,
		  (unsigned long)stats.slow_no_progress_events,
		  (unsigned long)stats.slow_remaining_resets,
		  (unsigned long)stats.slow_progress_bytes,
		  (unsigned long)stats.slow_first_progress_us,
		  (unsigned long)stats.slow_last_progress_to_done_us,
		  (unsigned long)stats.slow_event_gap_max_us,
		  (unsigned long)stats.slow_out_submits,
		  (unsigned long)stats.slow_out_bytes,
		  (unsigned long)stats.slow_initial_remaining,
		  (unsigned long)stats.slow_actual_bytes,
		  (unsigned long)stats.slow_short_packet,
		  (unsigned long)stats.slow_final_remaining);
	rt_printf("[USBRXRETRY][%lu] halt=%lu nak_to_halt n/avg/max=%lu/%lu/%lu "
		  "retry chena/not_enabled=%lu/%lu nak_to_retry n/avg/max="
		  "%lu/%lu/%lu retry_to_event n/avg/max=%lu/%lu/%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.halt_calls,
		  (unsigned long)stats.nak_to_halt_samples,
		  stats.nak_to_halt_samples != 0U ?
			(unsigned long)(stats.nak_to_halt_total_us /
				stats.nak_to_halt_samples) : 0UL,
		  (unsigned long)stats.nak_to_halt_max_us,
		  (unsigned long)stats.retry_chena,
		  (unsigned long)stats.retry_not_enabled,
		  (unsigned long)stats.nak_to_retry_samples,
		  stats.nak_to_retry_samples != 0U ?
			(unsigned long)(stats.nak_to_retry_total_us /
				stats.nak_to_retry_samples) : 0UL,
		  (unsigned long)stats.nak_to_retry_max_us,
		  (unsigned long)stats.retry_to_event_samples,
		  stats.retry_to_event_samples != 0U ?
			(unsigned long)(stats.retry_to_event_total_us /
				stats.retry_to_event_samples) : 0UL,
		  (unsigned long)stats.retry_to_event_max_us);
	rt_printf("[USBRXRETRY][%lu] completion short/full=%lu/%lu "
		  "actual_bytes total/avg/max=%llu/%lu/%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.done_short,
		  (unsigned long)stats.done_full,
		  (unsigned long long)stats.done_actual_bytes,
		  stats.rx_done != 0U ?
			(unsigned long)(stats.done_actual_bytes / stats.rx_done) : 0UL,
		  (unsigned long)stats.done_actual_max);
	rt_printf("[USBRXRETRY][%lu] nak_service active/halting/inactive="
		  "%lu/%lu/%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.nak_active,
		  (unsigned long)stats.nak_halting,
		  (unsigned long)stats.nak_inactive);
	rt_printf("[USBRXRETRY][%lu][SLOW] id=%lu halt=%lu "
		  "nak_to_halt_max_us=%lu retry chena/not_enabled=%lu/%lu "
		  "nak_to_retry_max_us=%lu retry_to_event_max_us=%lu "
		  "nak_service active/halting/inactive=%lu/%lu/%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.slow_request_id,
		  (unsigned long)stats.slow_halt_calls,
		  (unsigned long)stats.slow_nak_to_halt_max_us,
		  (unsigned long)stats.slow_retry_chena,
		  (unsigned long)stats.slow_retry_not_enabled,
		  (unsigned long)stats.slow_nak_to_retry_max_us,
		  (unsigned long)stats.slow_retry_to_event_max_us,
		  (unsigned long)stats.slow_nak_active,
		  (unsigned long)stats.slow_nak_halting,
		  (unsigned long)stats.slow_nak_inactive);
	rt_printf("[USBRXNTB][%lu] calls/invalid/len_mismatch=%lu/%lu/%lu "
		  "datagrams 0/1/2/3/>=4=%lu/%lu/%lu/%lu/%lu "
		  "payload_bytes=%llu wait_us n/avg/max=%lu/%lu/%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.ntb_calls,
		  (unsigned long)stats.ntb_invalid,
		  (unsigned long)stats.ntb_length_mismatch,
		  (unsigned long)stats.ntb_datagrams_0,
		  (unsigned long)stats.ntb_datagrams_1,
		  (unsigned long)stats.ntb_datagrams_2,
		  (unsigned long)stats.ntb_datagrams_3,
		  (unsigned long)stats.ntb_datagrams_ge4,
		  (unsigned long long)stats.ntb_datagram_bytes,
		  (unsigned long)stats.ntb_wait_samples,
		  stats.ntb_wait_samples != 0U ?
			(unsigned long)(stats.ntb_wait_total_us /
				stats.ntb_wait_samples) : 0UL,
		  (unsigned long)stats.ntb_wait_max_us);
	rt_printf("[USBRXNTB][%lu][SLOW] id=%lu wait_us=%lu "
		  "actual/block=%lu/%lu datagrams/payload_bytes=%lu/%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.slow_ntb_request_id,
		  (unsigned long)stats.slow_ntb_wait_us,
		  (unsigned long)stats.slow_ntb_actual,
		  (unsigned long)stats.slow_ntb_block_length,
		  (unsigned long)stats.slow_ntb_datagrams,
		  (unsigned long)stats.slow_ntb_datagram_bytes);
	rt_printf("[USBRXNTB][%lu] wait_us one n/avg/max=%lu/%lu/%lu "
		  "multi n/avg/max=%lu/%lu/%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.ntb_wait_one_samples,
		  stats.ntb_wait_one_samples != 0U ?
			(unsigned long)(stats.ntb_wait_one_total_us /
				stats.ntb_wait_one_samples) : 0UL,
		  (unsigned long)stats.ntb_wait_one_max_us,
		  (unsigned long)stats.ntb_wait_multi_samples,
		  stats.ntb_wait_multi_samples != 0U ?
			(unsigned long)(stats.ntb_wait_multi_total_us /
				stats.ntb_wait_multi_samples) : 0UL,
		  (unsigned long)stats.ntb_wait_multi_max_us);
	rt_printf("[USBRXFLOW][%lu] valid=%u local/peer_port=%u/%u updates=%lu "
		  "tcp/payload_packets/payload_bytes=%lu/%lu/%llu "
		  "wait_us n/avg/max=%lu/%lu/%lu tail >=5/10/20/100ms="
		  "%lu/%lu/%lu/%lu\n",
		  (unsigned long)sequence,
		  (unsigned)car_flow_valid,
		  (unsigned)car_local_port,
		  (unsigned)car_peer_port,
		  (unsigned long)stats.flow_updates,
		  (unsigned long)stats.flow_tcp_packets,
		  (unsigned long)stats.flow_tcp_payload_packets,
		  (unsigned long long)stats.flow_tcp_payload_bytes,
		  (unsigned long)stats.flow_wait_samples,
		  stats.flow_wait_samples != 0U ?
			(unsigned long)(stats.flow_wait_total_us /
				stats.flow_wait_samples) : 0UL,
		  (unsigned long)stats.flow_wait_max_us,
		  (unsigned long)stats.flow_wait_ge_5ms,
		  (unsigned long)stats.flow_wait_ge_10ms,
		  (unsigned long)stats.flow_wait_ge_20ms,
		  (unsigned long)stats.flow_wait_ge_100ms);
	rt_printf("[USBRXHID][%lu] marks/matched/no_flow/reused/stale="
		  "%lu/%lu/%lu/%lu/%lu wait_us n/avg/max=%lu/%lu/%lu "
		  "tail >=5/10/20/100ms=%lu/%lu/%lu/%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.hid_marks,
		  (unsigned long)stats.hid_matched,
		  (unsigned long)stats.hid_no_flow,
		  (unsigned long)stats.hid_reused,
		  (unsigned long)stats.hid_stale,
		  (unsigned long)stats.hid_wait_samples,
		  stats.hid_wait_samples != 0U ?
			(unsigned long)(stats.hid_wait_total_us /
				stats.hid_wait_samples) : 0UL,
		  (unsigned long)stats.hid_wait_max_us,
		  (unsigned long)stats.hid_wait_ge_5ms,
		  (unsigned long)stats.hid_wait_ge_10ms,
		  (unsigned long)stats.hid_wait_ge_20ms,
		  (unsigned long)stats.hid_wait_ge_100ms);
	rt_printf("[USBRXHID][%lu][SLOW] id=%lu wait_us=%lu naks=%lu "
		  "frame/tcp_payload=%lu/%lu parser_age_us=%lu flags=0x%02lx\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.slow_hid_request_id,
		  (unsigned long)stats.slow_hid_wait_us,
		  (unsigned long)stats.slow_hid_naks,
		  (unsigned long)stats.slow_hid_frame_bytes,
		  (unsigned long)stats.slow_hid_tcp_payload_bytes,
		  (unsigned long)stats.slow_hid_parser_age_us,
		  (unsigned long)stats.slow_hid_tcp_flags);
	rt_printf("[USBRXHID][%lu] usb_to_parser_us n/avg/max=%lu/%lu/%lu "
		  "tail >=1/5/10/20ms=%lu/%lu/%lu/%lu\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.hid_parser_age_samples,
		  stats.hid_parser_age_samples != 0U ?
			(unsigned long)(stats.hid_parser_age_total_us /
				stats.hid_parser_age_samples) : 0UL,
		  (unsigned long)stats.hid_parser_age_max_us,
		  (unsigned long)stats.hid_parser_age_ge_1ms,
		  (unsigned long)stats.hid_parser_age_ge_5ms,
		  (unsigned long)stats.hid_parser_age_ge_10ms,
		  (unsigned long)stats.hid_parser_age_ge_20ms);
#else
	(void)sequence;
#endif
}

#else

void carbox_usb_rx_priority_report(uint32_t sequence)
{
	(void)sequence;
}

void carbox_usb_rx_priority_set_car_flow(uint16_t local_port,
					 uint16_t peer_port)
{
	(void)local_port;
	(void)peer_port;
}

void carbox_usb_rx_priority_mark_hid(uint32_t parser_time_us)
{
	(void)parser_time_us;
}

#endif
