#ifndef CARBOX_CAR_ACK_RESPONSE_CACHE_H
#define CARBOX_CAR_ACK_RESPONSE_CACHE_H

#include <stdint.h>

/* Replacement target for the one no-body event response call patched in the
 * derived Accessory2 archive.  The two arguments match AirPlayEvent_SendResponse
 * and are deliberately ignored for this validated empty 200 response path. */
void carbox_airplay_event_send_fast_response(const void *body,
					      uint32_t body_length);

void carbox_car_ack_response_cache_report(uint32_t sequence);

#endif /* CARBOX_CAR_ACK_RESPONSE_CACHE_H */
