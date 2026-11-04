/* Audited mono AAC-ELD continuity experiment. Private layouts are guarded by
 * the archive scripts. Decoder is the sole FDK caller. Reader and cache writes
 * share the native context semaphore; no condition mutex is acquired here.
 * State is allocated once, and released only AFTER native Free joins decoder.
 */
#include <stdint.h>
#include "nav_tx_trace.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#ifndef NAV_CONTINUITY_HOST_TEST
#include "aacdecoder_lib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "audio_diag_log.h"
extern AAC_DECODER_ERROR __wrap_aacDecoder_DecodeFrame(HANDLE_AACDECODER,INT_PCM *,const INT,const UINT);
#else
#define AACDEC_CONCEAL 1
#define IS_OUTPUT_VALID(e) ((e)==0 || ((e)>=0x4000 && (e)<=0x4fff))
#endif
#ifndef NAV_ALLOC
#define NAV_ALLOC(n) calloc(1,(n))
#endif
extern int dispatch_semaphore_wait(void *,uint64_t);
extern int dispatch_semaphore_signal(void *);
extern int AudioConverterFillComplexBuffer(void *,void *,void *,uint32_t *,void *,void *);
extern void _RTPJitterBufferDiscardExcess(void *);
extern uint32_t carbox_nav_session_id(void *);
extern void carbox_nav_diag_concealed(void *,unsigned,uint32_t);
#define FRAME 480U
#define CACHE 4U
struct continuity_stats {
    uint32_t sid, rate, at_us, closed;
    uint32_t start_keep, start_pressure, start_head, start_busy;
    uint32_t gap_events, gap_skip, plc_ok, plc_fail, plc_bytes, fallback_bytes;
    uint32_t cache_full, cache_expired, last_gap_ts, last_gap_frames, plc_max_us;
    uint32_t tail_valid, tail_samples, tail_nodes, tail_encoded, tail_nonzero;
    uint32_t tail_peak, tail_mean_abs, tail_truncated;
};
struct cached_pcm { uint32_t valid, ts; int16_t pcm[FRAME]; };
struct continuity {
    void *ctx;
    uint32_t started, have_expected, expected;
    struct continuity_stats stats;
    struct cached_pcm cache[CACHE];
    int16_t scratch[FRAME];
};
static struct continuity *sessions[4];
static struct continuity_stats closed_stats[4];
static uint32_t closed_head,closed_tail,records_lost,alloc_fail;
static uint32_t word(const void *p,unsigned off)
{ uint32_t v;memcpy(&v,(const uint8_t *)p+off,4);return v; }
static void *pointer(const void *p,unsigned off)
{ return (void *)(uintptr_t)word(p,off); }
static struct continuity *find(void *ctx)
{
    unsigned i;struct continuity *s=NULL;
    /* Another session may close while this one decodes. Protect the table
     * scan itself against dereferencing that other session after free. The
     * matching session stays alive until its native decoder/reader stop. */
    taskENTER_CRITICAL();
    for(i=0;i<4;i++)if(sessions[i] && sessions[i]->ctx==ctx){s=sessions[i];break;}
    taskEXIT_CRITICAL();return s;
}
static void lock(void *ctx){dispatch_semaphore_wait(pointer(ctx,0),UINT64_MAX);}
static void unlock(void *ctx){dispatch_semaphore_signal(pointer(ctx,0));}
void carbox_nav_continuity_open(void *ctx)
{
    struct continuity *s;unsigned i;
    if(!ctx || (word(ctx,0x150)!=44100 && word(ctx,0x150)!=48000) ||
       word(ctx,0x154)!=0x61616365 || word(ctx,0x160)!=FRAME ||
       word(ctx,0x174)!=word(ctx,0x150) || word(ctx,0x188)!=2 ||
       word(ctx,0x18c)!=1 || word(ctx,0x190)!=16)return;
    s=NAV_ALLOC(sizeof(*s));
    if(!s){taskENTER_CRITICAL();alloc_fail++;taskEXIT_CRITICAL();return;}
    s->ctx=ctx;s->stats.sid=carbox_nav_session_id(ctx);s->stats.rate=word(ctx,0x150);
    taskENTER_CRITICAL();
    for(i=0;i<4;i++)if(!sessions[i]){sessions[i]=s;break;}
    if(i==4)alloc_fail++;
    taskEXIT_CRITICAL();if(i==4)free(s);
}
/* Called ONLY from Read recovery under the context semaphore. PutBusyNode's
 * emergency purge remains native. A later rebuffer also remains native. */
void carbox_nav_startup_purge(void *ctx)
{
    struct continuity *s=find(ctx);
    if(s && !s->started){
        uint32_t cap=word(ctx,0x148),busy=word(ctx,0x14c);
        void *head=pointer(ctx,0x138);if(!head)head=pointer(ctx,0x140);
        if(head){
            s->started=1;s->stats.start_head=word(head,12);s->stats.start_busy=busy;
            if(cap>3 && busy<cap-3){s->stats.start_keep++;return;}
            s->stats.start_pressure++;
        }
    }
    _RTPJitterBufferDiscardExcess(ctx);
}
static void publish(struct continuity *s,uint32_t ts)
{
    unsigned i;int vacant=-1;
    lock(s->ctx);
    for(i=0;i<CACHE;i++){
        struct cached_pcm *p=&s->cache[i];
        if(p->valid && s->started && (int32_t)(word(s->ctx,0x1b8)-(p->ts+FRAME))>=0){
            p->valid=0;s->stats.cache_expired++;
        }
        if(!p->valid && vacant<0)vacant=(int)i;
    }
    if(s->started && (int32_t)(word(s->ctx,0x1b8)-(ts+FRAME))>=0)s->stats.cache_expired++;
    else if(vacant<0)s->stats.cache_full++;
    else {
        struct cached_pcm *p=&s->cache[vacant];
        memcpy(p->pcm,s->scratch,sizeof(p->pcm));p->ts=ts;p->valid=1;
    }
    unlock(s->ctx);
}
int carbox_nav_convert(void *ctx,void *converter,void *callback,void *node,
                       uint32_t *samples,void *buffers,void *descs)
{
    struct continuity *s=find(ctx);uint32_t ts=node?word(node,12):0;int rc;
    /* Converter stores the AAC router handle, not a raw FDK handle. Always
     * enter its DecodeFrame wrapper. Verify the pinned format/channel fields. */
    if(s && converter && word(converter,0)==0x61616365 && word(converter,12)==1 && pointer(converter,16)){
        uint32_t gap=ts-s->expected;
        if(s->have_expected && gap){
            if(gap<=3*FRAME && gap%FRAME==0){
                uint32_t n=gap/FRAME,i;
                s->stats.gap_events++;s->stats.last_gap_ts=s->expected;s->stats.last_gap_frames=n;
                for(i=0;i<n;i++){
                    uint32_t start=hal_read_curtime_us();
                    navtx_decode_context(s->stats.sid,s->expected+i*FRAME,1);
                    int e=__wrap_aacDecoder_DecodeFrame(pointer(converter,16),s->scratch,FRAME,AACDEC_CONCEAL);
                    navtx_decode_context(0,0,0);
                    uint32_t elapsed=hal_read_curtime_us()-start;
                    if(elapsed>s->stats.plc_max_us)s->stats.plc_max_us=elapsed;
                    if(IS_OUTPUT_VALID(e)){s->stats.plc_ok++;publish(s,s->expected+i*FRAME);}
                    else s->stats.plc_fail++;
                }
            }else s->stats.gap_skip++;
        }
    }
    if(s)navtx_decode_context(s->stats.sid,ts,0);
    rc=AudioConverterFillComplexBuffer(converter,callback,node,samples,buffers,descs);
    if(s)navtx_decode_context(0,0,0);
    if(s){
        /* Failed/zero-output decode breaks continuity; never conceal the same
         * missing interval twice following a decoder error. */
        s->have_expected=(!rc || rc==-6750) && samples && *samples==FRAME;
        if(s->have_expected)s->expected=ts+FRAME;
    }
    return rc;
}
/* Read holds context semaphore. Replace only future/gap memset, never startup
 * silence or underrun padding. Return count actually substituted for evidence. */
uint32_t carbox_nav_fill_gap(void *ctx,void *dst,uint32_t bytes,uint32_t ts,unsigned reason)
{
    struct continuity *s=find(ctx);uint32_t done=0,filled=0;
    memset(dst,0,bytes);
    if(!s || bytes%2)return 0;
    while(done<bytes/2){
        unsigned i;struct cached_pcm *hit=NULL;uint32_t at=ts+done,off=0,n;
        for(i=0;i<CACHE;i++){
            struct cached_pcm *p=&s->cache[i];
            if(p->valid && (uint32_t)(at-p->ts)<FRAME){hit=p;off=at-p->ts;break;}
        }
        if(!hit){done++;continue;}
        n=FRAME-off;if(n>bytes/2-done)n=bytes/2-done;
        memcpy((int16_t *)dst+done,hit->pcm+off,n*2);filled+=n*2;done+=n;
        if(off+n==FRAME)hit->valid=0;
    }
    s->stats.plc_bytes+=filled;s->stats.fallback_bytes+=bytes-filled;
    if(filled)carbox_nav_diag_concealed(ctx,reason,filled);
    return filled;
}
/* Thin C bridges from saved private call-site registers. r4=context. */
int carbox_nav_convert_saved(void *ctx,const uint32_t *r)
{
    return carbox_nav_convert(ctx,(void *)(uintptr_t)r[0],(void *)(uintptr_t)r[1],
        (void *)(uintptr_t)r[2],(uint32_t *)(uintptr_t)r[3],
        (void *)(uintptr_t)r[14],(void *)(uintptr_t)r[15]);
}
void *carbox_nav_gap_saved(void *ctx,const uint32_t *r,unsigned reason)
{
    void *dst=(void *)(uintptr_t)r[0];
    carbox_nav_fill_gap(ctx,dst,r[2],reason==11?r[8]-r[5]:r[5],reason);
    return dst;
}
void carbox_nav_continuity_stop(void *ctx)
{
    struct continuity *s=find(ctx);void *p;uint64_t sum=0;unsigned walked=0;
    if(!s)return;
    /* Output caller is stopping; the context lock still protects decoded PCM
     * against native recycling. Never walk encoded node PCM while decoding. */
    lock(ctx);p=pointer(ctx,0x138);
    while(p && walked++<256){
        uint32_t count=word(p,0x5c8)/2,i;const uint8_t *pcm=pointer(p,0x5d0);
        s->stats.tail_nodes++;
        if(count>FRAME || (!pcm && count)){s->stats.tail_truncated++;break;}
        for(i=0;i<count;i++){
            int16_t sample;int32_t v;memcpy(&sample,pcm+i*2,2);v=sample;if(v<0)v=-v;
            sum+=(uint32_t)v;if(v)s->stats.tail_nonzero++;
            if((uint32_t)v>s->stats.tail_peak)s->stats.tail_peak=(uint32_t)v;
        }
        s->stats.tail_samples+=count;p=pointer(p,0);
    }
    if(p)s->stats.tail_truncated++;
    p=pointer(ctx,0x140);walked=0;
    while(p && walked++<256){s->stats.tail_encoded++;p=pointer(p,0);}
    if(p)s->stats.tail_truncated++;
    s->stats.tail_mean_abs=s->stats.tail_samples?(uint32_t)(sum/s->stats.tail_samples):0;
    s->stats.tail_valid=1;s->stats.at_us=hal_read_curtime_us();unlock(ctx);
}
void carbox_nav_continuity_before_mutex_destroy(void *mutex)
{
    unsigned i;void *ctx=NULL;
    taskENTER_CRITICAL();
    for(i=0;i<4;i++)if(sessions[i] && (uint8_t *)sessions[i]->ctx+0xc4==mutex){ctx=sessions[i]->ctx;break;}
    taskEXIT_CRITICAL();
    if(ctx)carbox_nav_continuity_stop(ctx);
}
void carbox_nav_continuity_close(void *ctx)
{
    unsigned i;struct continuity *s=NULL;
    taskENTER_CRITICAL();
    for(i=0;i<4;i++)if(sessions[i] && sessions[i]->ctx==ctx){s=sessions[i];sessions[i]=NULL;break;}
    if(s){
        s->stats.closed=1;
        if(closed_head-closed_tail<4)closed_stats[closed_head++%4]=s->stats;else records_lost++;
    }
    taskEXIT_CRITICAL();free(s);
}
#define U(x) ((unsigned long)(x))
static void report(const struct continuity_stats *s)
{
    carbox_audio_diag_log("[NAV_CONT] session=%lu closed=%lu rate=%lu startup_keep/pressure=%lu/%lu head_ts=%lu busy=%lu gap_events/skip=%lu/%lu last_gap_ts/frames=%lu/%lu plc_ok/fail=%lu/%lu plc/fallback_bytes=%lu/%lu cache_full/expired=%lu/%lu plc_max_us=%lu\n",
      U(s->sid),U(s->closed),U(s->rate),U(s->start_keep),U(s->start_pressure),U(s->start_head),U(s->start_busy),U(s->gap_events),U(s->gap_skip),U(s->last_gap_ts),U(s->last_gap_frames),U(s->plc_ok),U(s->plc_fail),U(s->plc_bytes),U(s->fallback_bytes),U(s->cache_full),U(s->cache_expired),U(s->plc_max_us));
    if(s->closed)carbox_audio_diag_log("[NAV_STOP_PCM] session=%lu at_us=%lu valid=%lu samples=%lu duration_us=%lu nodes=%lu encoded_nodes=%lu nonzero=%lu mean_abs=%lu peak=%lu truncated=%lu\n",
      U(s->sid),U(s->at_us),U(s->tail_valid),U(s->tail_samples),U(s->rate?(uint64_t)s->tail_samples*1000000/s->rate:0),U(s->tail_nodes),U(s->tail_encoded),U(s->tail_nonzero),U(s->tail_mean_abs),U(s->tail_peak),U(s->tail_truncated));
}
void carbox_nav_continuity_report(void)
{
    unsigned i;struct continuity_stats s;uint32_t lost,failed;
    for(i=0;i<4;i++){
        int have;
        taskENTER_CRITICAL();have=sessions[i]!=NULL;if(have)s=sessions[i]->stats;taskEXIT_CRITICAL();
        if(have)report(&s);
    }
    for(i=0;i<4;i++){
        int have;
        taskENTER_CRITICAL();have=closed_head!=closed_tail;if(have)s=closed_stats[closed_tail++%4];taskEXIT_CRITICAL();
        if(!have)break;
        report(&s);
    }
    taskENTER_CRITICAL();lost=records_lost;failed=alloc_fail;taskEXIT_CRITICAL();
    if(lost || failed)carbox_audio_diag_log("[NAV_CONT_LOST] records=%lu allocation_or_slots=%lu\n",U(lost),U(failed));
}
