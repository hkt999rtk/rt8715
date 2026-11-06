# 客户 carplay_app_14 集成检查

日期：2026-11-05。结论：已集成并生成测试固件，但客户尚未移入全部源码修正。

## 最新补交版本：2026-11-05 12:40

输入为 `/home/kevin/lib_CarPlay.a`。本节是最新状态，后文保留首版 v14 的审查记录，不能将首版的释放顺序缺口继续视为当前版本的缺口。

- 新库 SHA256：`965d7b467690649f9b49fef85ad3806beba57282d96878ae7c437e6db5ace69d`。
- 191 个 archive member 中只有 `AirPlayUtils.o` 改变。其代码变化仅在 `RTPJitterBufferFree()`；其余函数字节一致，其他重定位按符号归一化后也一致。符号表/字符串表及两个函数的重定位索引有相应变化，不代表新增行为修改。
- **上次指出的 decoder/logQueue 释放顺序已修正**：先设置 decodeDone、唤醒并等待 decoder 结束，再排空和释放 logQueue。对应已交付参考代码 [AirPlayUtils.c:609](/home/kevin/work/customer_src/AirPlayUtils.c:609)，顺序说明见 [第 636 行](/home/kevin/work/customer_src/AirPlayUtils.c:636)。
- 新对象内证据：Free 的 `+0x18` 设置 decodeDone，`+0x1c` signal，`+0x28..0x42` 等待线程停止，之后 `+0x96` dispatch_sync_f、`+0xa0` dispatch_release。最终固件 ELF 也已核对该顺序。
- `AudioConverter_FDKAAC_OPUS.o` 完全未变，后文 PLC 初始化状态及格式检查的缺口仍然存在；参考实现位置见后文第 2 项。
- 本次没有新 `lib_Accessory2.a`，后文音讯/屏幕源码迁移缺口仍然存在；SDK 保留的发送策略不变。
- 更新了关键对象版本校验；集成前库备份在 `/home/kevin/work/carplay_app_before_followup_20261105/lib_CarPlay.a`。
- `/home/kevin/work/customer_src` 所有文件前后 SHA256 一致，本次没有修改客户参考代码。

确认现有 LP ELF 存在后，使用指定 Realtek 工具链执行 IS 增量构建，退出码 0。最终链接检查通过：修正后的 Free 顺序、PLC 到 FDK wrapper、屏幕 4KB helper/poll、音讯 pacing 均保留。这里只能确认静态代码与构建结果，尚未进行设备播放测试。

最新固件：`/home/kevin/work/rt8715/project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`，8,298,496 bytes。

最新固件 SHA256：

```text
ee5790195e77e36a8ddee2d0bc4daafcc7fad19ede6ecfea8122032afd165148
```

构建/比对记录：`/tmp/carplay-followup-review/`，最终构建日志 `build-final.log`。未 commit / push。

## 以下为首版 v14 审查记录

## 范围与备份

- 输入：`/home/kevin/carplay_app_14.rar`，仅含 14 个静态库，没有源码。
- 原始解包：`/home/kevin/work/carplay_app_14_20261105`。
- 集成前备份：`/home/kevin/work/carplay_app_before_14_20261105`。
- 集成目录：`project/realtek_amebapro_v0_example/GCC-RELEASE/carplay_app/`。
- 14 个 vendor 库与解包版本逐一 SHA256 比对一致；二进制补丁只生成派生库。
- 依据：GNU ar/objdump 对象、函数、重定位对比，以及最终 ELF 检查。没有硬件运行验证；不能将本报告视为断音问题已解决。

## 已移入及本次集成处理

| 项目 | 检查结果与处理 |
| --- | --- |
| 导航 PLC | 新增原生 `AudioConverterConcealPCM`，使用同一个 FDK decoder handle；最终链接调用 `__wrap_aacDecoder_DecodeFrame` |
| 导航连续性 | AirPlayUtils 已包含连续性缓存、启动/purge 保护、带条件的线程唤醒、RTP 时间戳回绕处理及部分初始化检查 |
| 缓冲时长 | Setup 将 type 100/101 的 `audioLatencyMs` 下限由 300ms 提高到 400ms；这是协商参数下限，不代表每时刻实际库存都有 400ms |
| 旧导航二进制补丁 | 默认关闭 NAV_PURGE_FIX / NAV_PLAYOUT / NAV_CONTINUITY / NAV_DIAG，防止重复处理及访问过时的私有结构偏移；禁止与原生模式同时启用 |
| PCM 发送 | AirPlayAudio.o 未改变，保留 SDK 发送节奏及音讯发送优先级修正 |
| 屏幕发送 | AirPlayScreen.o 未改变，原有 `lwip_poll(...,250)` 本来就存在；保留 SDK 4KB 分批写入及 TCP 恢复处理 |
| 日志 | `AUDIO_DIAG_LOG=0`，保留策略所需 hook，不开启高频诊断输出 |
| 兼容性校验 | 新增四个关键对象的 SHA256 校验，未知版本必须重新审查 |

未叠加额外的 RTPJitterBufferInit wrapper：客户的 400ms 下限已覆盖此前 300ms 加 30ms 的导航裕量。

两处旧脚本只增加经过审查的版本签名：

1. `patch_carplay_music_lifecycle.py`：Setup 仅在函数偏移 `0x10fa/0x1108` 改变 300→400 的立即数，调用点及重定位保持一致。
2. `patch_carplay_ui_aligned_archive.py`：Accessory.o 修改蓝牙提示字符串及对应 addend；转换/编码调用布局与 mono 版本一致，继续使用其偏移。

## 客户尚未移入的修正

### 1. decoder 与 logQueue 释放顺序

新版 `RTPJitterBufferFree` 仍先同步、释放并清空 logQueue，再通知并等待 decoder 退出。

- Free 函数偏移 `+0x58` 调用 dispatch_sync_f，`+0x62` 释放 queue，`+0x66` 清空。
- decoder 停止流程从 `+0x6a` 开始，`+0x7c` 设置 decodeDone，后续 signal/wait。
- decoder 线程仍有调用 RTPJitterBufferLog 的路径。

这存在退出并发竞争风险，不是本次日志已证实的运行时崩溃。**此修正已包含在交付源码中**，但新库没有同步：参考 [AirPlayUtils.c:609](/home/kevin/work/customer_src/AirPlayUtils.c:609) 的 `RTPJitterBufferFree()`。第 613–634 行先通知并等待 decoder 退出；[第 636 行](/home/kevin/work/customer_src/AirPlayUtils.c:636) 的中文注释说明顺序要求；第 648–651 行才排空并释放 logQueue。本次不凭未知私有布局追加二进制改写。

### 2. PLC 接口防御条件不完整

新 `AudioConverterConcealPCM` 已检查 AAC-ELD、44100/48000Hz、mono、handle 及容量，并以 AACDEC_CONCEAL 调用 FDK。但未看到明确的“至少成功解码过一帧”状态检查，也没有完整验证输出 PCM 格式和配置帧长。

正常 jitter 调用路径已有部分格式及连续性保护，因此不能直接判定所有 PLC 调用都有问题；但接口本身尚未完整实现参考代码要求。

**此部分已经提供参考实现，但没有客户私有 converter 源码，不能说已经替客户改好该 `.c`。** 对照位置：

- [AudioConverterConcealFDK.inc:27](/home/kevin/work/customer_src/AudioConverterConcealFDK.inc:27)：完整 `AudioConverterConcealPCM()` 函数体。第 38 行调用格式/初始化检查，第 39 行取得正常解码使用的同一个 handle，第 44 行执行 FDK PLC。
- [INTEGRATION.md:72](/home/kevin/work/customer_src/INTEGRATION.md:72)：`CustomerConverterCanConceal()` 参考实现；第 77 行检查 `plcReady`，第 83–95 行检查输入帧长和输出 PCM 格式。
- [INTEGRATION.md:99](/home/kevin/work/customer_src/INTEGRATION.md:99)：两个宏映射，第 107 行包含 `.inc`。
- [INTEGRATION.md:110](/home/kevin/work/customer_src/INTEGRATION.md:110)：`plcReady` 创建/reset 清零、真实帧成功后置位及格式不匹配清零的规则。
- [AirPlayUtils.c:990](/home/kevin/work/customer_src/AirPlayUtils.c:990)：jitter buffer 调用该接口的位置。

客户应把上述参考实现接到其真实 `AudioConverter_FDKAAC_OPUS.c`，按实际私有字段映射；不能直接照抄示例字段名。该客户文件尚未提供，因此这里不虚列它的行号。

### 3. 音讯/屏幕源码修改未全部迁移

AirPlayAudio.o、AirPlayScreen.o 与集成前完全相同。SDK 侧仍承担发送节奏、优先级及 4KB 限制；不能因此宣称客户已原生实现这些功能。此前源码交付中的分配失败重试/复用、屏幕 write 返回 0、停止条件及资源清理等修改，也需对照这两个源文件确认。

**以下均已写入交付给客户的 `.c`，可直接参照迁移：**

| 修改 | 交付源码参考位置 | 具体逻辑 |
| --- | --- | --- |
| 导航发送节奏 | [AirPlayAudio.c:341](/home/kevin/work/customer_src/AirPlayAudio.c:341)、[AirPlayAudio.c:419](/home/kevin/work/customer_src/AirPlayAudio.c:419) | 在指定导航 PCM 格式启用 pacer；sendto 耗时计入期限，等待支持停止，不靠丢 PCM 调整节奏。算法在 [AirPlayNavPacing.h:88](/home/kevin/work/customer_src/AirPlayNavPacing.h:88) |
| 包缓冲复用与 OOM | [AirPlayAudio.c:381](/home/kevin/work/customer_src/AirPlayAudio.c:381)、[AirPlayAudio.c:457](/home/kevin/work/customer_src/AirPlayAudio.c:457) | pop PCM 前先确保包缓冲存在；OOM 重试不消费 PCM，每线程复用缓冲，退出释放 |
| 音讯输出优先级 | [AirPlayAudio.c:638](/home/kevin/work/customer_src/AirPlayAudio.c:638) | AudioOutput priority=5，高于 ScreenThread priority=4 |
| 画面 4KB 分批 | [AirPlayScreen.c:458](/home/kevin/work/customer_src/AirPlayScreen.c:458) | 每次 write 最多 4096 bytes，按实际写入量推进指针 |
| 发送循环停止条件 | [AirPlayScreen.c:464](/home/kevin/work/customer_src/AirPlayScreen.c:464) | 每轮同时检查 remaining 与 bScreenSend |
| write 返回 0 | [AirPlayScreen.c:495](/home/kevin/work/customer_src/AirPlayScreen.c:495) | 非空写入返回 0 时停止发送，避免无限空转 |
| StopVideo 清理 | [AirPlayScreen.c:743](/home/kevin/work/customer_src/AirPlayScreen.c:743) | 已停止发送但 socket/队列仍存在时继续清理；控制连接断开也释放本地资源 |
| 原有 poll 及 USB RTO 接入 | [AirPlayScreen.c:371](/home/kevin/work/customer_src/AirPlayScreen.c:371)、[AirPlayScreen.c:467](/home/kevin/work/customer_src/AirPlayScreen.c:467) | 250ms poll 已在旧库存在；参考源码保留它，并在等待/写入前启用 USB 专用 RTO，不能将原有 poll 误列为本版新增 |

以上行号以 2026-11-05 本机 `/home/kevin/work/customer_src` 文件为准，指向修改版文件，不是 `original/` 备份。客户文件行号可能不同，应以函数名和逻辑定位。本次只补报告，没有再次修改交付源码。

建议客户补交本版 `AirPlayUtils.c/.h`、`AudioConverter_FDKAAC_OPUS.c`（及私有头文件）；继续完整迁移时还需 `AirPlayAudio.c`、`AirPlayScreen.c`。

## 验证与产物

- 按仓库规则顺序执行 clean、`make -j1 -B ram_lp`、`make -j8 ram_is`；最终构建退出码 0。
- 使用指定 Realtek GCC 6.4.1 工具链，Python 文件系统依赖来自 `/tmp/rt8715-python-deps`。
- nav output integration 测试通过（日志开/关两种配置）。
- music lifecycle 测试通过：指令/重定位、ABI、64 位返回值、尾调用参数保持。
- music progress patch 测试通过：原始代码数据保持，未知版本拒绝。
- screen batch 测试通过：部分写入、EAGAIN、4096 上限、错误及零返回处理。
- 最终 ELF：原生 PLC → FDK wrapper；屏幕 → 4KB helper（经 veneer）及 250ms poll；音讯 pacing 已链接；旧导航 convert/condition/Init wrapper 未链接。
- 构建日志：`/tmp/carplay14-review/build-final.log`；比对材料：`/tmp/carplay14-review/`。

固件：`/home/kevin/work/rt8715/project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`

SHA256：

```text
b79f3ca638a4bb0f85d279358b4af5d6558c1fb92f1704bb7de326c63b6b4a0a
```

本次未 commit / push。固件用于整合验证，剩余源码缺口仍需处理。
