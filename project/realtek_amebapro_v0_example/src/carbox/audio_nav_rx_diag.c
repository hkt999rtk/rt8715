/* Hash-pinned _MainAltAudioThread call-site evidence. Producers never allocate
 * or print. Saved register ABI: r[0..12], LR, original caller stack at r+14.
 * After-call r0 contains return value; remaining saved registers are incoming.
 * Raw sequence evidence is collected BEFORE native validation/byte swapping. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "lwip/sockets.h"
#include "audio_diag_log.h"

#define APP_RX_SLOTS 4U
#define APP_RX_PERIOD_US 5000000U
/* Reasons describe native frees, not speculative loss inferred from sequence. */
enum app_discard { APP_OTHER, APP_RECV_ERROR, APP_SHORT, APP_CRYPTO_SHORT,
    APP_AUTH, APP_LENGTH, APP_QUEUE, APP_REASONS };
struct app_rx_slot {
    void *app;
    uint32_t sid, port, fd, active, dirty, last_report;
    uint32_t calls, received, bytes, headers, invalid, oversize;
    uint32_t first_seq, high_seq, last_seq, last_ts, have_seq, missing, reorder, duplicate;
    uint32_t last_rx, have_rx, gap_max, recv_max, select_max, select_errors;
    uint32_t recv_errors, verify_calls, auth_errors, length_errors, queued, queue_errors;
    uint32_t discarded[APP_REASONS], last_discard_seq, last_discard_reason;
    int32_t last_error;
    uint32_t pending, reason, pending_seq, recv_start, select_start;
    uint32_t buffer, capacity, length_ptr, verify_error_ptr, verify_node, decrypted;
};
static struct app_rx_slot app_slots[APP_RX_SLOTS];
static uint32_t app_lost_slots;
extern uint32_t carbox_nav_session_id(void *ctx);
extern void carbox_nav_udp_register(uint16_t port, uint32_t sid);
extern void carbox_nav_udp_unregister(uint16_t port);

static uint32_t app_field(const void *p, unsigned off)
{ uint32_t v; memcpy(&v, (const uint8_t *)p+off, sizeof(v)); return v; }
static struct app_rx_slot *app_find(void *app)
{
    unsigned i;
    for(i=0;i<APP_RX_SLOTS;i++)
        if(app_slots[i].active && app_slots[i].app==app) return &app_slots[i];
    return NULL;
}
static uint16_t app_port(int fd)
{
    struct sockaddr_storage address;
    socklen_t length=sizeof(address);
    memset(&address,0,sizeof(address));
    if(lwip_getsockname(fd,(struct sockaddr *)&address,&length))return 0;
    if(address.ss_family==AF_INET)
        return lwip_ntohs(((struct sockaddr_in *)&address)->sin_port);
#if LWIP_IPV6
    if(address.ss_family==AF_INET6)
        return lwip_ntohs(((struct sockaddr_in6 *)&address)->sin6_port);
#endif
    return 0;
}
static void app_start(void *app,uint32_t now)
{
    unsigned i;
    uint32_t sid=carbox_nav_session_id((uint8_t *)app+48);
    uint32_t fd=app_field(app,28);
    uint16_t port;
    if(!sid)return; /* Other alt formats do not have a navigation session. */
    port=app_port((int)fd); /* May enter TCPIP: always outside critical section. */
    taskENTER_CRITICAL();
    for(i=0;i<APP_RX_SLOTS;i++)if(!app_slots[i].active && !app_slots[i].dirty)break;
    if(i==APP_RX_SLOTS)app_lost_slots++;
    else {
        struct app_rx_slot *s=&app_slots[i];
        memset(s,0,sizeof(*s));s->app=app;s->sid=sid;s->fd=fd;s->port=port;
        s->active=1;s->dirty=1;s->last_report=now;
    }
    taskEXIT_CRITICAL();
    if(i<APP_RX_SLOTS && port)carbox_nav_udp_register(port,sid);
}

void carbox_nav_app_before(void *app,uint32_t id,const uint32_t *r)
{
    struct app_rx_slot *s;
    uint32_t now=hal_read_curtime_us();
    uint16_t closed_port=0;
    if(id==1){app_start(app,now);return;}
    taskENTER_CRITICAL();s=app_find(app);
    if(!s){taskEXIT_CRITICAL();return;}
    if(id==2){
        s->calls++;s->recv_start=now;s->buffer=r[1];s->capacity=r[2];s->length_ptr=r[3];
        s->pending=1;s->pending_seq=UINT32_MAX;s->reason=APP_OTHER;
    } else if(id==3){
        s->verify_calls++;s->verify_error_ptr=r[3];
        s->verify_node=r[26];s->decrypted=r[27];
    } else if(id==4){
        if(s->pending){
            s->discarded[s->reason]++;s->last_discard_seq=s->pending_seq;
            s->last_discard_reason=s->reason;s->pending=0;
        }
    } else if(id==6){closed_port=(uint16_t)s->port;s->active=0;s->app=NULL;}
    else if(id==7)s->select_start=now;
    s->dirty=1;taskEXIT_CRITICAL();
    if(closed_port)carbox_nav_udp_unregister(closed_port);
}

void carbox_nav_app_after(void *app,uint32_t id,const uint32_t *r)
{
    struct app_rx_slot *s;
    struct {uint32_t sid, buffer, capacity, length_ptr, verify_error_ptr, verify_node;} input;
    uint32_t now=hal_read_curtime_us(),length=0,seq=0,ts=0,valid=0,expected=0;
    int32_t error=(int32_t)r[0];
    /* Snapshot pointers, then inspect bounded caller-owned buffers outside the
     * critical section. Only this app's RX task updates pending call state. */
    taskENTER_CRITICAL();s=app_find(app);
    if(!s){taskEXIT_CRITICAL();return;}
    input.sid=s->sid;input.buffer=s->buffer;input.capacity=s->capacity;
    input.length_ptr=s->length_ptr;input.verify_error_ptr=s->verify_error_ptr;
    input.verify_node=s->verify_node;taskEXIT_CRITICAL();
    if(id==2 && !error && input.length_ptr){
        length=app_field((void *)(uintptr_t)input.length_ptr,0);
        if(length>=12 && length<=input.capacity && input.buffer){
            const uint8_t *p=(const uint8_t *)(uintptr_t)input.buffer;
            valid=(p[0]>>6)==2;
            seq=((uint32_t)p[2]<<8)|p[3];
            ts=((uint32_t)p[4]<<24)|((uint32_t)p[5]<<16)|((uint32_t)p[6]<<8)|p[7];
        }
    } else if(id==3){
        error=input.verify_error_ptr?(int32_t)app_field((void *)(uintptr_t)input.verify_error_ptr,0):0;
        if(input.verify_node)expected=app_field((void *)(uintptr_t)input.verify_node,0x5c8);
    }
    taskENTER_CRITICAL();s=app_find(app);
    if(!s || s->sid!=input.sid){taskEXIT_CRITICAL();return;}
    if(id==2){
        uint32_t elapsed=now-s->recv_start;
        if(elapsed>s->recv_max)s->recv_max=elapsed;
        if(error){s->recv_errors++;s->last_error=error;s->reason=APP_RECV_ERROR;}
        else {
            s->received++;s->bytes+=length;
            if(s->have_rx && now-s->last_rx>s->gap_max)s->gap_max=now-s->last_rx;
            s->last_rx=now;s->have_rx=1;
            if(length>input.capacity){s->oversize++;s->reason=APP_OTHER;}
            else if(length<12)s->reason=APP_SHORT;
            else {
                if(valid){
                    uint16_t delta=(uint16_t)(seq-s->high_seq);
                    s->headers++;s->pending_seq=seq;s->last_seq=seq;s->last_ts=ts;
                    if(!s->have_seq){s->first_seq=seq;s->have_seq=1;s->high_seq=seq;}
                    else if(!delta)s->duplicate++;
                    else if(delta<0x8000U){s->missing+=delta-1U;s->high_seq=seq;}
                    else s->reorder++;
                }else s->invalid++;
                if(((uint8_t *)app)[1432] && length<36)s->reason=APP_CRYPTO_SHORT;
            }
        }
    }else if(id==3){
        if(error){s->auth_errors++;s->last_error=error;s->reason=APP_AUTH;}
        else if(expected<24 || s->decrypted+r[0]!=expected-24){
            s->length_errors++;s->reason=APP_LENGTH;
        }
    }else if(id==5){
        if(error){s->queue_errors++;s->last_error=error;s->reason=APP_QUEUE;}
        else {s->queued++;s->pending=0;}
    }else if(id==7){
        uint32_t elapsed=now-s->select_start;
        if(elapsed>s->select_max)s->select_max=elapsed;
        if(error<0){s->select_errors++;s->last_error=error;}
    }
    s->dirty=1;taskEXIT_CRITICAL();
}

/* Only the shared low-priority diagnostic reporter calls this. Counts are
 * cumulative per SID; closed sessions are retained until a final snapshot. */
void carbox_nav_rx_report(void)
{
    unsigned i;uint32_t now=hal_read_curtime_us(),lost;
    static uint32_t reported_lost;
    taskENTER_CRITICAL();lost=app_lost_slots;taskEXIT_CRITICAL();
    if(lost!=reported_lost){
        carbox_audio_diag_log("[NAV_APP_LOST] slots=%lu log_skipped=%lu\n",
            (unsigned long)lost,(unsigned long)carbox_audio_diag_log_skipped());
        reported_lost=lost;
    }
    for(i=0;i<APP_RX_SLOTS;i++){
        struct app_rx_slot s;
        taskENTER_CRITICAL();
        if(!app_slots[i].dirty || (app_slots[i].active && now-app_slots[i].last_report<APP_RX_PERIOD_US)){
            taskEXIT_CRITICAL();continue;
        }
        s=app_slots[i];app_slots[i].dirty=0;app_slots[i].last_report=now;
        taskEXIT_CRITICAL();
#define U(x) ((unsigned long)(x))
        /* Keep even maximum-width counter lines within the logger's 160-byte
         * chunk budget; repeat SID and port for correlation after interleaving. */
        carbox_audio_diag_log("[NAV_APP_RX] session=%lu port=%lu closed=%lu calls/recv/header=%lu/%lu/%lu bytes=%lu\n",
            U(s.sid),U(s.port),U(!s.active),U(s.calls),U(s.received),U(s.headers),U(s.bytes));
        carbox_audio_diag_log("[NAV_APP_SEQ] session=%lu port=%lu first/last/high=%lu/%lu/%lu missing/reorder/dup=%lu/%lu/%lu\n",
            U(s.sid),U(s.port),U(s.first_seq),U(s.last_seq),U(s.high_seq),U(s.missing),U(s.reorder),U(s.duplicate));
        carbox_audio_diag_log("[NAV_APP_TIME] session=%lu port=%lu at/rtp_ts=%lu/%lu gap/recv/select_max_us=%lu/%lu/%lu\n",
            U(s.sid),U(s.port),U(now),U(s.last_ts),U(s.gap_max),U(s.recv_max),U(s.select_max));
        carbox_audio_diag_log("[NAV_APP_CHECK] session=%lu port=%lu select_err/invalid/oversize=%lu/%lu/%lu queued=%lu\n",
            U(s.sid),U(s.port),U(s.select_errors),U(s.invalid),U(s.oversize),U(s.queued));
        carbox_audio_diag_log("[NAV_APP_VERIFY] session=%lu port=%lu calls/auth/length=%lu/%lu/%lu recv_err/queue_err=%lu/%lu\n",
            U(s.sid),U(s.port),U(s.verify_calls),U(s.auth_errors),U(s.length_errors),U(s.recv_errors),U(s.queue_errors));
        carbox_audio_diag_log("[NAV_APP_DROP] session=%lu port=%lu other/recv/short/crypto_short=%lu/%lu/%lu/%lu\n",
            U(s.sid),U(s.port),U(s.discarded[APP_OTHER]),U(s.discarded[APP_RECV_ERROR]),U(s.discarded[APP_SHORT]),U(s.discarded[APP_CRYPTO_SHORT]));
        carbox_audio_diag_log("[NAV_APP_DROP2] session=%lu port=%lu auth/length/queue=%lu/%lu/%lu last_seq/reason=%lu/%lu error=%ld\n",
            U(s.sid),U(s.port),U(s.discarded[APP_AUTH]),U(s.discarded[APP_LENGTH]),U(s.discarded[APP_QUEUE]),
            U(s.last_discard_seq),U(s.last_discard_reason),(long)s.last_error);
#undef U
    }
}
