#include "car_ack_timestamp.h"

#if CONFIG_CAR_ACK_TIMESTAMP
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "FreeRTOS_POSIX/time.h"
#include "diag.h"
#include "hal_timer.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"

/* This probe follows the current single-frame NCM pipeline. It never waits
 * for a trace buffer and never prints on a transport/USB task. */
#if !CONFIG_NCM_TX_ASYNC || !CONFIG_NCM_TX_PIPELINE || CONFIG_NCM_TX_BATCH_MAX != 1
#error "CAR_ACK_TIMESTAMP requires the single-frame asynchronous NCM pipeline"
#endif
#if !LWIP_TCPIP_CORE_LOCKING
#error "CAR_ACK_TIMESTAMP requires TCP writes in the response task's context"
#endif
#define ACKTS_SLOTS 64U
#define ACKTS_EXPIRE_MS 2000
#define ACKTS_BSS __attribute__((section(".lpddr.bss.car_ack_timestamp")))

typedef struct {
	uint32_t sec, ms, us;
} ackts_time_t;

typedef struct {
	uint32_t id, first_seq, end_seq;
	uint16_t local_port, peer_port;
	uint8_t local_ip[16], peer_ip[16];
	uint8_t ip_bytes, have_seq, ended, reported;
	uint32_t packets;
	int status;
	ackts_time_t enter, write_return;
	ackts_time_t http_done, hid_begin, hid_end;
	uint8_t have_http, hid_finished, app_reported;
} ackts_response_t;

typedef struct {
	uint32_t cookie, response, seq, bytes, submits;
	ackts_time_t enter, ncm, hw_begin, hw_end, done;
	int status, hw_status;
	uint8_t complete, have_hw;
	uint32_t input_depth, ready_depth, free_slots, busy, core_events, core_wait_max_us;
	ackts_time_t notify, dispatch;
	uint8_t have_queue, have_notify, have_dispatch;
} ackts_packet_t;

typedef struct {
	uint32_t seq, ack, bytes, generation;
	ackts_time_t ingress, done, dispatch;
	uint8_t valid_usb, occupied;
} ackts_rx_t;

typedef struct {
	uint32_t channel, packet, generation;
	ackts_time_t time;
	uint8_t rx_done;
} ackts_queue_event_t;

typedef struct {
	uint32_t channel, state, action, elapsed, base, count;
	uint32_t packet;
	ackts_time_t time;
	uint8_t occupied;
} ackts_recovery_t;

static ackts_rx_t rx_records[ACKTS_SLOTS] ACKTS_BSS;
static ackts_recovery_t recoveries[16] ACKTS_BSS;
static ackts_queue_event_t core_events[32];
static void *core_queue;
static TaskHandle_t core_task, http_task;
static uint32_t core_head, core_count, rx_generation, rx_notified, rx_context_generation;
static uint32_t rx_next, rx_drop, recovery_drop, core_desync, core_send_fail;
static uint8_t core_synced, have_http_done, rx_context_valid;
static volatile uint8_t *rx_hc;
static ackts_time_t http_done, rx_done_time, rx_dispatch_time;
static uint32_t tx_channel = 3U;
static uint32_t last_urb_channel, last_urb_state, last_elapsed, last_elapsed_base;
static uint8_t last_urb_valid, last_elapsed_valid;

static void ackts_report_extra(ackts_time_t now);

static ackts_response_t responses[ACKTS_SLOTS] ACKTS_BSS;
static ackts_packet_t packets[ACKTS_SLOTS] ACKTS_BSS;
static TaskHandle_t response_task, report_task;
static uint32_t next_response, active_response, next_packet;
static uint32_t active_usb_packet;
static const void *active_usb_buffer;
static uint16_t flow_local_port, flow_peer_port;
static uint32_t dropped, ambiguous, response_overrun;

static ackts_time_t ackts_now(void)
{
	struct timespec ts = {0, 0};
	ackts_time_t t;
	/* SDK CLOCK_MONOTONIC has 1 ms resolution. Keep the independent HAL
	 * microsecond clock too; do not pretend tv_nsec has nanosecond precision. */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	t.sec = (uint32_t)ts.tv_sec;
	t.ms = (uint32_t)ts.tv_nsec / 1000000U;
	t.us = hal_read_curtime_us();
	return t;
}

static int32_t ackts_age_ms(ackts_time_t now, ackts_time_t then)
{
	/* The reporter's snapshot can precede a concurrently created record. */
	return (int32_t)((now.sec - then.sec) * 1000U + now.ms - then.ms);
}

static ackts_packet_t *ackts_packet(uint32_t cookie)
{
	ackts_packet_t *p = &packets[cookie % ACKTS_SLOTS];
	return cookie != 0U && p->cookie == cookie ? p : NULL;
}

static void ackts_report(void *arg)
{
	uint32_t i;
	(void)arg;
	rt_printf("[ACKTS] CLOCK_MONOTONIC seconds.ms (1ms resolution); "
		  "delta_us=HAL clock; hw_begin/end bracket HAL submission, "
		  "not on-wire timestamps\r\n");
	for (;;) {
		ackts_time_t now;
		uint32_t lost, multi, overwritten;
		vTaskDelay(pdMS_TO_TICKS(100));
		now = ackts_now();
		for (i = 0; i < ACKTS_SLOTS; i++) {
			ackts_packet_t copy;
			int ready = 0;
			taskENTER_CRITICAL();
			if (packets[i].cookie != 0U &&
			    (packets[i].complete ||
			     ackts_age_ms(now, packets[i].ncm) >= ACKTS_EXPIRE_MS)) {
				copy = packets[i];
				packets[i].cookie = 0U;
				ready = 1;
			}
			taskEXIT_CRITICAL();
			if (!ready) continue;
			rt_printf("[ACKTS] id=%lu pkt=%lu seq=%lu bytes=%lu "
				  "enter=%lu.%03lu ncm=%lu.%03lu "
				  "hw_begin=%lu.%03lu hw_end=%lu.%03lu "
				  "done=%lu.%03lu valid_hw/done=%u/%u "
				  "delta_us enter_ncm/ncm_hw/hal=%lu/%lu/%lu "
				  "submit_n=%lu status_hw/tx=%d/%d\r\n",
				  (unsigned long)copy.response, (unsigned long)copy.cookie,
				  (unsigned long)copy.seq, (unsigned long)copy.bytes,
				  (unsigned long)copy.enter.sec, (unsigned long)copy.enter.ms,
				  (unsigned long)copy.ncm.sec, (unsigned long)copy.ncm.ms,
				  (unsigned long)copy.hw_begin.sec, (unsigned long)copy.hw_begin.ms,
				  (unsigned long)copy.hw_end.sec, (unsigned long)copy.hw_end.ms,
				  (unsigned long)copy.done.sec, (unsigned long)copy.done.ms,
				  copy.have_hw, copy.complete,
				  (unsigned long)(copy.ncm.us - copy.enter.us),
				  (unsigned long)(copy.have_hw ? copy.hw_begin.us - copy.ncm.us : 0U),
				  (unsigned long)(copy.have_hw ? copy.hw_end.us - copy.hw_begin.us : 0U),
				  (unsigned long)copy.submits, copy.hw_status, copy.status);
			rt_printf("[ACKQ] id=%lu pkt=%lu input/ready/free/busy=%lu/%lu/%lu/%lu "
				  "notify=%lu.%03lu dispatch=%lu.%03lu "
				  "valid_queue/notify/dispatch=%u/%u/%u core_events=%lu wait_max_us=%lu\r\n",
				  (unsigned long)copy.response, (unsigned long)copy.cookie,
				  (unsigned long)copy.input_depth, (unsigned long)copy.ready_depth,
				  (unsigned long)copy.free_slots, (unsigned long)copy.busy,
				  (unsigned long)copy.notify.sec, (unsigned long)copy.notify.ms,
				  (unsigned long)copy.dispatch.sec, (unsigned long)copy.dispatch.ms,
				  copy.have_queue, copy.have_notify, copy.have_dispatch,
				  (unsigned long)copy.core_events, (unsigned long)copy.core_wait_max_us);
		}
		for (i = 0; i < ACKTS_SLOTS; i++) {
			ackts_response_t copy;
			int ready = 0;
			taskENTER_CRITICAL();
			if (responses[i].id && !responses[i].reported && responses[i].ended &&
			    (responses[i].status || (!responses[i].packets &&
			     ackts_age_ms(now, responses[i].enter) >= ACKTS_EXPIRE_MS))) {
				copy = responses[i];
				responses[i].reported = 1;
				ready = 1;
			}
			taskEXIT_CRITICAL();
			if (ready)
				rt_printf("[ACKTS] id=%lu enter=%lu.%03lu write_ret=%lu.%03lu "
					  "status=%d tcp_range=%u packets=%lu (error or no NCM match)\r\n",
					  (unsigned long)copy.id,
					  (unsigned long)copy.enter.sec, (unsigned long)copy.enter.ms,
					  (unsigned long)copy.write_return.sec, (unsigned long)copy.write_return.ms,
					  copy.status, copy.have_seq, (unsigned long)copy.packets);
		}
		taskENTER_CRITICAL();
		lost = dropped; multi = ambiguous; overwritten = response_overrun;
		dropped = ambiguous = response_overrun = 0;
		taskEXIT_CRITICAL();
		if (lost || multi || overwritten)
			rt_printf("[ACKTS] trace_drop=%lu ambiguous_packet=%lu response_overrun=%lu\r\n",
				  (unsigned long)lost, (unsigned long)multi, (unsigned long)overwritten);
		ackts_report_extra(now);
	}
}

uint32_t car_ack_timestamp_begin(void)
{
	ackts_time_t now = ackts_now();
	ackts_response_t *r;
	uint32_t id;
	taskENTER_CRITICAL();
	id = ++next_response;
	if (!id) id = ++next_response;
	r = &responses[id % ACKTS_SLOTS];
	if (r->id && ((!r->packets && !r->reported) || !r->app_reported))
		response_overrun++;
	memset(r, 0, sizeof(*r));
	r->id = active_response = id;
	r->enter = now;
	if (have_http_done && http_task == xTaskGetCurrentTaskHandle()) {
		r->http_done = http_done;
		r->have_http = 1;
		have_http_done = 0;
	}
	response_task = xTaskGetCurrentTaskHandle();
	taskEXIT_CRITICAL();
	if (report_task == NULL)
		(void)xTaskCreate(ackts_report, "ackts", 2048U, NULL, 2U, &report_task);
	return id;
}

void car_ack_timestamp_end(uint32_t id, int status)
{
	ackts_time_t now = ackts_now();
	ackts_response_t *r;
	taskENTER_CRITICAL();
	r = &responses[id % ACKTS_SLOTS];
	if (r->id == id) {
		r->write_return = now;
		r->status = status;
		r->ended = 1;
	}
	if (active_response == id) active_response = 0;
	taskEXIT_CRITICAL();
}

extern err_t __real_tcp_write(struct tcp_pcb *, const void *, u16_t, u8_t);
err_t __wrap_tcp_write(struct tcp_pcb *pcb, const void *data, u16_t len, u8_t flags)
{
	uint32_t first = pcb != NULL ? pcb->snd_lbb : 0U;
	err_t result = __real_tcp_write(pcb, data, len, flags);
	ackts_response_t *r;
	/* tcp_write queues bytes but does not call tcp_output. Register the exact
	 * encrypted TCP sequence range before output can reach linkoutput. This
	 * also matches a delayed send after the HTTP write has already returned. */
	if (result != ERR_OK || len == 0U ||
	    xTaskGetCurrentTaskHandle() != response_task) return result;
	taskENTER_CRITICAL();
	r = &responses[active_response % ACKTS_SLOTS];
	if (active_response && r->id == active_response) {
		if (!r->have_seq) {
			r->first_seq = first;
			r->local_port = flow_local_port = pcb->local_port;
			r->peer_port = flow_peer_port = pcb->remote_port;
			r->ip_bytes = IP_IS_V6(&pcb->local_ip) ? 16U : 4U;
			if (r->ip_bytes == 16U) {
				memcpy(r->local_ip, ip_2_ip6(&pcb->local_ip)->addr, 16);
				memcpy(r->peer_ip, ip_2_ip6(&pcb->remote_ip)->addr, 16);
			} else {
				memcpy(r->local_ip, &ip_2_ip4(&pcb->local_ip)->addr, 4);
				memcpy(r->peer_ip, &ip_2_ip4(&pcb->remote_ip)->addr, 4);
			}
			r->have_seq = 1;
		}
		r->end_seq = first + len;
	}
	taskEXIT_CRITICAL();
	return result;
}

static uint16_t be16(const uint8_t *b)
{ return ((uint16_t)b[0] << 8) | b[1]; }
static uint32_t be32(const uint8_t *b)
{ return ((uint32_t)be16(b) << 16) | be16(b + 2); }

uint32_t car_ack_timestamp_ncm(const struct pbuf *p)
{
	uint8_t h[96];
	uint32_t tcp, end, hdr, src, dst, ip_bytes, seq, bytes, i, matches = 0;
	uint32_t cookie = 0;
	ackts_response_t *match = NULL;
	ackts_packet_t *packet;
	ackts_time_t now;
	uint16_t n;
	if (!flow_local_port || !p) return 0;
	n = p->tot_len < sizeof(h) ? p->tot_len : sizeof(h);
	if (n < 54U || pbuf_copy_partial(p, h, n, 0) != n) return 0;
	if (be16(h + 12) == 0x0800U && h[14] >> 4 == 4U) {
		hdr = (h[14] & 15U) * 4U;
		if (hdr < 20U || h[23] != 6U || (be16(h + 20) & 0x3fffU)) return 0;
		tcp = 14U + hdr; end = 14U + be16(h + 16);
		src = 26U; dst = 30U; ip_bytes = 4U;
	} else if (be16(h + 12) == 0x86ddU && h[14] >> 4 == 6U) {
		if (n < 74U || h[20] != 6U) return 0;
		tcp = 54U; end = 54U + be16(h + 18);
		src = 22U; dst = 38U; ip_bytes = 16U;
	} else return 0;
	if (tcp + 20U > n || end > p->tot_len || end < tcp + 20U ||
	    be16(h + tcp) != flow_local_port || be16(h + tcp + 2) != flow_peer_port)
		return 0;
	hdr = (h[tcp + 12] >> 4) * 4U;
	if (hdr < 20U || end <= tcp + hdr || (h[tcp + 13] & 0x02U)) return 0;
	seq = be32(h + tcp + 4); bytes = end - tcp - hdr;
	now = ackts_now();
	taskENTER_CRITICAL();
	for (i = 0; i < ACKTS_SLOTS; i++) {
		ackts_response_t *r = &responses[i];
		if (r->id && r->have_seq && r->ip_bytes == ip_bytes &&
		    r->local_port == be16(h + tcp) && r->peer_port == be16(h + tcp + 2) &&
		    ackts_age_ms(now, r->enter) < ACKTS_EXPIRE_MS &&
		    (int32_t)(seq - r->end_seq) < 0 &&
		    (int32_t)(r->first_seq - (seq + bytes)) < 0 &&
		    !memcmp(r->local_ip, h + src, ip_bytes) &&
		    !memcmp(r->peer_ip, h + dst, ip_bytes)) {
			match = r;
			matches++;
		}
	}
	/* Never attribute a coalesced packet spanning several responses to just
	 * one response. Such packets are explicitly counted as ambiguous. */
	if (matches > 1U) ambiguous++;
	if (matches == 1U) {
		cookie = ++next_packet;
		if (!cookie) cookie = ++next_packet;
		packet = &packets[cookie % ACKTS_SLOTS];
		if (packet->cookie) { dropped++; cookie = 0; }
		else {
			memset(packet, 0, sizeof(*packet));
			packet->cookie = cookie;
			packet->response = match->id;
			packet->enter = match->enter;
			packet->ncm = now;
			packet->seq = seq;
			packet->bytes = bytes;
			match->packets++;
		}
	}
	taskEXIT_CRITICAL();
	return cookie;
}

void car_ack_timestamp_usb_begin(uint32_t packet, const void *buffer)
{
	taskENTER_CRITICAL();
	active_usb_packet = packet;
	active_usb_buffer = buffer;
	taskEXIT_CRITICAL();
}

void car_ack_timestamp_usb_end(uint32_t cookie, int status)
{
	ackts_time_t now;
	ackts_packet_t *p;
	if (!cookie) return;
	now = ackts_now();
	taskENTER_CRITICAL();
	p = ackts_packet(cookie);
	if (p) { p->done = now; p->status = status; p->complete = 1; }
	if (active_usb_packet == cookie) {
		active_usb_packet = 0;
		active_usb_buffer = NULL;
	}
	taskEXIT_CRITICAL();
}

extern uint8_t __real_usbh_hal_hc_start_transfer(void *hc, uint8_t dma);
uint8_t __wrap_usbh_hal_hc_start_transfer(void *hc, uint8_t dma)
{
	const uint8_t *channel = hc;
	uint32_t cookie = 0;
	ackts_time_t before, after;
	uint8_t result;
	ackts_packet_t *p;
	/* Closed customer HCD ABI, verified against usbh_hcd_hc_submit_request:
	 * buffer @0, endpoint type @0x2a, direction @0x2f. Match the sole USB
	 * owner's exact NTB buffer, not every outgoing transfer on the host. */
	taskENTER_CRITICAL();
	/* NCM bulk IN is channel 4 in the customer HCD. Preserve a transfer
	 * generation so a later RX notification cannot be attached to an old NTB. */
	if (channel[0x25] == 4U && channel[0x2a] == 2U && channel[0x2f] != 0U) {
		rx_hc = (volatile uint8_t *)hc;
		rx_generation++;
		rx_context_valid = 0;
	}
	if (active_usb_packet && channel[0x2a] == 2U && channel[0x2f] == 0U &&
	    *(void *const *)hc == active_usb_buffer) cookie = active_usb_packet;
	taskEXIT_CRITICAL();
	if (!cookie) return __real_usbh_hal_hc_start_transfer(hc, dma);
	before = ackts_now();
	result = __real_usbh_hal_hc_start_transfer(hc, dma);
	after = ackts_now();
	taskENTER_CRITICAL();
	p = ackts_packet(cookie);
	if (p) {
		p->submits++;
		if (!p->have_hw) {
			p->hw_begin = before; p->hw_end = after;
			p->hw_status = result; p->have_hw = 1;
		}
	}
	taskEXIT_CRITICAL();
	return result;
}

void car_ack_timestamp_http_done(int status)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	const char *name;
	ackts_time_t now;
	if (status != 0) return;
	name = pcTaskGetName(task);
	if (!name || strcmp(name, "TCPClient")) return;
	now = ackts_now();
	taskENTER_CRITICAL();
	http_done = now; http_task = task; have_http_done = 1;
	taskEXIT_CRITICAL();
}

void car_ack_timestamp_hid_stage(int finished)
{
	ackts_response_t *r;
	ackts_time_t now;
	if (xTaskGetCurrentTaskHandle() != response_task || !next_response) return;
	now = ackts_now();
	taskENTER_CRITICAL();
	r = &responses[next_response % ACKTS_SLOTS];
	if (r->id == next_response) {
		if (finished) { r->hid_end = now; r->hid_finished = 1; }
		else r->hid_begin = now;
	}
	taskEXIT_CRITICAL();
}

void car_ack_timestamp_queue(uint32_t cookie, uint32_t input,
	uint32_t ready, uint32_t free_slots, uint32_t busy)
{
	ackts_packet_t *p;
	taskENTER_CRITICAL();
	p = ackts_packet(cookie);
	if (p) {
		p->input_depth = input; p->ready_depth = ready;
		p->free_slots = free_slots; p->busy = busy; p->have_queue = 1;
	}
	taskEXIT_CRITICAL();
}

/* RX is logged by TCP sequence, not guessed to be a particular HTTP request.
 * A TCP cumulative ACK is not a one-to-one HTTP message identifier. */
void car_ack_timestamp_rx(const struct pbuf *p)
{
	uint8_t h[96];
	uint32_t tcp, end, hdr, src, dst, ip_bytes, seq, ack, bytes;
	uint16_t n;
	ackts_time_t now;
	ackts_rx_t *r;
	ackts_response_t *flow;
	if (!flow_local_port || !p) return;
	n = p->tot_len < sizeof(h) ? p->tot_len : sizeof(h);
	if (n < 54U || pbuf_copy_partial(p, h, n, 0) != n) return;
	if (be16(h + 12) == 0x0800U && h[14] >> 4 == 4U) {
		hdr = (h[14] & 15U) * 4U;
		if (hdr < 20U || h[23] != 6U || (be16(h + 20) & 0x3fffU)) return;
		tcp = 14U + hdr; end = 14U + be16(h + 16);
		src = 26U; dst = 30U; ip_bytes = 4U;
	} else if (be16(h + 12) == 0x86ddU && h[14] >> 4 == 6U) {
		if (n < 74U || h[20] != 6U) return;
		tcp = 54U; end = 54U + be16(h + 18);
		src = 22U; dst = 38U; ip_bytes = 16U;
	} else return;
	if (tcp + 20U > n || end > p->tot_len || end < tcp + 20U ||
	    be16(h + tcp) != flow_peer_port || be16(h + tcp + 2) != flow_local_port)
		return;
	hdr = (h[tcp + 12] >> 4) * 4U;
	if (hdr < 20U || end <= tcp + hdr) return;
	seq = be32(h + tcp + 4); ack = be32(h + tcp + 8); bytes = end - tcp - hdr;
	now = ackts_now();
	taskENTER_CRITICAL();
	flow = &responses[next_response % ACKTS_SLOTS];
	if (!flow->have_seq || flow->ip_bytes != ip_bytes ||
	    memcmp(flow->peer_ip, h + src, ip_bytes) ||
	    memcmp(flow->local_ip, h + dst, ip_bytes)) {
		taskEXIT_CRITICAL(); return;
	}
	r = &rx_records[rx_next++ % ACKTS_SLOTS];
	if (r->occupied) rx_drop++;
	else {
		memset(r, 0, sizeof(*r)); r->occupied = 1;
		r->seq = seq; r->ack = ack; r->bytes = bytes; r->ingress = now;
		if (rx_context_valid && xTaskGetCurrentTaskHandle() == core_task &&
		    rx_context_generation == rx_generation) {
			r->valid_usb = 1; r->generation = rx_context_generation;
			r->done = rx_done_time; r->dispatch = rx_dispatch_time;
		}
	}
	taskEXIT_CRITICAL();
}

extern int __real_usb_os_queue_send(void *, const void *, uint32_t);
extern int __real_usb_os_queue_receive(void *, void *, uint32_t);
int __wrap_usb_os_queue_send(void *queue, const void *message, uint32_t timeout)
{
	ackts_queue_event_t event;
	ackts_packet_t *p;
	int result;
	if (queue != core_queue || !message || timeout != 0U)
		return __real_usb_os_queue_send(queue, message, timeout);
	memset(&event, 0, sizeof(event));
	event.channel = *(const uint32_t *)message;
	event.time = ackts_now();
	/* All USB core sends are nonblocking. Hold a short critical section so
	 * the timestamp sidecar is visible before the awakened receiver runs. */
	taskENTER_CRITICAL();
	event.packet = event.channel == tx_channel ? active_usb_packet : 0U;
	if (event.channel == 4U && rx_hc && rx_hc[0x28] == 1U &&
	    rx_notified != rx_generation) {
		event.rx_done = 1; event.generation = rx_generation;
	}
	result = __real_usb_os_queue_send(queue, message, timeout);
	if (!result && core_synced && core_count < 32U) {
		core_events[(core_head + core_count) % 32U] = event;
		core_count++;
		if (event.rx_done) rx_notified = event.generation;
		p = ackts_packet(event.packet);
		if (p && !p->have_notify) { p->notify = event.time; p->have_notify = 1; }
	} else if (!result && core_synced) {
		core_synced = 0; core_count = 0; core_desync++;
	} else if (result) core_send_fail++;
	taskEXIT_CRITICAL();
	return result;
}

int __wrap_usb_os_queue_receive(void *queue, void *message, uint32_t timeout)
{
	int result;
	ackts_time_t now;
	ackts_queue_event_t event;
	ackts_packet_t *p;
	if (!core_queue) {
		const char *name = pcTaskGetName(NULL);
		if (name && !strcmp(name, "usbh_main_task")) {
			taskENTER_CRITICAL();
			core_queue = queue; core_task = xTaskGetCurrentTaskHandle();
			core_synced = uxQueueMessagesWaiting(queue) == 0U;
			taskEXIT_CRITICAL();
		}
	}
	result = __real_usb_os_queue_receive(queue, message, timeout);
	if (result || queue != core_queue || !message) return result;
	now = ackts_now();
	taskENTER_CRITICAL();
	rx_context_valid = 0;
	last_urb_valid = last_elapsed_valid = 0;
	if (core_synced && core_count &&
	    core_events[core_head].channel == *(uint32_t *)message) {
		event = core_events[core_head];
		core_head = (core_head + 1U) % 32U; core_count--;
		p = ackts_packet(event.packet);
		if (p) {
			uint32_t wait = now.us - event.time.us;
			p->core_events++;
			if (!p->have_dispatch) { p->dispatch = now; p->have_dispatch = 1; }
			if (wait > p->core_wait_max_us) p->core_wait_max_us = wait;
		}
		if (event.rx_done && event.generation == rx_generation) {
			rx_done_time = event.time; rx_dispatch_time = now;
			rx_context_generation = event.generation; rx_context_valid = 1;
		}
	} else {
		if (core_synced) core_desync++;
		core_synced = 0; core_count = 0; core_head = 0;
		if (uxQueueMessagesWaiting(queue) == 0U) core_synced = 1;
	}
	taskEXIT_CRITICAL();
	return result;
}

extern uint8_t __real_usbh_get_urb_state(void *, uint8_t);
uint8_t __wrap_usbh_get_urb_state(void *host, uint8_t channel)
{
	uint8_t state = __real_usbh_get_urb_state(host, channel);
	if (xTaskGetCurrentTaskHandle() == core_task) {
		if (!last_urb_valid || channel != last_urb_channel || state != last_urb_state) last_elapsed_valid = 0;
		last_urb_channel = channel; last_urb_state = state; last_urb_valid = 1;
	}
	return state;
}

extern uint32_t __real_usbh_get_elapsed_ticks(void *, uint32_t);
uint32_t __wrap_usbh_get_elapsed_ticks(void *host, uint32_t base)
{
	uint32_t elapsed = __real_usbh_get_elapsed_ticks(host, base);
	if (xTaskGetCurrentTaskHandle() == core_task && last_urb_valid) {
		last_elapsed = elapsed; last_elapsed_base = base; last_elapsed_valid = 1;
	}
	return elapsed;
}

static void ackts_recovery(uint32_t channel, uint32_t action)
{
	ackts_recovery_t *r = NULL;
	ackts_time_t now;
	uint32_t i;
	if (!flow_local_port || xTaskGetCurrentTaskHandle() != core_task ||
	    !last_urb_valid || channel != last_urb_channel ||
	    (channel != 4U && channel != tx_channel)) return;
	now = ackts_now();
	taskENTER_CRITICAL();
	for (i = 0; i < 16U; i++) {
		ackts_recovery_t *candidate = &recoveries[i];
		if (!candidate->occupied) { if (!r) r = candidate; continue; }
		if (candidate->channel == channel && candidate->action == action &&
		    candidate->state == last_urb_state && candidate->base == (last_elapsed_valid ? last_elapsed_base : UINT32_MAX) &&
		    candidate->packet == (channel == tx_channel ? active_usb_packet : 0U)) {
			candidate->count++; taskEXIT_CRITICAL(); return;
		}
	}
	if (!r) recovery_drop++;
	else {
		r->occupied = 1; r->channel = channel; r->action = action;
		r->state = last_urb_state; r->time = now; r->base = last_elapsed_valid ? last_elapsed_base : UINT32_MAX;
		r->elapsed = last_elapsed_valid ? last_elapsed : UINT32_MAX;
		r->packet = channel == tx_channel ? active_usb_packet : 0;
		r->count = 1;
	}
	taskEXIT_CRITICAL();
}

extern uint8_t __real_usbh_check_nak_timeout(void *, uint8_t, uint8_t);
uint8_t __wrap_usbh_check_nak_timeout(void *host, uint8_t channel, uint8_t limit)
{
	/* This call is after the TX 80-tick / RX 40-tick NOTREADY gates in the
	 * validated NCM driver. Log the observed elapsed value, not an assumption. */
	ackts_recovery(channel, 0);
	return __real_usbh_check_nak_timeout(host, channel, limit);
}
extern void __real_usbh_trigger_rexfer(void *, uint8_t);
void __wrap_usbh_trigger_rexfer(void *host, uint8_t channel)
{
	ackts_recovery(channel, 1);
	__real_usbh_trigger_rexfer(host, channel);
}
extern void __real_usbh_enable_nak_interrupt(void *, uint8_t);
void __wrap_usbh_enable_nak_interrupt(void *host, uint8_t channel)
{
	ackts_recovery(channel, 2);
	__real_usbh_enable_nak_interrupt(host, channel);
}

static void ackts_report_extra(ackts_time_t now)
{
	uint32_t i, rx_lost, recovery_lost, desync, send_fail;
	for (i = 0; i < ACKTS_SLOTS; i++) {
		ackts_response_t app;
		ackts_rx_t rx;
		int have_app = 0;
		taskENTER_CRITICAL();
		if (responses[i].id && !responses[i].app_reported &&
		    (responses[i].hid_finished || ackts_age_ms(now, responses[i].enter) >= ACKTS_EXPIRE_MS)) {
			app = responses[i]; responses[i].app_reported = 1; have_app = 1;
		}
		rx = rx_records[i]; rx_records[i].occupied = 0;
		taskEXIT_CRITICAL();
		if (have_app)
			rt_printf("[ACKAPP] id=%lu http_done=%lu.%03lu enter=%lu.%03lu "
				  "hid_begin/end=%lu.%03lu/%lu.%03lu valid_http/hid_end=%u/%u "
				  "http_enter_us=%lu hid_us=%lu\r\n",
				  (unsigned long)app.id, (unsigned long)app.http_done.sec, (unsigned long)app.http_done.ms,
				  (unsigned long)app.enter.sec, (unsigned long)app.enter.ms,
				  (unsigned long)app.hid_begin.sec, (unsigned long)app.hid_begin.ms,
				  (unsigned long)app.hid_end.sec, (unsigned long)app.hid_end.ms,
				  app.have_http, app.hid_finished,
				  (unsigned long)(app.have_http ? app.enter.us - app.http_done.us : 0U),
				  (unsigned long)(app.hid_finished ? app.hid_end.us - app.hid_begin.us : 0U));
		if (rx.occupied)
			rt_printf("[ACKRX] seq=%lu ack=%lu bytes=%lu usb_gen=%lu "
				  "done_notify=%lu.%03lu dispatch=%lu.%03lu ingress=%lu.%03lu valid_usb=%u\r\n",
				  (unsigned long)rx.seq, (unsigned long)rx.ack, (unsigned long)rx.bytes,
				  (unsigned long)rx.generation,
				  (unsigned long)rx.done.sec, (unsigned long)rx.done.ms,
				  (unsigned long)rx.dispatch.sec, (unsigned long)rx.dispatch.ms,
				  (unsigned long)rx.ingress.sec, (unsigned long)rx.ingress.ms, rx.valid_usb);
	}
	for (i = 0; i < 16U; i++) {
		ackts_recovery_t r;
		static const char *const actions[] = {"nak_check", "rexfer", "nak_irq_enable"};
		taskENTER_CRITICAL(); r = recoveries[i]; recoveries[i].occupied = 0; taskEXIT_CRITICAL();
		if (r.occupied)
			rt_printf("[ACKUSB] t=%lu.%03lu ch=%lu urb=%lu action=%s "
				  "elapsed_ticks=%lu base_tick=%lu pkt=%lu count=%lu\r\n",
				  (unsigned long)r.time.sec, (unsigned long)r.time.ms,
				  (unsigned long)r.channel, (unsigned long)r.state, actions[r.action],
				  (unsigned long)r.elapsed, (unsigned long)r.base,
				  (unsigned long)r.packet, (unsigned long)r.count);
	}
	taskENTER_CRITICAL();
	rx_lost = rx_drop; recovery_lost = recovery_drop; desync = core_desync; send_fail = core_send_fail;
	rx_drop = recovery_drop = core_desync = core_send_fail = 0;
	taskEXIT_CRITICAL();
	if (rx_lost || recovery_lost || desync || send_fail)
		rt_printf("[ACKTS] rx_drop=%lu recovery_drop=%lu core_desync=%lu core_send_fail=%lu\r\n",
			  (unsigned long)rx_lost, (unsigned long)recovery_lost,
			  (unsigned long)desync, (unsigned long)send_fail);
}
#endif
