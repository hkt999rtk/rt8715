#ifndef WCAM_NET_H
#define WCAM_NET_H
#include <stddef.h>
#include <stdint.h>
struct wcam_net_frame {
    uint8_t *data;
    size_t length;
    uint32_t timestamp, generation;
    uint64_t received_ms;
};
uint64_t wcam_net_now_ms(void);
int wcam_net_start(void);
int wcam_net_ready(void);
/* Stamp the frame at capture. Zero means the station is offline. */
uint32_t wcam_net_tx_generation(void);
int wcam_net_send(const uint8_t *, size_t, uint32_t timestamp, uint32_t generation);
int wcam_net_take_idr(void);
void wcam_net_rx_active(uint32_t generation);
void wcam_net_rx_request_idr(void);
int wcam_net_frame_get(struct wcam_net_frame *,unsigned wait_ms);
void wcam_net_frame_release(struct wcam_net_frame *);
#endif
