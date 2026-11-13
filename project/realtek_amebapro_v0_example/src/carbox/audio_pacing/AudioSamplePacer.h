#ifndef AUDIO_SAMPLE_PACER_H
#define AUDIO_SAMPLE_PACER_H
#include <stdint.h>
/* 纯状态模块；输入是 PCM sample frames（不是 channels * frames）。
 * tick_ms 允许32位自然回绕，连续两次调用间隔必须小于49.7天。
 * 原厂 deltaTime 的单位实际为 ns/40ms PCM，本模块只渐进套用该目标。
 * 此模块不改变全局时钟、PCM 内容、RTP timestamp 或原厂 feedback 算法。 */
#define AUDIO_PACER_BASE_NS 40000000ULL
#define AUDIO_PACER_SLEW_PPM_PER_SEC 20U
struct audio_sample_pacer {
    uint64_t now_ms,last_slew_ms,deadline_ns,remainder,frames;
    uint64_t applied_ns,target_ns;
    uint32_t last_tick,rate,rebases,rejected,slew_remainder;
};
void AudioPacer_Init(struct audio_sample_pacer *,uint32_t tick_ms,uint32_t sample_rate);
int AudioPacer_Plan(struct audio_sample_pacer *,uint32_t tick_ms,uint32_t frames,uint64_t target_ns);
uint32_t AudioPacer_WaitMs(struct audio_sample_pacer *,uint32_t tick_ms);
#endif
