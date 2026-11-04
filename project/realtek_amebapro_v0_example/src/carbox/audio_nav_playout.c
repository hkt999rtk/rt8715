/* Navigation-only continuity policy. Native condition callers hold the same
 * condition mutex across this wrapper. Never acquire the context semaphore
 * here: producers take context -> condition, so the reverse order deadlocks.
 * Signals are coalesced, not counted as packets. A pending signal guarantees
 * one predicate recheck; subsequent native waits preserve timestamp gating.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#ifndef NAV_PLAYOUT_HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
#include "pthread.h"
#include "audio_diag_log.h"
#endif
#ifndef CONFIG_AUDIO_NAV_PLAYOUT
#define CONFIG_AUDIO_NAV_PLAYOUT 0
#endif
#ifndef CONFIG_AUDIO_NAV_DIAG
#define CONFIG_AUDIO_NAV_DIAG 0
#endif
#ifndef CONFIG_AUDIO_NAV_CONTINUITY
#define CONFIG_AUDIO_NAV_CONTINUITY 0
#endif
#if CONFIG_AUDIO_NAV_CONTINUITY
extern void carbox_nav_continuity_open(void *);
extern void carbox_nav_continuity_before_mutex_destroy(void *);
extern void carbox_nav_continuity_close(void *);
extern void carbox_nav_continuity_report(void);
#endif
extern int __real_RTPJitterBufferInit(void *,const void *,const void *,uint32_t);
extern void __real_RTPJitterBufferFree(void *);
#if CONFIG_AUDIO_NAV_DIAG
extern void carbox_nav_diag_open(void *);
extern void carbox_nav_diag_close(void *);
#endif
#if CONFIG_AUDIO_NAV_PLAYOUT
#define NAV_EXTRA_MS 30U
#define NAV_LATENCY_CAP_MS 400U
struct playout_slot {
    void *ctx, *cond;
    uint32_t pending, signals, bypass, waits, requested_ms, effective_ms;
};
static struct playout_slot playout[4];
static uint32_t playout_overflow;
static struct playout_slot *find_cond(void *cond)
{
    unsigned i;
    for(i=0;i<4;i++)if(playout[i].ctx && playout[i].cond==cond)return &playout[i];
    return NULL;
}
static uint32_t format_word(const void *format,unsigned index)
{
    uint32_t n;memcpy(&n,(const uint8_t *)format+index*4,4);return n;
}
#endif
int carbox_nav_condition_signal(void *cond)
{
#if CONFIG_AUDIO_NAV_PLAYOUT
    struct playout_slot *s;
    taskENTER_CRITICAL();s=find_cond(cond);
    if(s){s->pending=1;s->signals++;}
    taskEXIT_CRITICAL();
#endif
    /* Always wake an actual waiter, including native teardown/join. */
    return pthread_cond_signal(cond);
}
int carbox_nav_condition_wait(void *cond,void *mutex)
{
#if CONFIG_AUDIO_NAV_PLAYOUT
    struct playout_slot *s;int rc;
    taskENTER_CRITICAL();s=find_cond(cond);
    if(s && s->pending){s->pending=0;s->bypass++;taskEXIT_CRITICAL();return 0;}
    if(s)s->waits++;
    taskEXIT_CRITICAL();
    rc=pthread_cond_wait(cond,mutex);
    /* Successful wait returns with native condition mutex held: no producer
     * can signal between wake consumption and this clearing operation. */
    if(!rc && s){taskENTER_CRITICAL();s->pending=0;taskEXIT_CRITICAL();}
    return rc;
#else
    return pthread_cond_wait(cond,mutex);
#endif
}
/* Native Free destroys this mutex after decoder join and before clearing
 * queues. Member-local symbol redirection; other pthread users are untouched. */
int carbox_nav_condition_destroy_mutex(void *mutex)
{
#if CONFIG_AUDIO_NAV_CONTINUITY
    carbox_nav_continuity_before_mutex_destroy(mutex);
#endif
    return pthread_mutex_destroy(mutex);
}
int __wrap_RTPJitterBufferInit(void *ctx,const void *in,const void *out,uint32_t latency)
{
    int rc;
#if CONFIG_AUDIO_NAV_PLAYOUT
    struct playout_slot *s=NULL;unsigned i;
    if(ctx && in && (format_word(in,0)==44100U || format_word(in,0)==48000U)
       && format_word(in,4)==480U){
        taskENTER_CRITICAL();
        for(i=0;i<4;i++)if(!playout[i].ctx){s=&playout[i];break;}
        if(s){
            memset(s,0,sizeof(*s));s->ctx=ctx;
            /* Pinned AirPlayUtils layout: embedded condition at context+0x10.
             * Reserve before native Init starts the decoder thread. */
            s->cond=(uint8_t *)ctx+0x10;s->requested_ms=latency;
            if(latency<NAV_LATENCY_CAP_MS){
                uint32_t room=NAV_LATENCY_CAP_MS-latency;
                latency+=room<NAV_EXTRA_MS?room:NAV_EXTRA_MS;
            }
            s->effective_ms=latency;
        }else playout_overflow++;
        taskEXIT_CRITICAL();
    }
#endif
    rc=__real_RTPJitterBufferInit(ctx,in,out,latency);
#if CONFIG_AUDIO_NAV_PLAYOUT
    if(rc && s){taskENTER_CRITICAL();memset(s,0,sizeof(*s));taskEXIT_CRITICAL();}
#endif
#if CONFIG_AUDIO_NAV_DIAG
    if(!rc && ctx)carbox_nav_diag_open(ctx);
#endif
#if CONFIG_AUDIO_NAV_CONTINUITY
    if(!rc && ctx)carbox_nav_continuity_open(ctx);
#endif
    return rc;
}
void __wrap_RTPJitterBufferFree(void *ctx)
{
    __real_RTPJitterBufferFree(ctx); /* Native Free joins decoder first. */
#if CONFIG_AUDIO_NAV_CONTINUITY
    carbox_nav_continuity_close(ctx);
#endif
#if CONFIG_AUDIO_NAV_DIAG
    carbox_nav_diag_close(ctx);
#endif
#if CONFIG_AUDIO_NAV_PLAYOUT
    unsigned i;
    taskENTER_CRITICAL();
    for(i=0;i<4;i++)if(playout[i].ctx==ctx)memset(&playout[i],0,sizeof(playout[i]));
    taskEXIT_CRITICAL();
#endif
}
#if CONFIG_AUDIO_NAV_DIAG
void carbox_nav_playout_report(void)
{
#if CONFIG_AUDIO_NAV_CONTINUITY
    carbox_nav_continuity_report();
#endif
#if CONFIG_AUDIO_NAV_PLAYOUT
    unsigned i;
    for(i=0;i<4;i++){
        struct playout_slot s;uint32_t lost;
        taskENTER_CRITICAL();s=playout[i];lost=playout_overflow;taskEXIT_CRITICAL();
        if(s.ctx)carbox_audio_diag_log("[NAV_POLICY] ctx=%p latency_ms=%lu->%lu wake_signals=%lu retained_wakes=%lu waits=%lu pending=%lu slot_overflow=%lu\n",
            s.ctx,(unsigned long)s.requested_ms,(unsigned long)s.effective_ms,
            (unsigned long)s.signals,(unsigned long)s.bypass,(unsigned long)s.waits,
            (unsigned long)s.pending,(unsigned long)lost);
    }
#endif
}
#endif
