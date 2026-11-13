# carplay_app_19 整合记录

## 输入与范围

用户要求整合 `~/carplay_app_19.rar` 并移除不需要的wrapper。14个原始archive完整安装；除archive容器元数据外，实际member变化仅Accessory2的音频、时钟对象及SystemLibEx的版本字符串。原始vendor库不做就地patch；诊断仍生成独立派生archive。

审查/诊断构建目录：`/home/kevin/work/rt8715-clock-drift`；已同步全部必要源码与原始库到主工作目录 `/home/kevin/work/rt8715`。主目录使用默认生产配置构建。

## 二进制核对

- `AudioFeedback.o` 全部4个函数、`AudioSamplePacer.o` 全部3个函数的机器码与11月13日兼容性修订源码，用Realtek GCC 6.4.1同架构/ABI/Os编译后的结果逐字节相同。
- `AirPlayAudio.o` 只有 StartAudio、SendAudio、FeedBack 的代码和新增私有状态发生变化；AudioOutput、mic、RTP packetizer、加密、StopAudio代码保持v18字节不变。
- SendAudio +0xcc唯一RingBuffer_push，之后Plan与WaitMs、最多5ms vTaskDelay；检查stop、generation；失败push不刷新活动。Start重置新会话；Feedback配对HAL/RTOS并持续更新目标，旧一次性锁频流程已退出。
- `mainAudioStream`/`altAudioStream`仍各720bytes；新audioPacing两路共816bytes，audioFeedbackClock560bytes。
- Screen/Timer的代码未变，调用改名为AirPlayClock。新AirPlayClock与旧AirPlayMediaClock在名称正规化后代码/数据/重定位完全等价。详细证据见non-audio-audit.md。
- REA识别了ARM ELF及hash，但本会话没有配置native反编译引擎；上述代码核对来自Realtek objdump、ELF解析与实际Thumb模拟，不是REA反编译结论。

## wrapper与诊断处理

- 退役 `__wrap_AirPlayAudio_StartAudio`、`__wrap_AirPlayAudio_SendAudio`、`__wrap_AirPlayAudio_FeedBack`；移除audio_pacing_fix.c/.h及专属bridge测试，停止SDK重复编译两个纯音频模块。其源码只保留为主机回归和客户机器码对照。
- `AUDIO_PACING_SLEW_FIX=1`构建直接拒绝；旧音频诊断/pacing开关原有互斥继续保留。
- 音频只读观测保留：原来的6个输出路径hook未变，SendAudio的push hook从+0x54更新到已核对的+0xcc；逐条核对重定位，所有指令保持原样。
- screen_flow观测转送与patch改为AirPlayClock，避免拉入旧AirPlayMediaClock产生第二份时间基准。原始archive仍保留客户旧member，最终ELF禁止残留旧公开clock函数。
- AirTunesClock的Adjust/Finalize wrapper仅在MEDIA_CLOCK_DRIFT_PROFILE=1时观察，原函数仍恰好执行一次；它们不调节原生PCM或全局clock，保留用于与上次测试版对照。
- 原有网络、资源450、加密、WLAN等兼容性修正不由本次音频迁移取代。
- 删除旧AUDIO_PACING/AUDIO_FEEDBACK桥接日志；AUDIO_CLOCK_FEEDBACK仅打印原生目标周期。新控制器窗口和applied周期未导出，明确unavailable，不把旧lockFreq/count当作新控制器状态。

## 构建配置

独立诊断构建沿用测试版观测配置：`MOVE_LAG_PROFILE=1 SCREEN_SETUP_EVIDENCE=1 SCREEN_SESSION_FIX=0 AUDIO_TX_EVIDENCE=1 MEDIA_CLOCK_DRIFT_PROFILE=1`。不启用global media discipline，不启用旧audio pacing wrapper。

构建：顺序make clean，make -j1 -B ram_lp，make -j8 ram_is；指定AGENTS.md的外部Realtek工具链。镜像打包使用 `PYTHONPATH=/tmp/rt8715-python-deps` 中已安装的pyfatfs/littlefs依赖。

## 验证与边界

- 旧guard拒绝v19、旧screen forwarding无法链接新clock的回归先失败；更新后通过。
- 音频7处/画面6处重定位patch测试通过，原始指令/数据不变，重复patch被拒绝。
- 新版Clock/Timer/Screen实际Thumb执行测试通过：RTCP epoch/fraction、video header、整数起点、重连及HAL/RTOS回绕。
- feedback/pacer主机测试（含48小时模拟）通过。
- 新ELF gate在旧wrapper固件上失败，要求新固件原生唯一实现、实际调用关系、单次push、共用AirPlayClock与无旧音频wrapper。
- 独立只读审查未发现 Critical/Important 问题；核对原始库、实际链接的派生库、最终 ELF 调用链和 ABI。
- 本次未上板；仍需客户新会话音乐/导航、暂停恢复及长时间播放验证。

## 最终构建结果

默认生产配置与独立诊断配置都完成顺序 clean / LP / IS 构建，退出码为0。两者的最终 ELF 原生 v19 gate 均通过；诊断配置另通过只读观测 forwarding gate。主目录输出为默认生产配置。

- **production**：`/home/kevin/work/rt8715/project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`
  - 大小：8298496 bytes
  - SHA-256：`c1344bfac1c1eb0ba43b63310b9c50100e534212e17323fa3c9174ad1043377b`
- **diagnostic**：`/home/kevin/work/rt8715-clock-drift/project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`
  - 大小：8298496 bytes
  - SHA-256：`c0e19716a856c3f183c376fbada4f58ad69718f2ca2307ca374c46a7b36a8b7b`

构建日志保存在 `/tmp/rt8715-v19-review/`：主目录 `main-clean.log`、`main-ram_lp.log`、`main-ram_is.log`；诊断目录 `clean.log`、`ram_lp.log`、`ram_is-final.log`。

最终核对：主目录14个原始 `.a` 与客户输入逐字节相同，旧audio_pacing_fix.c/.h不存在；`git diff --check`通过。
