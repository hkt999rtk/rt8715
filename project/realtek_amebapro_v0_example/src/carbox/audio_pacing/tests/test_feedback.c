#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "AudioFeedback.h"

static void pair(struct audio_feedback_clock *c, uint64_t hal, int ppb)
{
    uint64_t rtos=hal+(int64_t)(hal/1000)*ppb/1000000;
    AudioFeedback_ClockObserve(c,(uint32_t)hal,(uint32_t)(rtos/1000),1);
}
int main(void)
{
    struct audio_feedback_clock c={0};
    struct audio_feedback f;
    AudioFeedback_Init(&f,48000);
    /* audio-miss: HAL/RTOS 相差1582ppm，车机回报48075，不应导致1563ppm加速。 */
    for(unsigned s=0;s<=180;s+=2){pair(&c,(uint64_t)s*1000000,1582000);
        AudioFeedback_Observe(&f,&c,c.last_tick,48075.031f);}
    assert(c.ready && f.updates>1);
    assert(f.target_ns>39997000 && f.target_ns<40003000);
    uint64_t held=f.target_ns;
    AudioFeedback_Observe(&f,&c,181000,NAN);
    assert(f.target_ns==held && f.rejected);
    AudioFeedback_Observe(&f,&c,182000,0);
    assert(f.target_ns==held);
    /* 第二个稳定窗口必须继续更新，不能锁频，也不能把平均值再次当总和。 */
    for(unsigned s=184;s<=320;s+=2){pair(&c,(uint64_t)s*1000000,1582000);
        AudioFeedback_Observe(&f,&c,c.last_tick,48090.0f);}
    assert(f.target_ns<held-8000 && f.target_ns>39970000);
    held=f.target_ns;
    AudioFeedback_Observe(&f,&c,400000,48090.0f); /* 时钟采样已过期 */
    assert(f.target_ns==held && !f.count);
    AudioFeedback_Init(&f,48000);
    assert(f.target_ns==40000000 && !f.count);
    /* HAL 71分钟、RTOS 49天回绕都采用差分，持续48小时。 */
    c=(struct audio_feedback_clock){0};
    uint64_t start=4294960000000ULL;
    for(uint64_t s=0;s<=172800;s+=2){uint64_t h=start+s*1000000;
        uint64_t r=start+s*1001500;
        AudioFeedback_ClockObserve(&c,(uint32_t)h,(uint32_t)(r/1000),1);
        AudioFeedback_Observe(&f,&c,(uint32_t)(r/1000),48072.0f);}
    assert(c.ready && c.rtos_per_hal_ppb==1500000);
    assert(f.target_ns>=39999999 && f.target_ns<=40000001);
    /* 反向偏差也必须正确；时钟重启/巨大变化不产生有效比例。 */
    c=(struct audio_feedback_clock){0};AudioFeedback_Init(&f,44100);
    for(unsigned s=0;s<=180;s+=2){pair(&c,(uint64_t)s*1000000,-3000000);
        AudioFeedback_Observe(&f,&c,(uint32_t)(s*997),43967.7f);}
    assert(c.rtos_per_hal_ppb< -2900000 && f.target_ns>39997000 && f.target_ns<40003000);
    AudioFeedback_ClockObserve(&c,1,1,1);assert(!c.ready);
    puts("feedback: logged clock mismatch, continuous windows, invalid/stale/reset, +/-ratio, 48h/wrap PASS");
}
