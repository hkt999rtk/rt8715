# 客户 carplay_app_18 集成检查

日期：2026-11-11。输入：`/home/kevin/carplay_app_18.rar`。

## 结论

14 个原始库已完整复制到 `project/realtek_amebapro_v0_example/GCC-RELEASE/carplay_app/`，逐个校验与解包文件一致。通过反汇编及实际 ARM 对象的模拟执行确认，客户已把时间戳修正实现到 `lib_Accessory2.a` 内。已移除对应 SDK wrapper 和二进制时间戳补丁。固件编译及下述验证通过；本次 v18 整包尚未经过客户设备实测。

## 已原生实现并移除的适配

| 客户对象 | 原生行为 | SDK 清理 |
| --- | --- | --- |
| `AirPlayMediaClock.o` | HAL 微秒计时与 RTOS epoch 组合，处理回绕；提供共享单调时钟 | 删除 `screen_hw_timestamp.c/.h` 及 `UpTicksToNTP` 链接 wrapper |
| `AirPlayTimer.o` | Start 记录整数秒起点 B；timing 使用原始单调时间，再扣 B、加 NTP epoch | 删除 `airplay_timer_clock.c/.h`、`patch_airplay_timer_clock.py` 及关联统计入口 |
| `AirPlayScreen.o` | Configure/NormalFrame 使用共享时钟减 B；NormalFrame 先采本帧时间，再写 header | 删除 `patch_screen_current_timestamp.py`，不再生成 mono/current_ts 派生库 |

保留原厂整数秒起点，不能改成强制零起点。video 的 32.32 时间不加 NTP epoch；timing 报文按原协议加 epoch。音频 RTP 仍按采样数推进，未套用 video 时间偏移。新 session 建立前仍需完成旧 session 的停止；本次未改生命周期。

旧时钟实验开关设为 1 会被 Makefile 明确拒绝，避免重复修正。增量编译兼容已删除头文件的旧 `.d` 依赖。

## 仍保留的功能

- 原生 video 每次 write 最大 4096 bytes、部分 write 推进、返回 0 退出及 250 ms poll 未改。
- SDK 仍屏蔽客户库的高频 screen-block 文本；仅把 `puts` 调用重定位到静音入口。v18 函数从 0x188 缩短为 0x174，已根据反汇编更新函数 hash、poll 调用校验和静音调用偏移 0xD4，未重写函数指令。
- 导航 PCM 发送策略、音讯优先级、PLC 相关对象保持前版已验证内容。UI 对齐、HID 快速响应、resource 450 兼容、FDK API 路由等独立适配继续保留；未发现本包已替代它们的证据。
- `lib_fdkaac.a` 同步为客户版本，但当前固件仍按原构建配置链接本地 O3 FDK。新增库中的 FDK 对象差异仅为构建日期字符串。
- 延迟诊断默认关闭；本次未重新开启高频统计。

## 客户包内其他变化（升级前须知）

**持久化设置格式由 20 bytes 改为 36 bytes。客户 LoadParam 拒绝旧长度后执行 RestoreFactory / SaveParam，没有发现旧格式迁移路径。因此升级旧固件时，已保存的设置会回到工厂／配置默认值。** 这来自客户库自身，未做额外二进制补丁。

其他实际代码变化：

- 增加音频 44100/48000Hz 能力筛选配置；默认值 0 保留原能力处理。
- 增加断线重启和启动延时配置；默认分别关闭和 0。
- 软件 I2C 增加 SDA/SCL 时序延迟；电气效果需要实机验证。
- 无线 CarPlay 连接超时分支阈值从 60000 降到 30000（按现有毫秒 tick，即 60→30 秒）。
- `acc_carplay_cb_send_key` 增加 `GetAirPlayInfo()+0x6c8` 字节判断，非零时不调用原回调；字段语义本次静态审查未确定，应实测按键交互。
- SystemSetup 内部结构扩展为 1344 bytes，库内分配／复制长度同步更新；未在 SDK 源码发现直接覆盖该私有结构的访问。

## 验证证据

- `verify_customer_native_nav.py`：6 个关键客户对象 hash 校验通过。
- `test_customer_media_clock.py`：直接链接并模拟执行客户 Thumb 对象。覆盖 Configure/NormalFrame 当前帧采时、RTCP 接收／发送独立采时、NTP epoch／小数位、整数秒起点、重连、HAL 和 RTOS 回绕。
- `test_screen_tx_poll_patch.py`：6 项通过；包括原生指令不变、仅预期重定位变化、错误 hash／poll 调用被拒绝。
- `test_screen_session_patch.py`：2 项通过；`test_audio_tx_evidence.py`：2 项通过。
- 独立 review 发现并修复 v18 poll 布局校验及旧头文件依赖两个问题，复核无新增阻塞项。
- 增量 `make -j8 ram_is` 使用指定 Realtek GCC 6.4.1 完成；沿用已有有效 LP 产物。
- 最终 ELF 的 Timer_Start / SetTimeRTCPData、Screen_Configure / NormalFrame 分别直接调用客户 `AirPlayMediaClock_*`；map 确认对象来自 `lib_Accessory2_resource450.a(AirPlayMediaClock.o)`。
- 最终 ELF 保留 4KB write、250 ms poll；无 `__wrap_UpTicksToNTP`、旧 timer wrapper 和本轮 move/audio 诊断入口。

模拟测试不覆盖硬件时间源精度、RTOS 实际调度及车机兼容性，需用新映像复测拖动、音乐、导航与重连。

## 固件

- build：`20261111135555_9AAA`
- 文件：`project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`
- 大小：8,298,496 bytes
- SHA256：`5b7751d5e1c6530ca94c6207d16aaa3695465688618cacaa4f3ad91908c17acc`

## 原始输入校验

RAR SHA256：`7a4ea91c40f347f88d100c614cb3584de65d499c53f7096c849bdbf397dd135e`

| 库 | SHA256 |
| --- | --- |
| `lib_Accessory2.a` | `bee1774f4157b7e3506980d486534e0f02ac53aa1324348fcfca9d940d10ff19` |
| `lib_AndroidAuto.a` | `bf6efa00f789680297c47fbeac2d111560149658384068948cd7ebed892e42a7` |
| `lib_CarPlay.a` | `25c49a4a644f81ea24a81e6d8026fba05566b8dd1492955dc41dc77f5936de0f` |
| `lib_SystemLib.a` | `22bbbcbeacd805252bba73405f7501202c6305e107083075e91f7fd47dce3311` |
| `lib_SystemLibEx.a` | `ca140218e7998ba2d07ce69713e9935b0fc131d9db7169b2adf1b3c3814a4c1a` |
| `lib_UiLib.a` | `3381775b0db73b02bcbe1e03392325d4ce55513b842658095f8dbc1fec56f158` |
| `lib_accessory.a` | `d7269b0a37ecc93f79db993edc6f5a85c307f525c19abffef6277224850571e4` |
| `lib_fdkaac.a` | `9f5d203089f9cb55e39ca1cfdbbb1375909d70b70b2ea0d1ef9cd4f0f6f628d3` |
| `lib_init.a` | `af9a90bf72e28325ff10f8abcb758a67edf79f403aa8c7f611b60569db1a855f` |
| `lib_jpeg.a` | `7fd8aa3de0922625d49af9a7507712143b572d133bcf428627e93bf2dfe70374` |
| `lib_link.a` | `a91f3dc757cd05a8fa6245f43b79eeb8b975f85ca53cf8a6671d3378e2610001` |
| `lib_png.a` | `ee9b1977924fa8db73b977cd2f4ed90ab4104d06af0b58f9c5f859518380d74c` |
| `lib_x264.a` | `1343e5632cb935635426e75fccab50a2f5618eb01dcb87af9b304d8f8cb10209` |
| `lib_zlib.a` | `127bdabe3b92cb64708310e6ac7150a26b23145409e7478538ddb405438ef565` |
