#include "AudioSamplePacer.h"
#include <string.h>
static void advance(struct audio_sample_pacer *p,uint32_t tick)
{
    p->now_ms+=(uint32_t)(tick-p->last_tick);p->last_tick=tick;
}
void AudioPacer_Init(struct audio_sample_pacer *p,uint32_t tick,uint32_t rate)
{
    memset(p,0,sizeof(*p));p->last_tick=tick;p->now_ms=tick;p->last_slew_ms=tick;
    p->deadline_ns=(uint64_t)tick*1000000;p->rate=rate;
    p->applied_ns=p->target_ns=AUDIO_PACER_BASE_NS;
}
int AudioPacer_Plan(struct audio_sample_pacer *p,uint32_t tick,uint32_t frames,uint64_t target)
{
    uint64_t elapsed,budget,change,den,numerator,duration,next;
    advance(p,tick);
    if(!p->rate || p->rate>192000 || !frames || frames>p->rate){p->rejected++;return 0;}
    /* 仅作为原厂目标的异常保护，不把 HAL/RTOS 的误差直接作为补偿量。
     * ±1%覆盖本次原厂0.37018%目标；越界保持上一个目标，不静默放大限幅。 */
    if(target>=39600000ULL && target<=40400000ULL)p->target_ns=target;
    else p->rejected++;
    elapsed=p->now_ms-p->last_slew_ms;p->last_slew_ms=p->now_ms;
    if(elapsed>100)elapsed=100; /* 暂停期间不积攒调速额度。 */
    budget=elapsed*(AUDIO_PACER_BASE_NS*AUDIO_PACER_SLEW_PPM_PER_SEC/1000000)+p->slew_remainder;
    p->slew_remainder=(uint32_t)(budget%1000);budget/=1000;
    if(p->target_ns>=p->applied_ns){change=p->target_ns-p->applied_ns;if(change>budget)change=budget;p->applied_ns+=change;}
    else {change=p->applied_ns-p->target_ns;if(change>budget)change=budget;p->applied_ns-=change;}
    if(p->applied_ns==p->target_ns)p->slew_remainder=0;
    den=(uint64_t)p->rate*40;
    numerator=(uint64_t)frames*p->applied_ns*1000+p->remainder;
    duration=numerator/den;p->remainder=numerator%den;
    next=p->deadline_ns+duration;
    if(next<p->now_ms*1000000ULL){next=p->now_ms*1000000ULL+duration;p->rebases++;}
    p->deadline_ns=next;p->frames+=frames;return 1;
}
uint32_t AudioPacer_WaitMs(struct audio_sample_pacer *p,uint32_t tick)
{
    /* 兼容只提供整数类型、未提供UINT32_MAX的客户标准库头。 */
    const uint32_t max_wait=(uint32_t)-1;
    uint64_t now,wait;advance(p,tick);now=p->now_ms*1000000ULL;
    if(now>=p->deadline_ns)return 0;
    wait=(p->deadline_ns-now+999999)/1000000;
    return wait>max_wait?max_wait:(uint32_t)wait;
}
