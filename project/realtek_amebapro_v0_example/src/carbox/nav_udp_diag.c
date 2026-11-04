/* No allocation, logging, or payload retention in receive callbacks. */
#include "nav_udp_diag.h"
#if CONFIG_AUDIO_NAV_DIAG
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "lwip/pbuf.h"
#include "audio_diag_log.h"
#define UDP_NAV_SLOTS 4U
#define UDP_NAV_REPORT_US 5000000U
struct udp_nav_slot {
    uint32_t token, session, port, active, dirty, last_report;
    uint32_t seen, bytes, rtp, invalid_rtp, first_seq, last_seq, last_ts;
    uint32_t missing, reorder, last_us, gap_max_us;
    uint32_t result[NAV_UDP_RESULT_COUNT];
};
static struct udp_nav_slot udp_slots[UDP_NAV_SLOTS];
static uint32_t udp_token, udp_lost;
static struct udp_nav_slot *udp_find(uint32_t token)
{
    unsigned i;
    for(i=0;i<UDP_NAV_SLOTS;i++) if(udp_slots[i].token==token) return &udp_slots[i];
    return NULL;
}
void carbox_nav_udp_register(uint16_t port, uint32_t session_id)
{
    unsigned i; struct udp_nav_slot *s=NULL;
    if(!port) return;
    taskENTER_CRITICAL();
    for(i=0;i<UDP_NAV_SLOTS;i++) {
        if(udp_slots[i].active && udp_slots[i].port==port) {
            /* Idempotent registration must not reset cumulative evidence. */
            if(udp_slots[i].session!=session_id) udp_lost++;
            taskEXIT_CRITICAL();return;
        }
        if(!udp_slots[i].active && !udp_slots[i].dirty && !s) s=&udp_slots[i];
    }
    if(!s) {
        for(i=0;i<UDP_NAV_SLOTS;i++) if(!udp_slots[i].active) {s=&udp_slots[i];break;}
        udp_lost++;
    }
    if(s) {
        memset(s,0,sizeof(*s));
        if(!++udp_token) ++udp_token;
        s->token=udp_token;s->session=session_id;s->port=port;s->active=1;s->dirty=1;
        s->last_report=hal_read_curtime_us();
    }
    taskEXIT_CRITICAL();
}
void carbox_nav_udp_unregister(uint16_t port)
{
    unsigned i;
    taskENTER_CRITICAL();
    for(i=0;i<UDP_NAV_SLOTS;i++) if(udp_slots[i].active && udp_slots[i].port==port) {
        udp_slots[i].active=0;udp_slots[i].dirty=1;
    }
    taskEXIT_CRITICAL();
}
uint32_t carbox_nav_udp_begin(uint16_t port, const struct pbuf *p)
{
    unsigned i; uint32_t token=0,now; uint8_t h[12]; uint16_t len=0;
    struct udp_nav_slot *s;
    taskENTER_CRITICAL();
    for(i=0;i<UDP_NAV_SLOTS;i++) if(udp_slots[i].active && udp_slots[i].port==port) {
        token=udp_slots[i].token;break;
    }
    taskEXIT_CRITICAL();
    if(!token || !p) return 0;
    /* Chained pbufs are supported, and malformed short datagrams are counted. */
    if(p->tot_len>=sizeof(h)) len=pbuf_copy_partial(p,h,sizeof(h),0);
    now=hal_read_curtime_us();
    taskENTER_CRITICAL();
    s=udp_find(token);
    if(!s) {udp_lost++;taskEXIT_CRITICAL();return 0;}
    if(s->seen && now-s->last_us>s->gap_max_us) s->gap_max_us=now-s->last_us;
    s->last_us=now;s->seen++;s->bytes+=p->tot_len;s->dirty=1;
    if(len==sizeof(h) && (h[0]>>6)==2) {
        uint16_t seq=((uint16_t)h[2]<<8)|h[3];
        if(!s->rtp) s->first_seq=s->last_seq=seq;
        else {
            uint16_t delta=(uint16_t)(seq-s->last_seq);
            if(delta && delta<0x8000U) {s->missing+=delta-1U;s->last_seq=seq;}
            else s->reorder++;
        }
        s->rtp++;s->last_ts=((uint32_t)h[4]<<24)|((uint32_t)h[5]<<16)|((uint32_t)h[6]<<8)|h[7];
    } else s->invalid_rtp++;
    taskEXIT_CRITICAL();return token;
}
void carbox_nav_udp_finish(uint32_t token, enum carbox_nav_udp_result result)
{
    struct udp_nav_slot *s;
    if(!token) return;
    taskENTER_CRITICAL();s=udp_find(token);
    if(s && (unsigned)result<NAV_UDP_RESULT_COUNT) {s->result[result]++;s->dirty=1;}
    else udp_lost++;
    taskEXIT_CRITICAL();
}
void carbox_nav_udp_report(void)
{
    unsigned i; uint32_t now=hal_read_curtime_us(),lost; static uint32_t reported_lost, report_id;
    for(i=0;i<UDP_NAV_SLOTS;i++) {
        struct udp_nav_slot copy; int emit=0;
        taskENTER_CRITICAL();
        if(udp_slots[i].dirty && (!udp_slots[i].active || now-udp_slots[i].last_report>=UDP_NAV_REPORT_US)) {
            copy=udp_slots[i];udp_slots[i].dirty=0;udp_slots[i].last_report=now;emit=1;
        }
        taskEXIT_CRITICAL();
        if(emit) {
#define U(x) ((unsigned long)(x))
            uint32_t id=++report_id;
            /* Each line is <=145 bytes including CRLF even at UINT32_MAX.
             * A shared id joins the separately emitted snapshot lines. */
            carbox_audio_diag_log("[NAV_UDP] id=%lu session=%lu port=%lu closed=%lu at_us=%lu\n",
                U(id),U(copy.session),U(copy.port),U(!copy.active),U(now));
            carbox_audio_diag_log("[NAV_UDP_RX] id=%lu seen=%lu bytes=%lu rtp=%lu invalid_rtp=%lu\n",
                U(id),U(copy.seen),U(copy.bytes),U(copy.rtp),U(copy.invalid_rtp));
            carbox_audio_diag_log("[NAV_UDP_SEQ] id=%lu seq=%lu..%lu ts=%lu missing=%lu reorder=%lu gap_max_us=%lu\n",
                U(id),U(copy.first_seq),U(copy.last_seq),U(copy.last_ts),U(copy.missing),U(copy.reorder),U(copy.gap_max_us));
            carbox_audio_diag_log("[NAV_UDP_POST] id=%lu accepted=%lu no_conn=%lu invalid_mbox=%lu rcvbuf=%lu netbuf=%lu mbox=%lu\n",
                U(id),U(copy.result[NAV_UDP_ACCEPTED]),U(copy.result[NAV_UDP_NO_CONN]),U(copy.result[NAV_UDP_INVALID_MBOX]),U(copy.result[NAV_UDP_RCVBUF_FULL]),U(copy.result[NAV_UDP_NETBUF_NOMEM]),U(copy.result[NAV_UDP_MBOX_FULL]));
        }
    }
    taskENTER_CRITICAL();lost=udp_lost;taskEXIT_CRITICAL();
    if(lost!=reported_lost) {carbox_audio_diag_log("[NAV_UDP_LOST] id=0 slots_or_results=%lu\n",U(lost));reported_lost=lost;}
#undef U
}
#endif
