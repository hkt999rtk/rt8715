#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "AudioSamplePacer.h"
int main(void)
{
    struct audio_sample_pacer p;
    uint32_t now=0;
    AudioPacer_Init(&p,now,44100);
    /* move-lag13 的目标周期：原 unsigned 算法第8次会停止等待。 */
    for(unsigned i=0;i<12000;i++) {
        AudioPacer_Plan(&p,now,1764,40148072);
        uint32_t wait=AudioPacer_WaitMs(&p,now);
        assert(wait>=39 && wait<=42);
        now+=wait;assert(AudioPacer_WaitMs(&p,now)==0);
    }
    assert(p.applied_ns==40148072 && p.target_ns==40148072);
    assert(p.rebases==0 && p.rejected==0);
    /* 未来的期限不能被当成过期。 */
    p.deadline_ns=p.now_ms*1000000ULL+41000000;
    assert(AudioPacer_WaitMs(&p,now)==41);
    /* 毫秒回绕、48K mono/stereo 共用 frame 数；任意块长保留余数。 */
    AudioPacer_Init(&p,UINT32_MAX-20,48000);now=UINT32_MAX-20;
    AudioPacer_Plan(&p,now,1920,40000000);now+=40;
    assert(AudioPacer_WaitMs(&p,now)==0 && p.now_ms>UINT32_MAX);
    AudioPacer_Init(&p,0,44100);
    for(unsigned i=0;i<44100;i++)AudioPacer_Plan(&p,0,1,40000000);
    assert(p.deadline_ns==1000000000ULL);
    /* 长时间停顿不积累突发补发或一次性调速额度。 */
    AudioPacer_Init(&p,0,44100);AudioPacer_Plan(&p,0,1764,40148072);
    now=3600000;AudioPacer_Plan(&p,now,1764,40148072);
    assert(p.applied_ns<=40000080 && p.rebases==1);
    assert(AudioPacer_WaitMs(&p,now)>=40);
    /* 超范围/零目标拒绝，不污染有效目标；降速/升速都受斜率限制。 */
    uint64_t target=p.target_ns;
    AudioPacer_Plan(&p,now,1764,0);assert(p.rejected==1 && p.target_ns==target);
    AudioPacer_Plan(&p,now,1764,50000000);assert(p.rejected==2 && p.target_ns==target);
    AudioPacer_Init(&p,0,48000);now=0;
    uint64_t before=p.applied_ns;
    for(unsigned i=0;i<12000;i++){
        AudioPacer_Plan(&p,now,1920,39880000);
        assert(p.applied_ns<=before && before-p.applied_ns<=34);
        before=p.applied_ns;now+=AudioPacer_WaitMs(&p,now);
    }
    assert(p.applied_ns==39880000);
    /* 48小时：可变目标仍保留精确累计相位，不随调用数积累舍入误差。 */
    AudioPacer_Init(&p,UINT32_MAX-1000,44100);now=UINT32_MAX-1000;
    uint64_t expected=p.deadline_ns;
    for(unsigned i=0;i<4320000;i++) {
        uint64_t wanted=i<2160000?40148072:39880000;
        AudioPacer_Plan(&p,now,1764,wanted);expected+=p.applied_ns;
        assert(p.deadline_ns==expected && p.rebases==0);
        now+=AudioPacer_WaitMs(&p,now);
    }
    assert(p.applied_ns==39880000);
    puts("sample pacing: fractional deadlines, 44100/48000, future/wrap, +/-slew, pause/reject PASS");
}
