#ifndef CAR_ACK_TIMESTAMP_H
#define CAR_ACK_TIMESTAMP_H

#include <stdint.h>
#include <stddef.h>

#ifndef CONFIG_CAR_ACK_TIMESTAMP
#define CONFIG_CAR_ACK_TIMESTAMP 0
#endif

struct pbuf;
#if CONFIG_CAR_ACK_TIMESTAMP
uint32_t car_ack_timestamp_begin(void);
void car_ack_timestamp_end(uint32_t id, int status);
uint32_t car_ack_timestamp_ncm(const struct pbuf *p);
void car_ack_timestamp_usb_begin(uint32_t packet, const void *buffer);
void car_ack_timestamp_usb_end(uint32_t packet, int status);
void car_ack_timestamp_http_done(int status);
void car_ack_timestamp_hid_stage(int finished);
void car_ack_timestamp_rx(const struct pbuf *p);
void car_ack_timestamp_queue(uint32_t packet, uint32_t input,
	uint32_t ready, uint32_t free_slots, uint32_t busy);
#else
static inline uint32_t car_ack_timestamp_begin(void) { return 0; }
static inline void car_ack_timestamp_end(uint32_t id, int status)
{ (void)id; (void)status; }
static inline uint32_t car_ack_timestamp_ncm(const struct pbuf *p)
{ (void)p; return 0; }
static inline void car_ack_timestamp_usb_begin(uint32_t p, const void *b)
{ (void)p; (void)b; }
static inline void car_ack_timestamp_usb_end(uint32_t p, int s)
{ (void)p; (void)s; }
static inline void car_ack_timestamp_http_done(int s) { (void)s; }
static inline void car_ack_timestamp_hid_stage(int f) { (void)f; }
static inline void car_ack_timestamp_rx(const struct pbuf *p) { (void)p; }
static inline void car_ack_timestamp_queue(uint32_t p, uint32_t i,
	uint32_t r, uint32_t f, uint32_t b)
{ (void)p; (void)i; (void)r; (void)f; (void)b; }
#endif
#endif
