# Timing / video 共用时间起点

## 已批准的范围

修复车机 timing 与 video 的起点不一致，保留当前帧时间戳修复，增加只读统计并构建客户测试固件。不修改 HID、视频封装、调度及 iPhone 对时。

## 证据

- move-lag9：timing TX 去掉 NTP epoch 后比本地单调时间少约 2 秒；初始化发生在开机 2.x 秒。
- 原厂 AirPlayTimer_Start 将 gettimeofday 的整数秒保存为 baseline；SetTime 两次采时后均减去 baseline，再加 NTP epoch。
- 当前 video 使用共享单调时钟的完整 uptime，没有减 baseline。
- 本地 SDK 接收端 VideoConfig 不读取其 timestamp 来补偿起点。普通 frame 在 flags[1] bit 1 为 1 时直接按同步时钟换算时间；NTP epoch 仅在 timing 协议边界处理。
- 以上接收端反汇编不代表已取得车机端实现；实际显示延迟仍需客户验证。

## 实现

1. 只将 AirPlayTimer_Start 的时间导入改到专用适配器。共享起点模式返回 baseline 秒 0；回覆采时继续读真实单调时钟。关掉开关可恢复旧 baseline 算法。
2. 保留原厂指令、epoch 和 wire 格式。校验对象 hash 及具体 relocation，未知库拒绝修改。
3. 统计线程报告初始化次数、捕获的 uptime、实际 baseline，以及 timing TX 去 epoch 后相对同次发送前 uptime 的偏差。普通 video 保留现有 header age 统计，另记录实际 VideoConfig header/flags。热路径不打印。
4. 测试开关组合、重启计时器、wall clock 跳变、回卷、原始 header/返回值保留及派生库修改范围；构建并检查最终 ELF。

## A/B 基准

旧映像：`/tmp/rt8715-shared-origin-20261111/before/flash_is.bin`

SHA256：`5e5916b2723c8b23d2fefd8116741a03f2c861ccd3d050dfba7043559451ce3e`

## 实现与验证结果

- `airplay_timer_clock.c`：新增 Start 专用入口，`AIRPLAY_TIMER_SHARED_ORIGIN=1` 将 baseline 秒设为 0；设为 0 保留旧算法。该开关仅在 `AIRPLAY_TIMER_MONOTONIC=1` 的派生库中生效。
- `patch_airplay_timer_clock.py`：Timer_Start 的 `+0x6` 调用独立重定位；SetTime 两次调用仍读取真实时间。profile 模式下仅将 Configure 的 `+0x102` 发送调用接到只读 observer。
- `application.is.mk`：加入起点开关及配置 stamp，切换 A/B 时重编译适配器；派生库仍保留全部原厂指令。
- Host C 测试通过：profile 0/1 × origin 0/1，包括 wall clock 跳变、重连、硬件/RTOS 时钟回卷、socket 结果及 errno 保留、观测 header 不变。
- 派生库测试通过：非目标成员不变；目标函数的指令/数据不变；验证准确的 relocation 目标；拒绝重复 patch 和覆盖输入。
- Unicorn 执行原厂 Start 和 SetTime 指令通过：3 个不同初始化时间 × 旧/新起点，验证 RX/TX 跨秒时的完整网络字节序时间戳，误差小于 1us。
- 现有 current-frame timestamp 原厂 Thumb 指令回归测试通过。
- 独立只读 review 未发现阻挡问题；最终 ELF 已确认 Start 专用调用、两次实际采时、保留 NTP epoch、Configure 转发和当前帧时间戳顺序。
- `git diff --check` 通过。未做硬件运行测试，不能据此声称车机显示延迟已消除。

构建：确认 LP AXF 存在后，使用外部 Realtek 工具链执行 `make -j8 ram_is`，参数为：

```text
MOVE_LAG_PROFILE=1 SCREEN_SESSION_FIX=0 SCREEN_SETUP_EVIDENCE=1
AIRPLAY_TIMER_MONOTONIC=1 AIRPLAY_TIMER_CLOCK_PROFILE=1
AIRPLAY_TIMER_SHARED_ORIGIN=1 SCREEN_CURRENT_TIMESTAMP=1
```

固件：`project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`，8,298,496 bytes。

SHA256：`70ff4932f7bf5ac52a257d5e20c8211ff09a63b5504e463cd00172c90ecc9405`

构建、测试及 ELF 验证记录：`/tmp/rt8715-shared-origin-20261111/`。

## 客户测试判读

- `MEDIA_ORIGIN`：应为 `shared_origin=1 baseline_s=0`；`captured` 是真实初始化 uptime，`starts` 记录初始化次数。
- `MEDIA_BASIS`：`tx_minus_uptime_us` 是实际 timing TX（去 NTP epoch）相对同次发送前 uptime 的差，正常略负，反映填包到提交前执行时间。不会因初始化在 2.x 秒就固定为约 -2,000,000。它不包含后续 socket、网络及车机显示延迟。
- `MEDIA_CONFIG`：实际发送前 config header 时间戳和 flags；`use_ts=1` 表示要求接收端使用 frame timestamp。count 变化可对应重新配置。
- `MEDIA_VIDEO`：继续观察实际当前帧 header 年龄、倒退/重复及空闲间隔。不要将它与不同时刻的 timing 样本直接相减。
- 请继续录制连续拖动及停止后再拖动的画面，并同时保存完整日志；若起点已一致但仍滞后，继续从排队和车机呈现端定位，不添加固定时间偏移。
