/* Observation only. Internal call sites/registers/layout are hash pinned by
 * patch_carplay_nav_diag.py. No allocation, sleep, or UART in producer hooks.
 * Queue walks run only while the ORIGINAL context semaphore is held; reporting
 * consumes copied records and never dereferences a live/freed jitter context. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "audio_diag_log.h"

#ifndef CONFIG_AUDIO_NAV_PLAYOUT
#define CONFIG_AUDIO_NAV_PLAYOUT 0
#endif

#define NAV_SLOTS 4U
#define NAV_EVENTS 16U
#define NAV_PERIOD_US 5000000U
#define NAV_WALK_LIMIT 256U

struct nav_queue {
    uint32_t enc_n, enc_span, enc_first, enc_end, enc_holes;
    uint32_t pcm_n, pcm_samples, pcm_span, pcm_first, pcm_end, pcm_holes;
    uint32_t busy, capacity, read_ts, late, gaps, skips, rebuffers, rebuffer, truncated;
};
struct nav_slot {
    void *ctx;
    uint32_t sid, rate, latency, frame, sample_bytes;
    uint32_t phase, phase_since, active, stage_start[26], wait_max[26];
    uint32_t wake_us, decode_start, decode_end, decode_calls, decode_errors;
    uint32_t decode_last, decode_max, decode_ts, decode_zero, decode_samples_ptr;
    uint32_t signal_us, waiting_signal, signal_wake_max, signals, gate_calls, expected_ts, gate_head_ts;
    uint32_t packets, last_packet_us, packet_gap_max, seq, rtp_ts, missing, reorder, ts_jumps;
    uint32_t put_ok, put_fail, node_calls, node_fail;
    uint32_t reads, last_read_us, read_gap_max, zero_bytes[4];
    uint32_t underruns, rebuffer_start, recovering, recovery_count, recovery_last, recovery_max;
    uint32_t purge_before, purge_calls, purge_nodes;
    uint32_t last_period, last_gap_event, initialized;
    uint32_t pending_gap_id, pending_gap_samples, pending_gap_cursor;
};
struct nav_event {
    uint32_t id, at_us, trigger_us, kind, qvalid, missing, cursor;
    struct nav_queue q;
    struct nav_slot s;
};
static struct nav_slot slots[NAV_SLOTS];
static struct nav_event events[NAV_EVENTS];
static uint32_t head, tail, lost_events, lost_slots, next_sid, next_event;

static uint32_t field(const void *p, unsigned offset)
{
    uint32_t v; memcpy(&v, (const uint8_t *)p + offset, 4); return v;
}
static void *pointer(const void *p, unsigned offset)
{
    return (void *)(uintptr_t)field(p, offset);
}
static struct nav_slot *find(void *ctx)
{
    unsigned i;
    if(!ctx)return NULL;
    for (i=0;i<NAV_SLOTS;i++) if (slots[i].ctx==ctx) return &slots[i];
    return NULL;
}
/* Native queues are singly traversable via node+0; do NOT infer number of
 * playable samples from timestamp span. Holes inflate span, not sample sum. */
static void queue_snapshot(void *ctx, struct nav_queue *q)
{
    unsigned which;
    uint32_t sample_bytes=field(ctx,0x188), frame=field(ctx,0x160);
    memset(q,0,sizeof(*q));
    q->busy=field(ctx,0x14c);q->capacity=field(ctx,0x148);
    q->read_ts=field(ctx,0x1b8);q->late=field(ctx,0x1cc);
    q->gaps=field(ctx,0x1d0);q->skips=field(ctx,0x1d4);q->rebuffers=field(ctx,0x1d8);
    q->rebuffer=((const uint8_t *)ctx)[0x1c9];
    for (which=0;which<2;which++) {
        void *p=pointer(ctx,which?0x138:0x140);
        uint32_t n=0, first=0, end=0, sum=0, holes=0;
        while (p && n<NAV_WALK_LIMIT) {
            uint32_t ts=field(p,12);
            uint32_t count=which?(sample_bytes?field(p,0x5c8)/sample_bytes:0):frame;
            if (!n) first=ts;
            else if (ts!=end) holes++;
            end=ts+count;sum+=count;n++;p=pointer(p,0);
        }
        if(p)q->truncated|=1U<<which;
        if(which){q->pcm_n=n;q->pcm_samples=sum;q->pcm_span=n?end-first:0;
            q->pcm_first=first;q->pcm_end=end;q->pcm_holes=holes;}
        else {q->enc_n=n;q->enc_span=n?end-first:0;q->enc_first=first;
            q->enc_end=end;q->enc_holes=holes;}
    }
}
/* kind: 1 init, 2 periodic, 3 underrun, 4 recovered, 5 TS gap,
 * 6 purge-before, 7 purge-after, 8 close. Called without our critical section. */
static void emit(struct nav_slot *s, uint32_t kind, uint32_t now,
                 uint32_t qvalid, uint32_t missing, uint32_t cursor)
{
    struct nav_queue q;
    if(qvalid)queue_snapshot(s->ctx,&q);
    taskENTER_CRITICAL();
    if(head-tail==NAV_EVENTS){lost_events++;taskEXIT_CRITICAL();return;}
    struct nav_event *e=&events[head%NAV_EVENTS];
    e->id=++next_event;e->trigger_us=now;e->kind=kind;e->qvalid=qvalid;
    e->missing=missing;e->cursor=cursor;e->s=*s;e->at_us=hal_read_curtime_us();
    if(qvalid)e->q=q;else memset(&e->q,0,sizeof(e->q));
    head++;taskEXIT_CRITICAL();
}
static void maximum(uint32_t *v,uint32_t n){if(n>*v)*v=n;}

void carbox_nav_before(void *ctx, uint32_t id, const uint32_t *r)
{
    uint32_t now, kind=0, missing=0, cursor=0;
    struct nav_slot *s;
    taskENTER_CRITICAL();
    now=hal_read_curtime_us();
    s=find(ctx);
    if(!s){taskEXIT_CRITICAL();return;}
    s->stage_start[id]=now;s->active|=1U<<id;
    if(id<=7 || id==22){s->phase=id;s->phase_since=now;}
    switch(id){
    case 1: s->waiting_signal=0;break;
    case 4:
        s->decode_start=now;s->decode_calls++;
        s->decode_samples_ptr=r[3];
        s->decode_ts=r[2]?field((void *)(uintptr_t)r[2],12):0;
        break;
    case 8:
        if(s->reads)maximum(&s->read_gap_max,now-s->last_read_us);
        s->last_read_us=now;s->reads++;break;
    case 9:
        if(now-s->last_period>=NAV_PERIOD_US){s->last_period=now;kind=2;}
        break;
    case 10: case 11: case 12: case 13:
        s->zero_bytes[id-10]+=r[2];
        if((id==11 || id==13) && now-s->last_gap_event>=1000000U){
            s->last_gap_event=now;s->pending_gap_id=id;
            s->pending_gap_samples=s->sample_bytes?r[2]/s->sample_bytes:0;
            s->pending_gap_cursor=id==11?r[8]-r[5]:r[5];
        }
        break;
    case 14:
        s->underruns++;s->recovering=1;s->rebuffer_start=now;
        kind=3;cursor=r[5];missing=r[8]-r[5];break;
    case 15: {
        const uint8_t *node=(const uint8_t *)(uintptr_t)r[5];
        uint16_t seq=0;uint32_t ts=0;
        if(node){memcpy(&seq,node+10,2);ts=field(node,12);}
        if(s->packets){
            uint16_t delta=(uint16_t)(seq-s->seq);
            maximum(&s->packet_gap_max,now-s->last_packet_us);
            if(delta>0 && delta<0x8000){
                s->missing+=delta-1U;
                if(ts-s->rtp_ts!=(uint32_t)delta*s->frame)s->ts_jumps++;
            }else s->reorder++;
        }
        s->seq=seq;s->rtp_ts=ts;s->last_packet_us=now;s->packets++;break;
    }
    case 16: if(r[5]==0)s->put_ok++;else s->put_fail++;break;
    case 17: case 18:
        s->signals++;s->signal_us=now;
        if(s->phase==1 && !s->waiting_signal)s->waiting_signal=now;
        break;
    case 19:
        s->gate_calls++;s->expected_ts=r[6];
        s->gate_head_ts=pointer(ctx,0x140)?field(pointer(ctx,0x140),12):0;
        break;
    case 20: case 21:
        s->purge_before=field(ctx,0x14c);s->purge_calls++;kind=6;break;
    case 25:
        if(s->recovering){
            s->recovery_last=now-s->rebuffer_start;
            maximum(&s->recovery_max,s->recovery_last);
            s->recovery_count++;s->recovering=0;
        }
        kind=4;break;
    default:break;
    }
    taskEXIT_CRITICAL();
    if(kind)emit(s,kind,now,1,missing,cursor);
}
void carbox_nav_after(void *ctx, uint32_t id, const uint32_t *r)
{
    uint32_t now, kind=0, missing=0, cursor=0;
    struct nav_slot *s;
    taskENTER_CRITICAL();now=hal_read_curtime_us();s=find(ctx);
    if(!s){taskEXIT_CRITICAL();return;}
    if(s->active&(1U<<id))maximum(&s->wait_max[id],now-s->stage_start[id]);
    s->active&=~(1U<<id);
    if(id<=7 || id==22){s->phase=100+id;s->phase_since=now;}
    if(id==1){
        s->wake_us=now;
        if(s->waiting_signal)maximum(&s->signal_wake_max,now-s->waiting_signal);
        s->waiting_signal=0;
    }else if(id==4){
        s->decode_end=now;s->decode_last=now-s->decode_start;
        maximum(&s->decode_max,s->decode_last);
        if(r[0] && (int32_t)r[0]!=-6750)s->decode_errors++;
        if(s->decode_samples_ptr && !field((void *)(uintptr_t)s->decode_samples_ptr,0))s->decode_zero++;
    }else if(id==20 || id==21){
        uint32_t after=field(ctx,0x14c);
        if(s->purge_before>after)s->purge_nodes+=s->purge_before-after;
        kind=7;
    }
    if(s->pending_gap_id==id){
        kind=5;missing=s->pending_gap_samples;cursor=s->pending_gap_cursor;
        s->pending_gap_id=0;
    }
    taskEXIT_CRITICAL();
    if(kind)emit(s,kind,now,1,missing,cursor);
}
/* before() counts requested fill; the continuity shim subtracts substituted
 * PCM so NAV_PCM retains its meaning: bytes actually replaced by zero. */
void carbox_nav_diag_concealed(void *ctx,unsigned reason,uint32_t bytes)
{
    struct nav_slot *s;
    if(reason!=11 && reason!=13)return;
    taskENTER_CRITICAL();s=find(ctx);
    if(s){uint32_t *n=&s->zero_bytes[reason-10];*n=*n>=bytes?*n-bytes:0;}
    taskEXIT_CRITICAL();
}
uint32_t carbox_nav_session_id(void *ctx)
{
    uint32_t sid;struct nav_slot *s;
    taskENTER_CRITICAL();s=find(ctx);sid=s?s->sid:0;taskEXIT_CRITICAL();
    return sid;
}
extern void carbox_nav_rx_report(void);
extern void carbox_nav_udp_report(void);
void carbox_nav_diag_open(void *ctx)
{
    if(ctx && (field(ctx,0x150)==44100U || field(ctx,0x150)==48000U) && field(ctx,0x160)==480U){
        unsigned i;struct nav_slot *s=NULL;uint32_t now=hal_read_curtime_us();
        taskENTER_CRITICAL();
        for(i=0;i<NAV_SLOTS;i++)if(!slots[i].ctx){s=&slots[i];break;}
        if(s){memset(s,0,sizeof(*s));s->ctx=ctx;s->sid=++next_sid;s->rate=field(ctx,0x150);
            s->latency=field(ctx,0x1b4);s->frame=field(ctx,0x160);s->sample_bytes=field(ctx,0x188);
            s->last_period=now;s->initialized=now;
        }else lost_slots++;
        taskEXIT_CRITICAL();if(s)emit(s,1,now,0,0,0);
    }
}
void carbox_nav_diag_close(void *ctx)
{
    struct nav_slot *s;
    /* Native Free joins decoder before reclaiming context members. Do not
     * walk queues here or retain any pointer for the deferred reporter. */
    taskENTER_CRITICAL();s=find(ctx);taskEXIT_CRITICAL();
    if(s){emit(s,8,hal_read_curtime_us(),0,0,0);
        taskENTER_CRITICAL();memset(s,0,sizeof(*s));taskEXIT_CRITICAL();}
}
extern int __real_RTPJitterBufferGetFreeNode(void *,void **);
int __wrap_RTPJitterBufferGetFreeNode(void *ctx,void **node)
{
    int rc=__real_RTPJitterBufferGetFreeNode(ctx,node);struct nav_slot *s;
    taskENTER_CRITICAL();s=find(ctx);
    if(s){s->node_calls++;if(rc || !node || !*node)s->node_fail++;}
    taskEXIT_CRITICAL();return rc;
}
static uint32_t age(uint32_t now,uint32_t then){return then?now-then:UINT32_MAX;}
#define U(x) ((unsigned long)(x))
extern void carbox_nav_playout_report(void);
void carbox_audio_nav_report(void)
{
    unsigned budget=NAV_EVENTS;
    carbox_nav_rx_report();
    carbox_nav_udp_report();
    static uint32_t reported_lost,reported_slots,reported_log,announced;
    unsigned i;
    if(!announced){
        carbox_audio_diag_log("[NAV_DIAG] enabled observation_only=%u purge_head_fix=%u rates=44100/48000 frame=480 period_ms=5000 event_slots=16 reporter=wlan_rx_diag\n", (unsigned)!(CONFIG_AUDIO_NAV_PURGE_FIX || CONFIG_AUDIO_NAV_PLAYOUT), (unsigned)CONFIG_AUDIO_NAV_PURGE_FIX);
        announced=1;
    }
    carbox_nav_playout_report();
    /* This remains available even if RX, decoder or PCM reader is blocked and
     * cannot produce a queue snapshot. No live context dereferences here. */
    for(i=0;i<NAV_SLOTS;i++){
        struct nav_slot s;uint32_t now;
        taskENTER_CRITICAL();s=slots[i];now=hal_read_curtime_us();taskEXIT_CRITICAL();
        if(!s.ctx)continue;
        carbox_audio_diag_log("[NAV_LIVE] session=%lu at_us=%lu phase=%lu phase_age_us=%lu active=0x%lx rx/read/decode_age_us=%lu/%lu/%lu\n",
            U(s.sid),U(now),U(s.phase),U(age(now,s.phase_since)),U(s.active),
            U(age(now,s.last_packet_us)),U(age(now,s.last_read_us)),U(age(now,s.decode_end)));
    }
    while(budget--){
        struct nav_event e;struct nav_slot *s=&e.s;struct nav_queue *q=&e.q;
        taskENTER_CRITICAL();
        if(head==tail){taskEXIT_CRITICAL();break;}
        e=events[tail%NAV_EVENTS];tail++;taskEXIT_CRITICAL();
        carbox_audio_diag_log("[NAV_EVT] id=%lu session=%lu ctx=%p kind=%lu at_us=%lu trigger_us=%lu rate=%lu latency_ms=%lu qvalid=%lu missing_samples=%lu cursor=%lu\n",
            U(e.id),U(s->sid),s->ctx,U(e.kind),U(e.at_us),U(e.trigger_us),U(s->rate),U(s->latency),U(e.qvalid),U(e.missing),U(e.cursor));
        if(e.qvalid){
            carbox_audio_diag_log("[NAV_Q] id=%lu enc_n=%lu span=%lu ts=%lu..%lu holes=%lu pcm_n=%lu samples=%lu span=%lu ts=%lu..%lu holes=%lu\n",
                U(e.id),U(q->enc_n),U(q->enc_span),U(q->enc_first),U(q->enc_end),U(q->enc_holes),
                U(q->pcm_n),U(q->pcm_samples),U(q->pcm_span),U(q->pcm_first),U(q->pcm_end),U(q->pcm_holes));
            carbox_audio_diag_log("[NAV_PLAY] id=%lu read_ts=%lu busy/cap=%lu/%lu rebuffer=%lu native_late/gap/skip/rebuf=%lu/%lu/%lu/%lu walk_truncated=%lu\n",
                U(e.id),U(q->read_ts),U(q->busy),U(q->capacity),U(q->rebuffer),U(q->late),U(q->gaps),U(q->skips),U(q->rebuffers),U(q->truncated));
        }
        carbox_audio_diag_log("[NAV_RX] id=%lu packets=%lu seq=%lu ts=%lu age_us=%lu gap_max_us=%lu missing/reorder/tsjump=%lu/%lu/%lu put_ok/fail=%lu/%lu node_fail=%lu\n",
            U(e.id),U(s->packets),U(s->seq),U(s->rtp_ts),U(age(e.at_us,s->last_packet_us)),U(s->packet_gap_max),
            U(s->missing),U(s->reorder),U(s->ts_jumps),U(s->put_ok),U(s->put_fail),U(s->node_fail));
        carbox_audio_diag_log("[NAV_DEC] id=%lu calls/err/zero=%lu/%lu/%lu last/max_us=%lu/%lu end_age_us=%lu phase=%lu age_us=%lu decode_ts=%lu\n",
            U(e.id),U(s->decode_calls),U(s->decode_errors),U(s->decode_zero),U(s->decode_last),U(s->decode_max),
            U(age(e.at_us,s->decode_end)),U(s->phase),U(age(e.at_us,s->phase_since)),U(s->decode_ts));
        carbox_audio_diag_log("[NAV_WAIT] id=%lu max_us cond/ctx/node/relock=%lu/%lu/%lu/%lu mutex_dec/rx/read=%lu/%lu/%lu read/rx=%lu/%lu signal_wake=%lu\n",
            U(e.id),U(s->wait_max[1]),U(s->wait_max[2]),U(s->wait_max[3]),U(s->wait_max[5]),
            U(s->wait_max[22]),U(s->wait_max[23]),U(s->wait_max[24]),U(s->wait_max[8]),U(s->wait_max[15]),U(s->signal_wake_max));
        carbox_audio_diag_log("[NAV_GATE] id=%lu checks=%lu expected/head_ts=%lu/%lu wake_age_us=%lu signal_age_us=%lu signals=%lu purge_calls/nodes=%lu/%lu\n",
            U(e.id),U(s->gate_calls),U(s->expected_ts),U(s->gate_head_ts),U(age(e.at_us,s->wake_us)),U(age(e.at_us,s->signal_us)),
            U(s->signals),U(s->purge_calls),U(s->purge_nodes));
        carbox_audio_diag_log("[NAV_PCM] id=%lu reads=%lu gap_max_us=%lu zero_bytes_wait/future/under/gap=%lu/%lu/%lu/%lu underruns=%lu recovery_n/last/max_us=%lu/%lu/%lu\n",
            U(e.id),U(s->reads),U(s->read_gap_max),U(s->zero_bytes[0]),U(s->zero_bytes[1]),U(s->zero_bytes[2]),U(s->zero_bytes[3]),
            U(s->underruns),U(s->recovery_count),U(s->recovery_last),U(s->recovery_max));
    }
    {uint32_t lost,full,log_skip=carbox_audio_diag_log_skipped();
    taskENTER_CRITICAL();lost=lost_events;full=lost_slots;taskEXIT_CRITICAL();
    if(lost!=reported_lost || full!=reported_slots || log_skip!=reported_log){
        carbox_audio_diag_log("[NAV_LOST] events=%lu sessions=%lu log_skip=%lu\n",U(lost),U(full),U(log_skip));reported_lost=lost;reported_slots=full;reported_log=log_skip;
    }}
}
