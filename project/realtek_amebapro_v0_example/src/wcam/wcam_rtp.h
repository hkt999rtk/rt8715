#ifndef WCAM_RTP_H
#define WCAM_RTP_H
#include <stdint.h>
#include <stddef.h>
#define WCAM_RTP_MTU 1200u
#define WCAM_RTP_WINDOW 16u
#define WCAM_RTP_PT 96u
#define WCAM_RTP_REORDER_MS 20u
#define WCAM_RTP_FRAME_MS 100u
/* Prefix only imported symbols to avoid SDK archive symbol collisions. */
#define rtp_h264_encode wcam_ir_h264_encode
#define rtp_h264_decode wcam_ir_h264_decode
#define rtp_h264_annexb_nalu wcam_ir_h264_annexb_nalu
#define rtp_packet_serialize wcam_ir_packet_serialize
#define rtp_packet_serialize_header wcam_ir_packet_serialize_header
#define rtp_packet_deserialize wcam_ir_packet_deserialize
/* Called synchronously. send must consume/copy bytes before returning. */
typedef int (*wcam_packet_fn)(void *,const uint8_t *,size_t);
/* Return the next writable AU buffer; returned storage has the same capacity. */
typedef uint8_t *(*wcam_frame_fn)(void *,uint8_t *,size_t,uint32_t);
struct wcam_rtp_tx {
    void *packer; uint32_t ssrc, timestamp, packets, octets;
    wcam_packet_fn send; void *arg; uint8_t packet[WCAM_RTP_MTU];
};
struct wcam_rtp_slot { uint16_t seq,len; uint8_t data[WCAM_RTP_MTU]; };
struct wcam_rtp_rx {
    void *unpacker; uint8_t *frame; size_t capacity, used;
    wcam_frame_fn ready; void *arg;
    uint32_t ssrc, timestamp, frame_start, gap_start, last_ms;
    uint32_t received,lost,late,out_of_order,invalid,frames,dropped,highest,base_seq;
    uint32_t feedback_epoch;
    uint32_t expected_prior,received_prior;
    uint32_t last_sr,last_sr_ms,jitter; uint32_t transit;
    uint16_t next_seq; unsigned flags;
    int have_ssrc,have_ts,need_idr,damaged,gap,have_transit;
    struct wcam_rtp_slot slots[WCAM_RTP_WINDOW];
};
int wcam_rtp_tx_init(struct wcam_rtp_tx *,uint32_t,uint16_t,wcam_packet_fn,void *);
void wcam_rtp_tx_destroy(struct wcam_rtp_tx *);
int wcam_rtp_send(struct wcam_rtp_tx *,const uint8_t *,size_t,uint32_t);
int wcam_rtp_rx_init(struct wcam_rtp_rx *,uint8_t *,size_t,wcam_frame_fn,void *);
void wcam_rtp_rx_destroy(struct wcam_rtp_rx *);
void wcam_rtp_receive(struct wcam_rtp_rx *,const uint8_t *,size_t,uint32_t);
void wcam_rtp_poll(struct wcam_rtp_rx *,uint32_t);
void wcam_rtp_resync(struct wcam_rtp_rx *);
void wcam_rtp_request_idr(struct wcam_rtp_rx *);
size_t wcam_rtcp_report(uint8_t *,size_t,uint32_t,struct wcam_rtp_rx *,int,uint32_t);
size_t wcam_rtcp_sender(uint8_t *,size_t,const struct wcam_rtp_tx *,uint32_t);
int wcam_rtcp_feedback(const uint8_t *,size_t,uint32_t);
void wcam_rtcp_receive_sr(struct wcam_rtp_rx *,const uint8_t *,size_t,uint32_t);
#endif
