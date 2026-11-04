/* Diagnostic only: fixed storage, bounded critical sections, deferred printing.
 * No PCM data is retained. A trace ends only after send return AND HAL return,
 * in either order. Late completions carry a generation id, never a raw pointer.
 */
#include "nav_tx_trace.h"
#if CONFIG_AUDIO_NAV_TX_TRACE
#include <string.h>
#ifndef NAVTX_HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "queue.h"
#include "lwip/pbuf.h"
#endif
extern void carbox_rxhd_log(const char *, ...);
#define NT_SLOTS 64U
#define NT_CONTEXTS 8U
#define NT_EVENTS 16U
#define NT_BSS __attribute__((section(".lpddr.bss.nav_tx_trace")))
struct nt_record {
    uint32_t id, epoch, seq, ts, length, q_us, debt_us, mask;
    uint32_t at[NT_STAGES], owner_age, depth;
    uintptr_t owner, owner_site;
    char owner_name[16];
    const void *pbuf, *payload, *ntb;
    int result, error, transport_status;
    unsigned send_done, attached, transport_done;
};
struct nt_context { uintptr_t task; uint32_t id, sid, ts, plc; };
struct nt_metric { uint64_t sum; uint32_t n, max; };
struct nt_fdk { uint32_t sid, ts, plc, at; int before, after, ok, samples; };
static struct nt_record records[NT_SLOTS] NT_BSS;
static struct nt_record events[NT_EVENTS] NT_BSS;
static struct nt_context contexts[NT_CONTEXTS] NT_BSS;
static struct nt_metric metrics[8] NT_BSS;
static struct nt_fdk fdk_events[NT_EVENTS] NT_BSS;
static struct {
    uint32_t next, alloc_lost, stale, detail_lost, expired, no_ncm, complete, failed, pending;
    uint32_t head, tail, fhead, ftail, fdk_lost, fdk_calls, fdk_nonok, fdk_transitions;
    uint32_t owner_since, last_report;
    uintptr_t owner, owner_site;
    char owner_name[16];
} st NT_BSS;
static struct nt_record report_copy NT_BSS;
#if CONFIG_NCM_TX_VENDOR_ASYNC
/* The adapter below is enabled only with a hash-pinned ncm_tx_async.o.
 * Queue item is 20 bytes; NTB builder uses batch16/segment64 regardless of
 * the local lwIP build's CONFIG_NCM_TX_BATCH_MAX. No native layouts change. */
struct nt_vendor_item {struct pbuf *p; uint32_t enqueue, generation, irq; uint8_t timer, pad[3];};
struct nt_vendor_frame {size_t len;uint16_t first_segment,segment_count;};
struct nt_vendor_segment {const void *data;size_t len;};
struct nt_vendor_batch {
    uint32_t magic;uint16_t frames,segments;
    struct nt_vendor_frame frame[16];struct nt_vendor_segment segment[64];
};
#ifndef NAVTX_HOST_TEST
_Static_assert(sizeof(struct nt_vendor_item)==20,"vendor queue ABI changed");
_Static_assert(sizeof(struct nt_vendor_batch)==648,"vendor batch ABI changed");
#endif
extern QueueHandle_t navtx_vendor_input_queue, navtx_vendor_ready_queue;
extern int carbox_ncm_tx_build_batch(const void *,void *,size_t,size_t *,uint16_t *);
extern int carbox_ncm_tx_send_prebuilt(void *,size_t);
#endif
static struct nt_context *context(uintptr_t task, int create)
{
    unsigned i; struct nt_context *free_slot = NULL;
    for (i=0;i<NT_CONTEXTS;i++) {
        if (contexts[i].task == task) return &contexts[i];
        if (!contexts[i].task && !free_slot) free_slot=&contexts[i];
    }
    if (create && free_slot) { free_slot->task=task; return free_slot; }
    return NULL;
}
static struct nt_record *record(uint32_t id)
{ return id && records[id%NT_SLOTS].id==id ? &records[id%NT_SLOTS] : NULL; }
static struct nt_record *current(void)
{
    struct nt_context *c=context((uintptr_t)xTaskGetCurrentTaskHandle(),0);
    return c ? record(c->id) : NULL;
}
static void stamp(struct nt_record *r, unsigned stage, uint32_t now)
{
    if (stage==NT_BUILD || !(r->mask & (1U<<stage))) {r->at[stage]=now;r->mask|=1U<<stage;}
}
static uint32_t delta(const struct nt_record *r, unsigned a, unsigned b)
{
    uint32_t m=(1U<<a)|(1U<<b);
    return (r->mask&m)==m ? r->at[b]-r->at[a] : 0;
}
/* All metric mutation/finalization is under a short task critical section. */
static void metric(unsigned n,const struct nt_record *r,unsigned a,unsigned b)
{
    uint32_t m=(1U<<a)|(1U<<b), d;
    if ((r->mask&m)!=m) return;
    d=delta(r,a,b);metrics[n].sum+=d;metrics[n].n++;
    if(d>metrics[n].max)metrics[n].max=d;
}
static void finish(struct nt_record *r)
{
    unsigned slow;
    if(!r->send_done || (r->attached && !r->transport_done))return;
    st.complete++;
    if(r->attached)st.pending--;
    if(!r->attached)st.no_ncm++;
    if(r->error || r->result!=(int)r->length || r->transport_status)st.failed++;
    metric(0,r,NT_ENTER,NT_RETURN);metric(1,r,NT_ENTER,NT_COPY);
    metric(2,r,NT_WAIT,NT_LOCK);metric(3,r,NT_LOCK,NT_CORE_END);
    metric(4,r,NT_ENQUEUE,NT_DEQUEUE);metric(5,r,NT_DEQUEUE,NT_BUILD);
    metric(6,r,NT_READY,NT_USB);metric(7,r,NT_USB,NT_HAL_END);
    slow=delta(r,NT_ENTER,NT_RETURN)>=5000U ||
         delta(r,NT_ENQUEUE,NT_HAL_END)>=10000U || r->transport_status || r->error;
    if(slow) {
        if(st.head-st.tail<NT_EVENTS)events[st.head++%NT_EVENTS]=*r;
        else st.detail_lost++;
    }
    r->id=0;
}
uint32_t navtx_begin(uint32_t epoch,const void *data,size_t len,uint32_t q,uint32_t debt)
{
    const uint8_t *h=data; unsigned i; uint32_t id=0;
    struct nt_context *c;
    if(!data || len<12)return 0;
    taskENTER_CRITICAL();
    c=context((uintptr_t)xTaskGetCurrentTaskHandle(),1);
    if(c && !c->id)for(i=0;i<NT_SLOTS;i++) {
        if(++st.next==0)++st.next;
        if(!records[st.next%NT_SLOTS].id) {
            struct nt_record *r=&records[st.next%NT_SLOTS];
            memset(r,0,sizeof(*r));id=r->id=st.next;
            r->epoch=epoch;r->seq=(uint32_t)h[2]*256+h[3];
            r->ts=((uint32_t)h[4]<<24)|((uint32_t)h[5]<<16)|((uint32_t)h[6]<<8)|h[7];
            r->length=len;r->q_us=q;r->debt_us=debt;
            stamp(r,NT_ENTER,hal_read_curtime_us());c->id=id;break;
        }
    }
    if(!id){st.alloc_lost++;if(c && !c->id && !c->sid)c->task=0;}
    taskEXIT_CRITICAL();return id;
}
void navtx_end(uint32_t id,int result,int error)
{
    struct nt_record *r;struct nt_context *c;
    if(!id)return;
    taskENTER_CRITICAL();c=context((uintptr_t)xTaskGetCurrentTaskHandle(),0);
    if(c && c->id==id){c->id=0;if(!c->sid)c->task=0;}
    r=record(id);
    if(r){stamp(r,NT_RETURN,hal_read_curtime_us());r->result=result;r->error=error;r->send_done=1;finish(r);}
    else st.stale++;
    taskEXIT_CRITICAL();
}
void navtx_prepared(const void *p)
{
    struct nt_record *r;taskENTER_CRITICAL();r=current();
    if(r){r->pbuf=p;stamp(r,NT_COPY,hal_read_curtime_us());}
    taskEXIT_CRITICAL();
}
void navtx_core_wait(void)
{
    struct nt_record *r;uint32_t now;
    taskENTER_CRITICAL();r=current();
    if(r && (r->mask&(1U<<NT_COPY)) && !(r->mask&(1U<<NT_WAIT))) {
        now=hal_read_curtime_us();stamp(r,NT_WAIT,now);
        r->owner=st.owner;r->owner_site=st.owner_site;
        memcpy(r->owner_name,st.owner_name,sizeof(r->owner_name));
        r->owner_age=st.owner?now-st.owner_since:0;
    }
    taskEXIT_CRITICAL();
}
void navtx_core_acquired(uintptr_t caller)
{
    struct nt_record *r;uint32_t now;
    taskENTER_CRITICAL();now=hal_read_curtime_us();
    st.owner=(uintptr_t)xTaskGetCurrentTaskHandle();st.owner_site=caller;st.owner_since=now;
    {
        const char *name=pcTaskGetName(NULL);unsigned i=0;
        if(name)for(;i<sizeof(st.owner_name)-1U && name[i];i++)st.owner_name[i]=name[i];
        st.owner_name[i]=0;
    }
    r=current();if(r && (r->mask&(1U<<NT_WAIT)))stamp(r,NT_LOCK,now);
    taskEXIT_CRITICAL();
}
void navtx_core_release(void)
{
    struct nt_record *r;taskENTER_CRITICAL();r=current();
    if(r && (r->mask&(1U<<NT_LOCK)))stamp(r,NT_CORE_END,hal_read_curtime_us());
    st.owner=0;st.owner_site=0;st.owner_name[0]=0;taskEXIT_CRITICAL();
}
uint32_t navtx_ncm(const void *p,uint32_t depth)
{
    struct nt_record *r;uint32_t id=0;
    taskENTER_CRITICAL();r=current();
    /* Exact pbuf identity excludes ARP/ND generated by this same send. Deferred
     * neighbour output or fragmentation is explicitly outside this correlation. */
    if(r && r->pbuf==p && !r->attached) {
        id=r->id;r->attached=1;st.pending++;r->depth=depth;stamp(r,NT_ENQUEUE,hal_read_curtime_us());
    }
    taskEXIT_CRITICAL();return id;
}
void navtx_mark(uint32_t id,unsigned stage)
{
    struct nt_record *r;if(!id || stage>=NT_STAGES)return;
    taskENTER_CRITICAL();r=record(id);
    if(r)stamp(r,stage,hal_read_curtime_us());else st.stale++;
    taskEXIT_CRITICAL();
}
void navtx_transport_end(uint32_t id,int status)
{
    struct nt_record *r;if(!id)return;
    taskENTER_CRITICAL();r=record(id);
    if(r){stamp(r,NT_HAL_END,hal_read_curtime_us());r->transport_status=status;r->transport_done=1;finish(r);}
    else st.stale++;
    taskEXIT_CRITICAL();
}
#if CONFIG_NCM_TX_VENDOR_ASYNC
/* Native source pbuf references protect pointer identity until scoped free.
 * Once copied, retain only the generation + destination NTB identity. */
static uint32_t vendor_source(const void *p, int payload)
{
    unsigned i;uint32_t id=0;
    taskENTER_CRITICAL();
    if(st.pending)for(i=0;i<NT_SLOTS;i++) {
        struct nt_record *r=&records[i];
        if(r->id && r->attached && !r->transport_done &&
           (payload?r->payload:r->pbuf)==p) {id=r->id;break;}
    }
    taskEXIT_CRITICAL();return id;
}
static unsigned vendor_ntb(const void *buffer,uint32_t ids[16])
{
    unsigned i,n=0;taskENTER_CRITICAL();
    if(st.pending)for(i=0;i<NT_SLOTS && n<16;i++) {
        struct nt_record *r=&records[i];
        if(r->id && r->attached && !r->transport_done && r->ntb==buffer)ids[n++]=r->id;
    }
    taskEXIT_CRITICAL();return n;
}
BaseType_t navtx_vendor_send(QueueHandle_t q,const void *item,TickType_t ticks,BaseType_t pos)
{
    uint32_t id=0;BaseType_t rc;
    if(q==navtx_vendor_input_queue && item) {
        const struct nt_vendor_item *v=item;
        if(!v->timer && v->p) {
            id=navtx_ncm(v->p,uxQueueMessagesWaiting(q));
            if(id) {
                struct nt_record *r;taskENTER_CRITICAL();r=record(id);
                if(r)r->payload=v->p->payload;
                taskEXIT_CRITICAL();
            }
        }
    }else if(q==navtx_vendor_ready_queue && item) {
        uint32_t ids[16];unsigned i,n=vendor_ntb(*(void *const *)item,ids);
        for(i=0;i<n;i++)navtx_mark(ids[i],NT_READY);
    }
    rc=xQueueGenericSend(q,item,ticks,pos);
    if(id && rc!=pdPASS)navtx_transport_end(id,-2);
    return rc;
}
BaseType_t navtx_vendor_receive(QueueHandle_t q,void *item,TickType_t ticks)
{
    BaseType_t rc=xQueueReceive(q,item,ticks);
    if(rc==pdPASS && q==navtx_vendor_input_queue && item) {
        struct nt_vendor_item *v=item;
        if(!v->timer && v->p)navtx_mark(vendor_source(v->p,0),NT_DEQUEUE);
    }
    return rc;
}
int navtx_vendor_build(const void *batch,void *buffer,size_t capacity,size_t *len,uint16_t *sent)
{
    const struct nt_vendor_batch *b=batch;
    uint32_t ids[16]={0};unsigned i,n=0;int rc;
    if(b && b->magic==0x324d434eU && b->frames<=16 && b->segments<=64) {
        n=b->frames;
        for(i=0;i<n;i++)if(b->frame[i].segment_count && b->frame[i].first_segment<b->segments) {
            ids[i]=vendor_source(b->segment[b->frame[i].first_segment].data,1);
            navtx_mark(ids[i],NT_BUILD);
        }
    }
    rc=carbox_ncm_tx_build_batch(batch,buffer,capacity,len,sent);
    if(!rc && sent && *sent<=n)for(i=0;i<*sent;i++)if(ids[i]) {
        struct nt_record *r;taskENTER_CRITICAL();r=record(ids[i]);
        if(r)r->ntb=buffer;
        taskEXIT_CRITICAL();
    }
    return rc;
}
int navtx_vendor_usb(void *buffer,size_t len)
{
    uint32_t ids[16];unsigned i,n=vendor_ntb(buffer,ids);int rc;
    for(i=0;i<n;i++)navtx_mark(ids[i],NT_USB);
    rc=carbox_ncm_tx_send_prebuilt(buffer,len);
    for(i=0;i<n;i++)navtx_transport_end(ids[i],rc);
    return rc;
}
u8_t navtx_vendor_free(struct pbuf *p)
{
    uint32_t id=vendor_source(p,0);unsigned orphan=0;
    if(id) {
        struct nt_record *r;taskENTER_CRITICAL();r=record(id);
        if(r){orphan=!r->ntb;r->pbuf=NULL;r->payload=NULL;}
        taskEXIT_CRITICAL();
        /* Native source reference released without an NTB: builder/queue failed.
         * HAL may have completed already; generation lookup then safely misses. */
        if(orphan)navtx_transport_end(id,-3);
    }
    return pbuf_free(p);
}
#endif
void navtx_decode_context(uint32_t sid,uint32_t ts,unsigned plc)
{
    struct nt_context *c;taskENTER_CRITICAL();
    c=context((uintptr_t)xTaskGetCurrentTaskHandle(),sid!=0);
    if(c){c->sid=sid;c->ts=ts;c->plc=plc;if(!sid && !c->id)c->task=0;}
    else if(sid)st.fdk_lost++;
    taskEXIT_CRITICAL();
}
void navtx_fdk_state(int before,int after,int ok,int samples)
{
    struct nt_context *c;taskENTER_CRITICAL();
    c=context((uintptr_t)xTaskGetCurrentTaskHandle(),0);
    if(c && c->sid) {
        st.fdk_calls++;if(after!=0)st.fdk_nonok++;
        if(before!=after)st.fdk_transitions++;
        if(before!=after || !ok || c->plc) {
            if(st.fhead-st.ftail<NT_EVENTS) {
                struct nt_fdk *f=&fdk_events[st.fhead++%NT_EVENTS];
                f->sid=c->sid;f->ts=c->ts;f->plc=c->plc;f->at=hal_read_curtime_us();
                f->before=before;f->after=after;f->ok=ok;f->samples=samples;
            }else st.fdk_lost++;
        }
    }
    taskEXIT_CRITICAL();
}
void navtx_report(void)
{
    unsigned i,j;uint32_t now=hal_read_curtime_us();
    if((uint32_t)(now-st.last_report)<5000000U)return;
    st.last_report=now;
    /* Expire each slot separately; never hold interrupts off for a whole table. */
    for(i=0;i<NT_SLOTS;i++) {
        taskENTER_CRITICAL();
        if(records[i].id && (int32_t)(now-records[i].at[NT_ENTER])>5000000) {
            if(records[i].attached)st.pending--;
            records[i].id=0;st.expired++;
        }
        taskEXIT_CRITICAL();
    }
    {
        uint32_t complete,failed,no_ncm,lost,stale,expired,details,fdk_n,fdk_bad,fdk_change,fdk_lost;
        taskENTER_CRITICAL();complete=st.complete;failed=st.failed;no_ncm=st.no_ncm;
        lost=st.alloc_lost;stale=st.stale;expired=st.expired;details=st.detail_lost;
        fdk_n=st.fdk_calls;fdk_bad=st.fdk_nonok;fdk_change=st.fdk_transitions;fdk_lost=st.fdk_lost;
        taskEXIT_CRITICAL();
        carbox_rxhd_log("[NAV_TX] v=1 at_us=%lu cumulative=1 done/fail/no_ncm=%lu/%lu/%lu lost/stale/expired/detail_lost=%lu/%lu/%lu/%lu fdk_calls/nonok/transitions/lost=%lu/%lu/%lu/%lu USB=HAL-return-not-peer-playback\n",
            (unsigned long)now,(unsigned long)complete,(unsigned long)failed,(unsigned long)no_ncm,
            (unsigned long)lost,(unsigned long)stale,(unsigned long)expired,(unsigned long)details,
            (unsigned long)fdk_n,(unsigned long)fdk_bad,(unsigned long)fdk_change,(unsigned long)fdk_lost);
    }
    /* Metric ids: send, alloc/copy, core wait, core hold, input queue,
     * gather/free-slot, ready queue, blocking USB HAL. Missing != zero duration. */
    for(i=0;i<8;i++) {
        struct nt_metric m;taskENTER_CRITICAL();m=metrics[i];taskEXIT_CRITICAL();
        if(m.n)carbox_rxhd_log("[NAV_TX_METRIC] stage=%u n=%lu avg/max_us=%lu/%lu\n",i,
            (unsigned long)m.n,(unsigned long)(m.sum/m.n),(unsigned long)m.max);
    }
    for(j=0;j<4;j++) {
        struct nt_record *r=&report_copy;unsigned ready;
        taskENTER_CRITICAL();ready=st.tail!=st.head;
        if(ready)*r=events[st.tail++%NT_EVENTS];
        taskEXIT_CRITICAL();
        if(!ready)break;
        carbox_rxhd_log("[NAV_TX_SLOW] id=%lu epoch=%lu seq=%lu ts=%lu at_us=%lu mask=%lx q/debt_us=%lu/%lu depth=%lu result/errno/hal=%d/%d/%d\n",
            (unsigned long)r->id,(unsigned long)r->epoch,(unsigned long)r->seq,(unsigned long)r->ts,
            (unsigned long)r->at[NT_ENTER],(unsigned long)r->mask,(unsigned long)r->q_us,
            (unsigned long)r->debt_us,(unsigned long)r->depth,r->result,r->error,r->transport_status);
        carbox_rxhd_log("[NAV_TX_SLOW] id=%lu send/copy/corewait/corehold_us=%lu/%lu/%lu/%lu owner=%p owner_name=%s owner_site=%p owner_age_us=%lu\n",
            (unsigned long)r->id,(unsigned long)delta(r,NT_ENTER,NT_RETURN),(unsigned long)delta(r,NT_ENTER,NT_COPY),
            (unsigned long)delta(r,NT_WAIT,NT_LOCK),(unsigned long)delta(r,NT_LOCK,NT_CORE_END),
            (void*)r->owner,r->owner_name,(void*)r->owner_site,(unsigned long)r->owner_age);
        carbox_rxhd_log("[NAV_TX_SLOW] id=%lu input/gather/build/ready/hal/end2end_us=%lu/%lu/%lu/%lu/%lu/%lu\n",
            (unsigned long)r->id,(unsigned long)delta(r,NT_ENQUEUE,NT_DEQUEUE),(unsigned long)delta(r,NT_DEQUEUE,NT_BUILD),
            (unsigned long)delta(r,NT_BUILD,NT_READY),(unsigned long)delta(r,NT_READY,NT_USB),
            (unsigned long)delta(r,NT_USB,NT_HAL_END),(unsigned long)delta(r,NT_ENTER,NT_HAL_END));
    }
    for(j=0;j<4;j++) {
        struct nt_fdk f;unsigned ready;
        taskENTER_CRITICAL();ready=st.ftail!=st.fhead;if(ready)f=fdk_events[st.ftail++%NT_EVENTS];taskEXIT_CRITICAL();
        if(!ready)break;
        carbox_rxhd_log("[NAV_FDK_STATE] session=%lu ts=%lu at_us=%lu explicit_plc=%lu state=%d->%d frame_ok=%d samples=%d states=0:ok,1:single,2:fadein,3:mute,4:fadeout\n",
            (unsigned long)f.sid,(unsigned long)f.ts,(unsigned long)f.at,(unsigned long)f.plc,f.before,f.after,f.ok,f.samples);
    }
}
#endif
