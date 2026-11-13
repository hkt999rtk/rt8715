#!/usr/bin/env python3
"""校验最终ELF的原生v19音频调用链、单一控制器与公共HAL时钟。"""
import re
import subprocess
import sys
from collections import Counter


def verify(objdump, nm, image):
    asm = subprocess.check_output([objdump, '-d', image], text=True)
    functions = dict(re.findall(r'^\w+ <([^>]+)>:\n(.*?)(?=^\w+ <|\Z)', asm, re.M | re.S))
    symbols = subprocess.check_output([nm, '--defined-only', image], text=True)
    definitions = Counter(line.split()[-1] for line in symbols.splitlines() if line.split())
    addresses = {parts[-1]: int(parts[0], 16) & ~1
                 for line in symbols.splitlines() if len(parts := line.split()) == 3}

    def require(condition, message):
        if not condition:
            raise SystemExit('customer v19 ELF: ' + message)

    required = ('AirPlayAudio_StartAudio', 'AirPlayAudio_SendAudio', 'AirPlayAudio_FeedBack',
                'AudioPacer_Init', 'AudioPacer_Plan', 'AudioPacer_WaitMs',
                'AudioFeedback_Init', 'AudioFeedback_ClockObserve', 'AudioFeedback_Observe',
                'AudioFeedback_Pause', 'AirPlayClock_GetTime', 'AirPlayClock_StartTime',
                'AirPlayClock_VideoTimestamp')
    for name in required:
        require(definitions[name] == 1, 'missing/duplicate native definition: ' + name)
    for name in definitions:
        require(name not in ('__wrap_AirPlayAudio_StartAudio', '__wrap_AirPlayAudio_SendAudio',
                             '__wrap_AirPlayAudio_FeedBack', 'carbox_audio_pacing_report'),
                'retired audio wrapper retained: ' + name)
        require(not name.startswith('AirPlayMediaClock_'), 'legacy clock retained: ' + name)

    def calls(name):
        targets = re.findall(r'\b(?:blx?|b(?:\.w|\.n)?)\s+\w+\s+<([^>+]+)(?:\+[^>]*)?>', functions.get(name, ''))
        result = []
        for target in targets:
            # Realtek跨RAM区调用使用16-byte Thumb veneer；核实literal目标地址，
            # 不能只凭veneer名称就当成已转送至预期函数。
            if target.startswith('__') and target.endswith('_veneer'):
                native = target[2:-7]
                body = functions.get(target, '')
                words = re.findall(r'\.word\s+0x([0-9a-f]+)', body)
                if (len(words) == 1 and int(words[0], 16) & ~1 == addresses.get(native)
                        and re.search(r'\bldr\s+r0, \[pc, #8\]', body)
                        and re.search(r'\bmov\s+ip, r0', body)
                        and re.search(r'\bbx\s+ip', body)):
                    target = native
            result.append(target)
        return result

    edges = {
        'AirPlayAudio_StartAudio': ('AudioFeedback_Init',),
        'AirPlayAudio_SendAudio': ('AudioPacer_Init', 'AudioPacer_Plan', 'AudioPacer_WaitMs', 'vTaskDelay'),
        'AirPlayResponse_GetInfoFeedbackResponse': ('AirPlayAudio_FeedBack',),
        'AirPlayAudio_FeedBack': ('AudioFeedback_ClockObserve', 'AudioFeedback_Observe',
                                'AudioFeedback_Pause', 'hal_read_curtime_us', 'GetTickCount'),
    }
    for caller, targets in edges.items():
        for target in targets:
            require(target in calls(caller), 'missing call: ' + caller + ' -> ' + target)
    for target in ('AirPlayAudio_StartAudio', 'AirPlayAudio_SendAudio'):
        require(any(target in calls(caller) for caller in functions if caller != target),
                'native caller missing: ' + target)
    push = calls('AirPlayAudio_SendAudio')
    require(push.count('RingBuffer_push') + push.count('carbox_audio_tx_ring_push') == 1,
            'producer must have exactly one PCM push callsite')

    # 生产与诊断构建均须落到同一套AirPlayClock；诊断路径只允许一次转送。
    clock_edges = (
        ('AirPlayScreen_SendScreenConfigure', 'carbox_screen_time_config', 'AirPlayClock_VideoTimestamp'),
        ('AirPlayScreen_SendScreenNormalFrame', 'carbox_screen_time_video', 'AirPlayClock_VideoTimestamp'),
        ('AirPlayTimer_Start', 'carbox_screen_time_start', 'AirPlayClock_StartTime'),
        ('AirPlayTimer_SetTimeRTCPData', 'carbox_screen_time_receive', 'AirPlayClock_GetTime'),
        ('AirPlayTimer_SetTimeRTCPData', 'carbox_screen_time_transmit', 'AirPlayClock_GetTime'),
    )
    for caller, observer, native in clock_edges:
        route = calls(caller)
        if observer in route:
            require(calls(observer).count(native) == 1, 'observer must forward once: ' + observer)
        else:
            require(native in route, 'missing shared clock: ' + caller + ' -> ' + native)
    print('customer v19 ELF: unique native feedback/pacer; direct entry calls; one PCM push; shared AirPlayClock; no retired audio wrappers')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit('usage: check_customer_v19_elf.py OBJDUMP NM ELF')
    verify(*sys.argv[1:])
