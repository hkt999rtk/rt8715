#include "car_ack_wire_profiler.h"

#ifndef CONFIG_CAR_ACK_WIRE_PROFILE
#define CONFIG_CAR_ACK_WIRE_PROFILE 0
#endif

#if CONFIG_CAR_ACK_WIRE_PROFILE

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "hal_timer.h"
#include "lwip/pbuf.h"
#include "lwip/sockets.h"
#include "car_ack_response_cache.h"

#define CAR_ACK_WIRE_TRACK_SLOTS 16U
#define CAR_ACK_WIRE_REPORT_MS 10000U

typedef struct car_ack_wire_stage_s {
	uint64_t total_us;
	uint32_t max_us;
	uint32_t samples;
} car_ack_wire_stage_t;

typedef struct car_ack_wire_track_s {
	uint32_t token;
	uint32_t response_us;
	uint32_t enqueue_us;
	uint32_t dequeue_us;
	uint8_t valid;
} car_ack_wire_track_t;

typedef struct car_ack_wire_stats_s {
	uint32_t requests;
	uint32_t responses;
	uint32_t write_complete;
	uint32_t response_errors;
	uint32_t flow_match;
	uint32_t flow_miss;
	uint32_t enqueue;
	uint32_t dequeue;
	uint32_t usb_submit;
	uint32_t usb_complete;
	uint32_t usb_error;
	uint32_t track_miss;
	car_ack_wire_stage_t request_to_response;
	car_ack_wire_stage_t response_to_write;
	car_ack_wire_stage_t response_to_local_complete;
	car_ack_wire_stage_t response_to_ncm_enqueue;
	car_ack_wire_stage_t ncm_queue;
	car_ack_wire_stage_t ncm_to_usb_submit;
	car_ack_wire_stage_t usb_transfer;
	car_ack_wire_stage_t response_to_usb_complete;
} car_ack_wire_stats_t;

static car_ack_wire_track_t car_ack_wire_tracks[CAR_ACK_WIRE_TRACK_SLOTS];
static car_ack_wire_stats_t car_ack_wire_stats;
static TaskHandle_t car_ack_wire_report_task;
static uint32_t car_ack_wire_next_token;
static uint32_t car_ack_wire_active_token;
static uint32_t car_ack_wire_last_request_us;
static uint32_t car_ack_wire_active_response_us;
static int car_ack_wire_socket = -1;
static uint16_t car_ack_wire_local_port;
static uint16_t car_ack_wire_peer_port;
static uint8_t car_ack_wire_flow_valid;

static void car_ack_wire_stage_add(car_ack_wire_stage_t *stage,
				   uint32_t elapsed_us)
{
	stage->samples++;
	stage->total_us += elapsed_us;
	if (elapsed_us > stage->max_us) stage->max_us = elapsed_us;
}

static uint32_t car_ack_wire_stage_avg(const car_ack_wire_stage_t *stage)
{
	return stage->samples != 0U ?
		(uint32_t)(stage->total_us / stage->samples) : 0U;
}

static car_ack_wire_track_t *car_ack_wire_find(uint32_t token)
{
	car_ack_wire_track_t *track;

	if (token == 0U) return NULL;
	track = &car_ack_wire_tracks[token % CAR_ACK_WIRE_TRACK_SLOTS];
	return track->valid != 0U && track->token == token ? track : NULL;
}

static uint16_t car_ack_wire_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int car_ack_wire_parse_tcp(const struct pbuf *p,
				  uint16_t *source_port,
				  uint16_t *destination_port,
				  uint32_t *payload_bytes)
{
	uint8_t header[96];
	uint16_t frame_len;
	uint16_t ether_type;
	uint32_t ip_offset = 14U;
	uint32_t tcp_offset;
	uint32_t network_end;
	uint32_t tcp_header;
	uint8_t protocol;

	if (p == NULL || p->tot_len < 54U) return 0;
	frame_len = p->tot_len < sizeof(header) ? p->tot_len : sizeof(header);
	if (pbuf_copy_partial((struct pbuf *)p, header, frame_len, 0U) !=
	    frame_len) return 0;
	ether_type = car_ack_wire_be16(header + 12U);
	if (ether_type == 0x0800U) {
		uint32_t ip_header;
		uint32_t total_length;

		if (frame_len < ip_offset + 20U || (header[ip_offset] >> 4) != 4U)
			return 0;
		ip_header = (uint32_t)(header[ip_offset] & 0x0fU) * 4U;
		if (ip_header < 20U || frame_len < ip_offset + ip_header + 20U)
			return 0;
		protocol = header[ip_offset + 9U];
		total_length = car_ack_wire_be16(header + ip_offset + 2U);
		network_end = ip_offset + total_length;
		tcp_offset = ip_offset + ip_header;
	} else if (ether_type == 0x86ddU) {
		uint32_t ipv6_payload;

		if (frame_len < ip_offset + 60U || (header[ip_offset] >> 4) != 6U)
			return 0;
		protocol = header[ip_offset + 6U];
		ipv6_payload = car_ack_wire_be16(header + ip_offset + 4U);
		network_end = ip_offset + 40U + ipv6_payload;
		tcp_offset = ip_offset + 40U;
	} else {
		return 0;
	}
	if (protocol != 6U || frame_len < tcp_offset + 20U) return 0;
	tcp_header = (uint32_t)(header[tcp_offset + 12U] >> 4) * 4U;
	if (tcp_header < 20U || network_end < tcp_offset + tcp_header)
		return 0;
	*source_port = car_ack_wire_be16(header + tcp_offset);
	*destination_port = car_ack_wire_be16(header + tcp_offset + 2U);
	*payload_bytes = network_end - tcp_offset - tcp_header;
	return 1;
}

static void car_ack_wire_report(void *arg)
{
	uint32_t sequence = 0U;
	(void)arg;

	for (;;) {
		car_ack_wire_stats_t stats;
		struct lwip_car_ack_diag tcp;
		int socket_fd;
		int tcp_valid = 0;

		vTaskDelay(pdMS_TO_TICKS(CAR_ACK_WIRE_REPORT_MS));
		taskENTER_CRITICAL();
		stats = car_ack_wire_stats;
		memset(&car_ack_wire_stats, 0, sizeof(car_ack_wire_stats));
		socket_fd = car_ack_wire_socket;
		taskEXIT_CRITICAL();
		memset(&tcp, 0, sizeof(tcp));
		if (socket_fd >= 0 &&
		    lwip_diag_car_ack_snapshot(socket_fd, &tcp, 1,
					       hal_read_curtime_us()) == 0)
			tcp_valid = 1;
		sequence++;
		rt_printf("[CARACKWIRE][%lu] req/resp/write/local_err="
			  "%lu/%lu/%lu/%lu flow match/miss=%lu/%lu "
			  "ncm enq/deq=%lu/%lu usb submit/done/error=%lu/%lu/%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)stats.requests,
			  (unsigned long)stats.responses,
			  (unsigned long)stats.write_complete,
			  (unsigned long)stats.response_errors,
			  (unsigned long)stats.flow_match,
			  (unsigned long)stats.flow_miss,
			  (unsigned long)stats.enqueue,
			  (unsigned long)stats.dequeue,
			  (unsigned long)stats.usb_submit,
			  (unsigned long)stats.usb_complete,
			  (unsigned long)stats.usb_error);
		rt_printf("[CARACKWIRE][%lu] us avg/max req_to_resp=%lu/%lu "
			  "resp_to_write=%lu/%lu local=%lu/%lu "
			  "resp_to_ncm=%lu/%lu ncm_queue=%lu/%lu "
			  "ncm_to_usb=%lu/%lu usb=%lu/%lu resp_to_usb_done=%lu/%lu "
			  "track_miss=%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)car_ack_wire_stage_avg(&stats.request_to_response),
			  (unsigned long)stats.request_to_response.max_us,
			  (unsigned long)car_ack_wire_stage_avg(&stats.response_to_write),
			  (unsigned long)stats.response_to_write.max_us,
			  (unsigned long)car_ack_wire_stage_avg(&stats.response_to_local_complete),
			  (unsigned long)stats.response_to_local_complete.max_us,
			  (unsigned long)car_ack_wire_stage_avg(&stats.response_to_ncm_enqueue),
			  (unsigned long)stats.response_to_ncm_enqueue.max_us,
			  (unsigned long)car_ack_wire_stage_avg(&stats.ncm_queue),
			  (unsigned long)stats.ncm_queue.max_us,
			  (unsigned long)car_ack_wire_stage_avg(&stats.ncm_to_usb_submit),
			  (unsigned long)stats.ncm_to_usb_submit.max_us,
			  (unsigned long)car_ack_wire_stage_avg(&stats.usb_transfer),
			  (unsigned long)stats.usb_transfer.max_us,
			  (unsigned long)car_ack_wire_stage_avg(&stats.response_to_usb_complete),
			  (unsigned long)stats.response_to_usb_complete.max_us,
			  (unsigned long)stats.track_miss);
		rt_printf("[CARACKWIRE][%lu] peer valid=%d mark/ack/already/pending="
			  "%lu/%lu/%lu/%lu sent_now/buffered=%lu/%lu "
			  "ack_us n/avg/max=%lu/%llu/%lu bins <=.1/1/5/20/>20ms="
			  "%lu/%lu/%lu/%lu/%lu rto/rexmit=%lu/%lu\r\n",
			  (unsigned long)sequence, tcp_valid,
			  (unsigned long)tcp.marked,
			  (unsigned long)tcp.acked,
			  (unsigned long)tcp.already_acked,
			  (unsigned long)tcp.pending,
			  (unsigned long)tcp.sent_immediate,
			  (unsigned long)tcp.locally_buffered,
			  (unsigned long)tcp.ack_samples,
			  (unsigned long long)(tcp.ack_samples != 0U ?
				tcp.ack_us_sum / tcp.ack_samples : 0U),
			  (unsigned long)tcp.ack_us_max,
			  (unsigned long)tcp.ack_le_100us,
			  (unsigned long)tcp.ack_le_1ms,
			  (unsigned long)tcp.ack_le_5ms,
			  (unsigned long)tcp.ack_le_20ms,
			  (unsigned long)tcp.ack_gt_20ms,
			  (unsigned long)tcp.rto_expired,
			  (unsigned long)tcp.rto_retransmit);
		carbox_car_ack_response_cache_report(sequence);
	}
}

void car_ack_wire_set_flow(int socket_fd, uint16_t local_port,
			   uint16_t peer_port)
{
	taskENTER_CRITICAL();
	car_ack_wire_socket = socket_fd;
	car_ack_wire_local_port = local_port;
	car_ack_wire_peer_port = peer_port;
	car_ack_wire_flow_valid = local_port != 0U && peer_port != 0U;
	taskEXIT_CRITICAL();
	if (car_ack_wire_report_task == NULL)
		(void)xTaskCreate(car_ack_wire_report, "carackwire", 2048U, NULL,
				  2U, &car_ack_wire_report_task);
}

void car_ack_wire_request_complete(uint32_t now_us)
{
	taskENTER_CRITICAL();
	car_ack_wire_stats.requests++;
	car_ack_wire_last_request_us = now_us;
	taskEXIT_CRITICAL();
}

uint32_t car_ack_wire_response_begin(uint32_t now_us)
{
	car_ack_wire_track_t *track;
	uint32_t token;

	taskENTER_CRITICAL();
	token = ++car_ack_wire_next_token;
	if (token == 0U) token = ++car_ack_wire_next_token;
	track = &car_ack_wire_tracks[token % CAR_ACK_WIRE_TRACK_SLOTS];
	memset(track, 0, sizeof(*track));
	track->token = token;
	track->response_us = now_us;
	track->valid = 1U;
	car_ack_wire_active_token = token;
	car_ack_wire_active_response_us = now_us;
	car_ack_wire_stats.responses++;
	if (car_ack_wire_last_request_us != 0U)
		car_ack_wire_stage_add(&car_ack_wire_stats.request_to_response,
			now_us - car_ack_wire_last_request_us);
	taskEXIT_CRITICAL();
	return token;
}

void car_ack_wire_http_write_complete(uint32_t now_us)
{
	taskENTER_CRITICAL();
	car_ack_wire_stats.write_complete++;
	if (car_ack_wire_active_response_us != 0U)
		car_ack_wire_stage_add(&car_ack_wire_stats.response_to_write,
			now_us - car_ack_wire_active_response_us);
	taskEXIT_CRITICAL();
}

void car_ack_wire_response_complete(uint32_t token, uint32_t now_us,
				    int status)
{
	car_ack_wire_track_t *track;

	taskENTER_CRITICAL();
	track = car_ack_wire_find(token);
	if (track != NULL)
		car_ack_wire_stage_add(&car_ack_wire_stats.response_to_local_complete,
			now_us - track->response_us);
	else
		car_ack_wire_stats.track_miss++;
	if (status != 0) car_ack_wire_stats.response_errors++;
	if (car_ack_wire_active_token == token ||
	    (track != NULL &&
	     car_ack_wire_active_response_us == track->response_us)) {
		car_ack_wire_active_token = 0U;
		car_ack_wire_active_response_us = 0U;
	}
	taskEXIT_CRITICAL();
}

uint32_t car_ack_wire_ncm_match_pbuf(const struct pbuf *p)
{
	uint16_t source_port;
	uint16_t destination_port;
	uint16_t local_port;
	uint16_t peer_port;
	uint32_t payload_bytes;
	uint32_t token;
	uint8_t valid;

	taskENTER_CRITICAL();
	valid = car_ack_wire_flow_valid;
	local_port = car_ack_wire_local_port;
	peer_port = car_ack_wire_peer_port;
	token = car_ack_wire_active_token;
	taskEXIT_CRITICAL();
	if (valid == 0U || token == 0U ||
	    !car_ack_wire_parse_tcp(p, &source_port, &destination_port,
				    &payload_bytes) || payload_bytes == 0U ||
	    source_port != local_port || destination_port != peer_port)
		return 0U;
	taskENTER_CRITICAL();
	if (car_ack_wire_active_token == token) {
		car_ack_wire_active_token = 0U;
		car_ack_wire_stats.flow_match++;
	} else {
		token = 0U;
		car_ack_wire_stats.flow_miss++;
	}
	taskEXIT_CRITICAL();
	return token;
}

void car_ack_wire_ncm_enqueue(uint32_t token, uint32_t now_us)
{
	car_ack_wire_track_t *track;
	taskENTER_CRITICAL();
	track = car_ack_wire_find(token);
	if (track != NULL) {
		track->enqueue_us = now_us;
		car_ack_wire_stats.enqueue++;
		car_ack_wire_stage_add(&car_ack_wire_stats.response_to_ncm_enqueue,
			now_us - track->response_us);
	} else car_ack_wire_stats.track_miss++;
	taskEXIT_CRITICAL();
}

void car_ack_wire_ncm_dequeue(uint32_t token, uint32_t now_us)
{
	car_ack_wire_track_t *track;
	taskENTER_CRITICAL();
	track = car_ack_wire_find(token);
	if (track != NULL) {
		track->dequeue_us = now_us;
		car_ack_wire_stats.dequeue++;
		car_ack_wire_stage_add(&car_ack_wire_stats.ncm_queue,
			now_us - track->enqueue_us);
	} else car_ack_wire_stats.track_miss++;
	taskEXIT_CRITICAL();
}

void car_ack_wire_usb_submit(uint32_t token, uint32_t now_us)
{
	car_ack_wire_track_t *track;
	taskENTER_CRITICAL();
	track = car_ack_wire_find(token);
	if (track != NULL) {
		car_ack_wire_stats.usb_submit++;
		car_ack_wire_stage_add(&car_ack_wire_stats.ncm_to_usb_submit,
			now_us - track->dequeue_us);
		track->dequeue_us = now_us;
	} else car_ack_wire_stats.track_miss++;
	taskEXIT_CRITICAL();
}

void car_ack_wire_usb_complete(uint32_t token, uint32_t now_us, int status)
{
	car_ack_wire_track_t *track;
	taskENTER_CRITICAL();
	track = car_ack_wire_find(token);
	if (track != NULL) {
		car_ack_wire_stats.usb_complete++;
		if (status != 0) car_ack_wire_stats.usb_error++;
		car_ack_wire_stage_add(&car_ack_wire_stats.usb_transfer,
			now_us - track->dequeue_us);
		car_ack_wire_stage_add(&car_ack_wire_stats.response_to_usb_complete,
			now_us - track->response_us);
		track->valid = 0U;
	} else car_ack_wire_stats.track_miss++;
	taskEXIT_CRITICAL();
}

#else

void car_ack_wire_set_flow(int s, uint16_t l, uint16_t p)
{ (void)s; (void)l; (void)p; }
void car_ack_wire_request_complete(uint32_t t) { (void)t; }
uint32_t car_ack_wire_response_begin(uint32_t t) { (void)t; return 0U; }
void car_ack_wire_http_write_complete(uint32_t t) { (void)t; }
void car_ack_wire_response_complete(uint32_t k, uint32_t t, int s)
{ (void)k; (void)t; (void)s; }
uint32_t car_ack_wire_ncm_match_pbuf(const struct pbuf *p)
{ (void)p; return 0U; }
void car_ack_wire_ncm_enqueue(uint32_t k, uint32_t t) { (void)k; (void)t; }
void car_ack_wire_ncm_dequeue(uint32_t k, uint32_t t) { (void)k; (void)t; }
void car_ack_wire_usb_submit(uint32_t k, uint32_t t) { (void)k; (void)t; }
void car_ack_wire_usb_complete(uint32_t k, uint32_t t, int s)
{ (void)k; (void)t; (void)s; }
#endif
