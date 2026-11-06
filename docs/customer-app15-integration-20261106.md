# 客户 carplay_app_15 集成检查

日期：2026-11-06。已集成并生成测试固件；尚未进行设备播放验证。

## 输入与差异范围

- 输入：`/home/kevin/carplay_app_15.rar`，包含 14 个静态库，没有源码。
- 解包目录：`/home/kevin/work/carplay_app_15_20261106`。
- 集成前备份：`/home/kevin/work/carplay_app_before_15_20261106`。
- 集成目录：`project/realtek_amebapro_v0_example/GCC-RELEASE/carplay_app/`。
- 比较基准为集成前的 v14 加 11 月 5 日补交版本，并非当前 Git HEAD。原有未提交工作保留。

14 个库中只有以下 3 个改变，其他 11 个逐字节相同：

| 库 | 改变的对象 | 结论 |
| --- | --- | --- |
| lib_CarPlay.a | AudioConverter_FDKAAC_OPUS.o | 补齐 PLC 状态、格式、容量和 Reset 检查；其余 190 个对象相同 |
| lib_link.a | libwireless.o | wireless 连接状态超时门槛从 10/15 秒改为 30/60 秒 |
| lib_SystemLibEx.a | Log.o | 仅版本字符串从 20261105105617_EFA1 改为 20261105172459_096B |

libwireless.o 只有 `.text.lib_wireless_thread` 改变：函数偏移 `+0xa6` 的 10000 改为 30000，`+0xbe` 的 15000 改为 60000。对应 Wi-Fi 与 CarPlay 连接超时路径；`vTaskDelay(10)` 不变。这不是音视频发送等待时间，也不是 TCP RTO 修改。

## Converter 审查结果

根据新对象的反汇编与重定位，已包含以下参考源码逻辑：

- [AudioConverter_FDKAAC_OPUS.c:112](/home/kevin/work/customer_src/AudioConverter_FDKAAC_OPUS.c:112)：PLC 仅允许 44.1/48 kHz、480 samples、mono AAC-ELD，验证对应 PCM 输出格式。
- [AudioConverter_FDKAAC_OPUS.c:130](/home/kevin/work/customer_src/AudioConverter_FDKAAC_OPUS.c:130)：通过 FDK stream info 验证实际采样率、声道和帧长。
- [AudioConverter_FDKAAC_OPUS.c:143](/home/kevin/work/customer_src/AudioConverter_FDKAAC_OPUS.c:143)：PLC 使用正常解码的同一个 handle；要求 plcReady，容量按 samples 校验，调用 AACDEC_CONCEAL；PLC 自身不能建立首次解码状态。
- [AudioConverter_FDKAAC_OPUS.c:317](/home/kevin/work/customer_src/AudioConverter_FDKAAC_OPUS.c:317)：ELD 正常解码前检查输出容量，检查 Fill 返回值及未消耗数据，验证实际格式后维护 PLC 状态。
- Reset 清除 plcReady，SetParam 失败时停止；4096-byte 临时 PCM 数组向 DecodeFrame 传入 2048 samples，避免字节数与 sample 数混淆。
- New 在发布 converter 之前处理配置失败；Dispose 有空指针保护。

新增的私有字段使结构分配从 32 增为 108 bytes，plcReady 位于偏移 104。旧的导航二进制补丁仍禁用，避免按过时的私有布局访问。

本次没有修改客户参考 `.c`。上述检查确认关键逻辑已进入客户对象，但不代表已证明所有运行路径无缺陷。

## SDK 配套与保留项

- 更新关键对象 SHA256 校验及 Makefile 版本说明，未知对象仍会拒绝构建。
- 保留此前添加的 `--wrap=aacDecoder_GetStreamInfo` 及 handle 路由。新 converter 需要此接口，不能直接把 SDK router handle 传给原始 FDK。
- 最终 ELF 已确认 `CustomerConverterELDOutputMatches` 调用 `__wrap_aacDecoder_GetStreamInfo`，FDK 路由取出内部 handle 后再调用真实接口。
- 最终 ELF 已确认 PLC 调用 `__wrap_aacDecoder_DecodeFrame`，容量 480 samples、flags=1；Reset 容量为 2048 samples。
- AirPlayUtils.o 与已修复释放顺序的补交版相同；AirPlayAudio.o、AirPlayScreen.o 未变，因此 SDK 音讯 pacing/优先级、屏幕 4KB 分批发送等配套仍保留。
- 原始 vendor 库直接复制，不对其原文件追加二进制修改；构建时按现有规则生成派生库。

## 验证与固件

- 14 个集成后的 vendor 库与解包文件 SHA256 逐一一致。
- 客户参考代码的 8 项单元测试通过；这是参考 C 与 stub 测试，不是客户二进制的设备测试。
- 屏幕分批发送与导航输出集成测试通过，覆盖日志开/关两种配置。
- 确认现有 LP ELF 后，使用指定 Realtek GCC 6.4.1 执行 `make -j8 ram_is`，退出码 0；没有运行并行 `make all`。
- 构建中的原生导航对象校验、WLAN zero-copy 与 A-MSDU return path 最终 ELF 校验通过。
- 构建及对象比较记录：`/tmp/carplay15-review/`。

固件：`/home/kevin/work/rt8715/project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`

大小：8,298,496 bytes。SHA256：

```text
8523d88943f4f352ab7fc754951e8a8c0fe005310643e95b38891f8a72b76e06
```

变更库 SHA256：

```text
400c05af3a29e7683a6c79d8eaa637c0bb63f0f1b0459ecf316b0ad1bb35ce87  lib_CarPlay.a
e72bef24a8a823d70eff4d765d1118338588d70723a39fab6ee826fc2849b97b  lib_link.a
c554ff045450dc013b9f689a061d7f32876616465ed209513d25a8f6089510b5  lib_SystemLibEx.a
```

未执行 commit / push。

## 复核：客户反馈已修改音讯优先级、发送节奏及 4KB 分批

重新从原始 RAR 解包到独立目录 `/tmp/carplay15-recheck.20B9Go`，未复用上次提取的对象。14 个库全部与集成目录逐字节一致；包内只有一份 `lib_Accessory2.a`，其中两个相关对象各只有一个。扫描全部 14 个库的定义符号，相关发送函数只由该库提供。

包 SHA256：`7e800e4201a4d56929bcc37e141a6a293ea32c886e902ebe9b0a8b9ebdcbc6b5`。

`lib_Accessory2.a` SHA256：`0ad23e5f2d5b2e2e064975df1180d4a8fd96e6dfcc4cf5b5fd70a6f678328b37`。

### 原始库中的直接证据

1. **输出优先级**：`AirPlayAudio_StartAudio+0x220` 为 `movs r1,#4`，`+0x224` 调用 `pthread_attr_setpriority`，之后 `+0x24a` 创建 AudioOutput thread。不是后面的 AudioInput 创建路径。`lib_SystemLib.a/stdc.o` 的 `pthread_attr_setpriority` 直接将该参数传给 `pthread_attr_setschedparam`，没有在此将 4 改为 5。
2. **发送节奏**：`AirPlayAudio_AudioOutputThread+0x78` 使用常量 50 计算积存门槛；`+0x118` 读取 RingBuffer_size，`+0x11c` 比较门槛。超出时 `+0x120..0x124` 将期限重置为当前时间；否则 `+0x148..0x156` 累加一个包的媒体时长。随后按期限调用 vTaskDelay(1)。这里仍是旧的 50ms 分支，不是已交付的导航渐进 pacing。这个结论来自控制流，而非仅凭找不到 nav_pace 符号。
3. **屏幕写入长度**：`AirPlayScreen_SendScreenNormalFrame+0xb8..0xc2` 将剩余长度限制为 `0xFFFFFF`，`+0xc8` 调用 lwip_write，没有 4096 限制。原生 250ms poll 则已经存在。

交付源码中相应修正在 [AirPlayAudio.c:341](/home/kevin/work/customer_src/AirPlayAudio.c:341)、[AirPlayAudio.c:419](/home/kevin/work/customer_src/AirPlayAudio.c:419)、[AirPlayAudio.c:638](/home/kevin/work/customer_src/AirPlayAudio.c:638) 及 [AirPlayScreen.c:458](/home/kevin/work/customer_src/AirPlayScreen.c:458)。不能据此断言客户工作目录没有修改；只能确认上述修改没有进入本次收到的这份库。可能是条件编译、旧对象复用或打包来源不同，尚无证据区分。

最终 map 显示发送对象来自 `carplay_app/build/lib_Accessory2_music_diag.a`，这是 SDK 构建的派生库；当前固件中的 pacing、priority=5 和 4KB helper 不等于原始客户库已经实现。暂时保留配套，不改动固件。

请客户核对上述库的 SHA256，确认修改的两个 `.c` 是否确实编入 `lib_Accessory2.a`。如 hash 相同，可提供这两个文件的实际编译命令及对应 `.o`，定位修改被遗漏在哪一步；无需重复修改已正确编辑的源文件。
