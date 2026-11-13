/* 首版只观察，不改变原厂同步算法、RTP timestamp、PCM 或输出时钟。
 * 包路径无日志、无分配。源对象只在 Adjust 的原调用生命周期内访问。
 * 构建前校验 v18 对象 hash/ABI；换客户库必须重新审计。 */
#include "media_clock_evidence.h"
#if CONFIG_MEDIA_CLOCK_DRIFT_PROFILE
#include "media_clock/AirPlayMediaClockSource.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "diag.h"
#include <stddef.h>
#include <string.h>
#ifndef MEDIA_DRIFT_SYSTICK_RELOAD
#define MEDIA_DRIFT_SYSTICK_RELOAD() (*(volatile uint32_t *)0xe000e014UL)
#endif
extern uint64_t UpTicks(void);
extern uint8_t __real_AirTunesClock_Adjust(void *,int64_t,uint8_t);
extern int __real_AirTunesClock_Finalize(void *);
/* v18 AirTunesTime: 秒在 +0，64-bit 二进制小数在 +8，总长 16。
 * 不读取 clock/session 私有结构；只调用已核实的公开 getter。 */
struct source_time {uint32_t seconds,pad;uint64_t fraction;};
extern void AirTunesClock_GetSynchronizedTimeNearUpTicks(void *,struct source_time *,uint64_t);
static struct media_source source;
static struct media_discipline source_fit,rtos_fit;
static uint64_t hw_extended,last_sample;
static uint32_t hw_previous,hw_valid,samples,steps,pair_bad,query_max;
static struct media_sample last_pair;
/* 至少由既有 reporter 每 10 秒调用，增量展开不依赖 RTOS 的速率。
 * HAL 读取和扩展一起串行化，避免并发旧读数误判为一次 71 分钟回绕。 */
static uint64_t local_us(void)
{
    uint32_t raw;uint64_t result;
    taskENTER_CRITICAL();raw=hal_read_curtime_us();
    if(!hw_valid){hw_extended=raw;hw_valid=1;}else hw_extended+=(uint32_t)(raw-hw_previous);
    hw_previous=raw;result=hw_extended;taskEXIT_CRITICAL();return result;
}
static uint64_t rtos_us(void)
{
    TimeOut_t t;uint64_t ticks;vTaskSetTimeOutState(&t);
    ticks=((uint64_t)(uint32_t)t.xOverflowCount<<32)|(uint32_t)t.xTimeOnEntering;
    return ticks/configTICK_RATE_HZ*1000000ULL+(ticks%configTICK_RATE_HZ)*1000000ULL/configTICK_RATE_HZ;
}
void carbox_media_clock_evidence_pair(uint64_t *hal,uint64_t *rtos)
{
    uint64_t first=local_us();*rtos=rtos_us();*hal=first+(local_us()-first)/2;
}
/* 只由统计线程调用，每2秒配对本机时钟。原厂 RTT 筛选可让 Adjust
 * 间隔超过20秒，不能据此驱动本机斜率估算或假定网络同步仍有效。 */
static uint64_t periodic_hal,periodic_rtos;
static uint32_t periodic_span;
void carbox_media_clock_evidence_tick(void)
{
    static int initialized;
    uint64_t before=local_us(),after;
    periodic_rtos=rtos_us();after=local_us();
    periodic_hal=before+(after-before)/2;periodic_span=(uint32_t)(after-before);
    if(!initialized){MediaDiscipline_Init(&rtos_fit,periodic_hal,periodic_hal);initialized=1;}
    MediaDiscipline_Observe(&rtos_fit,0,periodic_hal,periodic_rtos,periodic_hal,
        periodic_span<=2000?MEDIA_SYNC_VALID:0);
    MediaDiscipline_Update(&rtos_fit,after,0);
}
uint8_t __wrap_AirTunesClock_Adjust(void *clock,int64_t adjustment_ns,uint8_t reset)
{
    uint8_t stepped=__real_AirTunesClock_Adjust(clock,adjustment_ns,reset);
    struct media_sample e;struct source_time t;
    uint32_t generation;uint64_t before,after,finish;
    before=local_us();
    taskENTER_CRITICAL();generation=MediaSource_Begin(&source,(uintptr_t)clock);
    steps+=!!stepped;
    if(!generation || (!stepped && !reset && last_sample && before-last_sample<2000000ULL)){
        taskEXIT_CRITICAL();return stepped;
    }
    last_sample=before;taskEXIT_CRITICAL();
    memset(&e,0,sizeof(e));memset(&t,0,sizeof(t));
    before=local_us();e.upticks=UpTicks();e.rtos_us=rtos_us();after=local_us();
    AirTunesClock_GetSynchronizedTimeNearUpTicks(clock,&t,e.upticks);
    finish=local_us();e.local_us=before+(after-before)/2;
    e.source_us=(uint64_t)t.seconds*1000000ULL+((t.fraction>>32)*1000000ULL>>32);
    e.adjustment_ns=adjustment_ns;e.generation=generation;e.flags=MEDIA_SYNC_VALID;
    e.pair_span_us=(uint32_t)(after-before);e.query_us=(uint32_t)(finish-after);
    if(stepped||reset)e.flags|=MEDIA_PHASE_STEP;
    /* 接收时钟仍在明显纠相时只记证据，不作为稳态物理频率。 */
    if(adjustment_ns>2000000 || adjustment_ns< -2000000)e.flags|=MEDIA_SOURCE_SETTLING;
    if(after-before>2000)e.flags=0;
    taskENTER_CRITICAL();
    samples++;pair_bad+=!e.flags;if(e.query_us>query_max)query_max=e.query_us;
    MediaSource_Push(&source,(uintptr_t)clock,&e);
    taskEXIT_CRITICAL();return stepped;
}
int __wrap_AirTunesClock_Finalize(void *clock)
{
    /* 先丢弃待处理样本；reporter 永远不持有可解引用的裸 clock 指针。 */
    taskENTER_CRITICAL();
    if(source.owner==(uintptr_t)clock)last_sample=0;
    MediaSource_End(&source,(uintptr_t)clock);taskEXIT_CRITICAL();
    return __real_AirTunesClock_Finalize(clock);
}
void carbox_media_clock_evidence_report(uint32_t sequence)
{
    static int initialized;static uint64_t last_report;
    struct media_sample e;uint64_t now=local_us();
    uint32_t gen,owner,lost,stale,conflicts,n,step,bad,qmax;
    if(!initialized){MediaDiscipline_Init(&source_fit,now,now);initialized=1;}
    /* 最多消费一个队列容量，生产者持续进来不能饿死统计线程。 */
    for(unsigned i=0;i<MEDIA_SOURCE_QUEUE;i++) {
        int ok;taskENTER_CRITICAL();ok=MediaSource_Pop(&source,&e);taskEXIT_CRITICAL();if(!ok)break;
        if(source_fit.generation!=e.generation) {
            MediaDiscipline_SourceReset(&source_fit,e.generation);
        }
        MediaDiscipline_Observe(&source_fit,e.generation,e.local_us,e.source_us,e.local_us,e.flags);
        last_pair=e;
    }
    now=local_us();MediaDiscipline_Update(&source_fit,now,0);MediaDiscipline_Update(&rtos_fit,now,0);
    taskENTER_CRITICAL();gen=source.generation;owner=!!source.owner;lost=source.lost;stale=source.stale;
    conflicts=source.conflicts;n=samples;step=steps;bad=pair_bad;qmax=query_max;taskEXIT_CRITICAL();
    if(!owner || gen!=source_fit.generation) {
        MediaDiscipline_SourceReset(&source_fit,gen);
    }
    if(last_report && now-last_report<10000000)return;
    last_report=now;
    rt_printf("[MEDIA_DRIFT][%lu] mode=shadow applied_ppb=0 gen=%lu owner=%lu state=%lu points=%lu source_vs_hal_ppb=%ld target_ppb=%ld range=%lu rejected=%lu source_age_ms=%lu samples=%lu steps=%lu pair_bad=%lu lost/stale/conflict=%lu/%lu/%lu query_max_us=%lu buffer_rx_plc=unavailable\r\n",
        (unsigned long)sequence,(unsigned long)gen,(unsigned long)owner,(unsigned long)source_fit.state,(unsigned long)source_fit.count,
        (long)source_fit.estimated_ppb,(long)source_fit.target_ppb,(unsigned long)source_fit.out_of_range,(unsigned long)source_fit.rejected,
        (unsigned long)(source_fit.have_sync?(now-source_fit.last_sync)/1000:UINT32_MAX),(unsigned long)n,(unsigned long)step,(unsigned long)bad,
        (unsigned long)lost,(unsigned long)stale,(unsigned long)conflicts,(unsigned long)qmax);
    rt_printf("[MEDIA_PAIR][%lu] gen=%lu local_us=%llu rtos_us=%llu upticks=%llu source_us=%llu adjust_ns=%lld flags=%lu pair_span_us=%lu periodic_hal_us=%llu periodic_rtos_us=%llu periodic_span_us=%lu rtos_vs_hal_ppb=%ld rtos_points=%lu rtos_range=%lu core_hz=%lu systick_reload=%lu tick_hz=%lu timer_hz=%lu tickless=%u\r\n",
        (unsigned long)sequence,(unsigned long)last_pair.generation,(unsigned long long)last_pair.local_us,(unsigned long long)last_pair.rtos_us,
        (unsigned long long)last_pair.upticks,(unsigned long long)last_pair.source_us,(long long)last_pair.adjustment_ns,(unsigned long)last_pair.flags,
        (unsigned long)last_pair.pair_span_us,(unsigned long long)periodic_hal,(unsigned long long)periodic_rtos,(unsigned long)periodic_span,(long)rtos_fit.estimated_ppb,(unsigned long)rtos_fit.count,(unsigned long)rtos_fit.out_of_range,
        (unsigned long)SystemCoreClock,(unsigned long)MEDIA_DRIFT_SYSTICK_RELOAD(),(unsigned long)configTICK_RATE_HZ,
        (unsigned long)CONFIG_TIMER_SCLK_FREQ,(unsigned)configUSE_TICKLESS_IDLE);
}
#endif
