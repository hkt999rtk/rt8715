# 恢复 timing 起点，保持 video 同轴，增加音频证据

## 已批准范围

客户反馈 move-lag10 的视频拖动正常，但音频持续沙沙声；之前版本音频顺畅。本次作为受控回归测试：恢复 timing 原厂启动整数秒 baseline，让 video 减去同一个 baseline，保留当前帧时间戳修复。音频 RTP timestamp 仍以 PCM sample 数递增，不人为平移，不改声道、解码、重采样或发送节奏。

目前日志没有音频收送统计，不能据此断定丢包。静态审查确认音频 RTP 构包不直接调用 video 时间接口，车机的时间解释仍需实测。

## 修改与验证

- `AIRPLAY_TIMER_SHARED_ORIGIN=0` 保留 Timer_Start 捕获的整数秒；`SCREEN_TIMER_ORIGIN=1` 让 Configure 和普通 video 共用该起点。原始单调 uptime 接口保持不变。
- baseline 状态独立于 profile，重连会更新；开始前回退到 uptime 并计数，防止无符号下溢。统计辨别跨会话时间重置。
- `MEDIA_ORIGIN` 报告 baseline、video_align、初始化前调用及下溢；`MEDIA_BASIS` 同时报告相对 uptime 与相对 session 的误差。后者正常应接近 0。
- `AUDIO_TX_EVIDENCE=1` 仅在当前 v17 原生音频的明确调用点增加只读观测。验证目标对象 hash/重定位，未知版本拒绝修改。
- 音频记录 ring 拒收/空读、RTP 序号及 sample 增量、sendto 结果/间隔/耗时，使用现有统计线程输出。热路径不分配、不打印、不重试，不改原函数参数或返回值。
- 主机测试覆盖初始化、整数秒与小数、重连、系统日期跳变和时钟回卷；原厂 Thumb 指令测试覆盖 timing wire 计算和当前帧 header。
- 使用外部 Realtek 工具链增量构建 IS，LP 保持既有有效产物，检查最终 ELF 与镜像 hash。

## 对照镜像

视频正常、音频有杂音版本备份：`/tmp/rt8715-audio-clock-20261111/before/flash_is.bin`

SHA256：`70ff4932f7bf5ac52a257d5e20c8211ff09a63b5504e463cd00172c90ecc9405`

客户需要同时检查拖动延迟和持续音乐杂音，并保留连接开始的完整日志。sendto 成功只代表本机 stack 接受，不证明车机收包或播放正常；新统计不能替代车机端抓包。

## 构建与验证结果

- 时钟测试：profile/shared_origin/video_align 八种组合及 archive 保留检查通过；覆盖开始前采时、非整数秒重连、跨会话不误计倒退。
- 原厂 Thumb 指令验证：Timer_Start/RTCP 跨秒计算、当前帧 header 更新顺序通过。
- 音频测试：只修改七个调用重定位，所有原始指令/数据及其他 archive 成员保持一致。覆盖序号和 sample 回卷、变长 PCM、ring 拒收/空读、短发送/失败、fd/线程复用及正在发送时切换 generation。
- 外部工具链 `make -j8 ram_is` 成功，LP AXF 已存在；最终 ELF 核对 timing 保留捕获秒、video 调用对齐入口、音频七个观测调用点及原厂构包仍在。
- `git diff --check` 通过。构建仅出现既有 awk 转义和 submake jobserver 警告。
- 独立时钟代码 review 未发现阻挡问题。以上均为主机/静态验证，尚未进行客户硬件音频听测。

构建参数：

```text
MOVE_LAG_PROFILE=1 SCREEN_SESSION_FIX=0 SCREEN_SETUP_EVIDENCE=1
AIRPLAY_TIMER_MONOTONIC=1 AIRPLAY_TIMER_CLOCK_PROFILE=1
AIRPLAY_TIMER_SHARED_ORIGIN=0 SCREEN_TIMER_ORIGIN=1 SCREEN_CURRENT_TIMESTAMP=1
AUDIO_TX_EVIDENCE=1
```

固件：`project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`

大小：8,298,496 bytes。

SHA256：`e312050a9cc7d38e312bced8b84e31d5db51e3c4c5a374267457d09d7b754fd5`

验证记录及最终函数反汇编：`/tmp/rt8715-audio-clock-20261111/`。

### 新日志判读

- `MEDIA_ORIGIN`: `shared_origin=0 video_align=1`；baseline 是实际 Timer_Start 整数秒，重连会更新。
- `MEDIA_BASIS`: `tx_minus_uptime_us` 出现负 baseline 秒是本版设计行为；应看 `tx_minus_session_us` 是否接近 0。video 的 age 同样使用 session 轴计算。
- `AUDIO_TX`: stream=100 音乐 / 101 导航；rate/framebytes 为实际格式，tsbad 验证 timestamp 增量是否等于前一包 PCM sample 数，支持自然回卷。
- `AUDIO_TX_RING`: 水位/容量单位 bytes；时长 = bytes / framebytes / rate。`sampled=1` 表示操作前后采样，受并发影响，不能把差值当作原子操作结果。`rejected_bytes` 才是 push 返回失败的实际拒收量。空读计数单独增加不一定代表可听 underrun。
- `AUDIO_TX_NET`: sends/ok/error/short 区分本机 stack 接受或拒绝；gap 和 send 耗时区分送包节奏与 socket 卡顿；wirebad 验证构包到送出前 header/长度未异常。没有车机确认，不能把 ok 解释为播放成功。
- 各音频统计按 stream/gen 累计，重新启动输出线程会进入新 gen。`untracked` 应为 0；非零时先检查观测匹配，不能据此断定丢包。
