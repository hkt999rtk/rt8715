#ifndef AUDIO_FEEDBACK_H
#define AUDIO_FEEDBACK_H
#include <stdint.h>
#define AUDIO_FEEDBACK_POINTS 32
/* 纯状态模块。调用方串行化访问；HAL单位us，RTOS单位ms。
 * rtos_per_hal_ppb = (RTOS经过时间/HAL经过时间 - 1) * 1e9。
 * 仅换算本地时基，不代表iPhone物理时钟的漂移。 */
struct audio_feedback_clock {
    uint64_t hal_us,rtos_us,h[AUDIO_FEEDBACK_POINTS],r[AUDIO_FEEDBACK_POINTS];
    uint32_t last_hal,last_tick,have,count,head,ready,rejected;
    int32_t rtos_per_hal_ppb;
};
struct audio_feedback {
    uint64_t sum,target_ns;
    uint32_t rate,values[AUDIO_FEEDBACK_POINTS],ticks[AUDIO_FEEDBACK_POINTS];
    uint32_t head,count,last_tick,have,updates,rejected,raw_milli,normalized_milli;
};
void AudioFeedback_Init(struct audio_feedback *,uint32_t rate);
/* PCM暂停时丢弃学习窗口，保留最后有效目标和累计计数。 */
void AudioFeedback_Pause(struct audio_feedback *);
void AudioFeedback_ClockObserve(struct audio_feedback_clock *,uint32_t hal_us,uint32_t tick_ms,int valid);
int AudioFeedback_Observe(struct audio_feedback *,const struct audio_feedback_clock *,uint32_t tick_ms,float car_rate);
#endif
