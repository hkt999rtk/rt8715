#include "AudioFeedback.h"
#include <string.h>
#define BASE_NS 40000000ULL
#define SCALE 1000000000LL
#define STALE_MS 15000U
void AudioFeedback_Init(struct audio_feedback *f,uint32_t rate)
{
    memset(f,0,sizeof(*f));f->rate=rate;f->target_ns=BASE_NS;
}
static void clock_reset(struct audio_feedback_clock *c)
{
    c->have=c->count=c->head=c->ready=0;c->hal_us=c->rtos_us=0;c->rejected++;
}
void AudioFeedback_ClockObserve(struct audio_feedback_clock *c,uint32_t hal,uint32_t tick,int valid)
{
    uint64_t dh,dr;int64_t ppb;
    if(!valid){clock_reset(c);return;}
    if(c->have) {
        uint32_t h=hal-c->last_hal,t=tick-c->last_tick;
        /* 跨回绕用无符号差分。长时间无调用/计数器重置后重新获取。 */
        int64_t error=(int64_t)t*1000-h;
        if(error<0)error=-error;
        if(h>STALE_MS*1000U || t>STALE_MS ||
           (h>=1000000 && error>(int64_t)h/100+2000))clock_reset(c);
        else {c->hal_us+=h;c->rtos_us+=(uint64_t)t*1000;}
    }
    c->last_hal=hal;c->last_tick=tick;c->have=1;
    if(c->count) {
        unsigned last=(c->head+c->count-1)%AUDIO_FEEDBACK_POINTS;
        if(c->hal_us-c->h[last]<1000000)return;
    }
    if(c->count==AUDIO_FEEDBACK_POINTS){c->head=(c->head+1)%AUDIO_FEEDBACK_POINTS;c->count--;}
    unsigned pos=(c->head+c->count)%AUDIO_FEEDBACK_POINTS;
    c->h[pos]=c->hal_us;c->r[pos]=c->rtos_us;c->count++;
    dh=c->hal_us-c->h[c->head];dr=c->rtos_us-c->r[c->head];
    if(c->count<16 || dh<30000000){c->ready=0;return;}
    /* 窗口最多32点，单次间隔不超过15秒，乘法保持在int64范围内。 */
    ppb=((int64_t)dr-(int64_t)dh)*SCALE/(int64_t)dh;
    if(ppb>10000000 || ppb< -10000000){clock_reset(c);return;}
    c->rtos_per_hal_ppb=(int32_t)ppb;c->ready=1;
}
static void clear_window(struct audio_feedback *f)
{
    f->head=f->count=f->have=0;f->sum=0;
}
void AudioFeedback_Pause(struct audio_feedback *f)
{
    clear_window(f);
}
int AudioFeedback_Observe(struct audio_feedback *f,const struct audio_feedback_clock *c,uint32_t tick,float car_rate)
{
    uint32_t normalized,raw;
    /* NaN/Inf/零值通过有序范围比较统一拒绝，转换成整数前完成检查。
     * 无有效本地比例时保持上一次目标；不把HAL秒直接当RTOS秒。 */
    if(!f->rate || f->rate>192000 || !c->ready ||
       (uint32_t)(tick-c->last_tick)>STALE_MS ||
       !(car_rate>=(float)f->rate*0.98f && car_rate<=(float)f->rate*1.02f)) {
        f->rejected++;clear_window(f);return 0;
    }
    if(f->have && (uint32_t)(tick-f->last_tick)>STALE_MS)clear_window(f);
    if(f->have && (uint32_t)(tick-f->last_tick)<1000)return 0;
    raw=(uint32_t)((double)car_rate*1000.0+0.5);
    /* samples/HAL秒 ÷ (RTOS秒/HAL秒) = samples/RTOS秒。 */
    normalized=(uint32_t)((uint64_t)raw*1000000000ULL/(uint64_t)(SCALE+c->rtos_per_hal_ppb));
    if(normalized<(uint64_t)f->rate*990 || normalized>(uint64_t)f->rate*1010) {
        f->rejected++;clear_window(f);return 0;
    }
    f->raw_milli=raw;f->normalized_milli=normalized;
    f->have=1;f->last_tick=tick;
    if(f->count==AUDIO_FEEDBACK_POINTS){f->sum-=f->values[f->head];f->head=(f->head+1)%AUDIO_FEEDBACK_POINTS;f->count--;}
    unsigned pos=(f->head+f->count)%AUDIO_FEEDBACK_POINTS;
    f->values[pos]=normalized;f->ticks[pos]=tick;f->sum+=normalized;f->count++;
    if(f->count<16 || (uint32_t)(tick-f->ticks[f->head])<30000)return 0;
    /* sum始终保留总和，平均值使用临时变量，不能原地除count后继续累加。 */
    uint64_t average=(f->sum+f->count/2)/f->count;
    uint64_t next=(BASE_NS*f->rate*1000+average/2)/average;
    if(next<39600000ULL || next>40400000ULL){f->rejected++;clear_window(f);return 0;}
    f->target_ns=next;f->updates++;return 1;
}
