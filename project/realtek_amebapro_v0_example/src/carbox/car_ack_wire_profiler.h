#ifndef CARBOX_CAR_ACK_WIRE_PROFILER_H
#define CARBOX_CAR_ACK_WIRE_PROFILER_H

#include <stdint.h>

struct pbuf;

void car_ack_wire_set_flow(int socket_fd, uint16_t local_port,
			   uint16_t peer_port);
void car_ack_wire_request_complete(uint32_t now_us);
uint32_t car_ack_wire_response_begin(uint32_t now_us);
void car_ack_wire_http_write_complete(uint32_t now_us);
void car_ack_wire_response_complete(uint32_t token, uint32_t now_us,
				    int status);
uint32_t car_ack_wire_ncm_match_pbuf(const struct pbuf *p);
void car_ack_wire_ncm_enqueue(uint32_t token, uint32_t now_us);
void car_ack_wire_ncm_dequeue(uint32_t token, uint32_t now_us);
void car_ack_wire_usb_submit(uint32_t token, uint32_t now_us);
void car_ack_wire_usb_complete(uint32_t token, uint32_t now_us, int status);

#endif
