# v19 非音频对象稽核

## 范围和方法

只读比较 `/tmp/rt8715-v19-review/vendor` 与更新前备份 `/tmp/rt8715-v19-review/baseline/vendor` 的原始客户 archive；没有修改仓库或客户 archive。使用 Realtek GCC 6.4.1 的 ar/objdump，以及 `patch_screen_session.py:ELF` 逐 section 比较。完整临时证据位于 `/tmp/rt8715-v19-review` 中的 `non_audio_sections.txt`、`v18-*.o.{dr,r,t}`、`v19-*.o.{dr,r,t}`。这是静态等价证据；下述模拟测试执行实际 Thumb 指令、外部 HAL/RTOS/libc 被模拟，未上板。

## 结论

- `AirPlayScreen.o`：所有指令、数据和 relocation 原始 bytes 与 v18 完全相同。符号字符串改变使两处有效 relocation 目标从 `AirPlayMediaClock_VideoTimestamp` 变为 `AirPlayClock_VideoTimestamp`：Configure +0x14、NormalFrame +0x3e。
- `AirPlayTimer.o`：同样只有符号表/字符串表改变。GetTime 调用在 SetTimeRTCPData +0x32/+0x94；StartTime 调用在 Start +0x04，统一改为 `AirPlayClock_*`。
- 新增 `AirPlayClock.o` 是旧 `AirPlayMediaClock.o` 的等价改名；函数/section/目标名做 AirPlayMediaClock→AirPlayClock 正规化后，所有指令、数据、relocation 语义完全一致。仍从 `hal_read_curtime_us` 取 32-bit 硬件微秒，利用 RTOS 64-bit 时间选择 wrap epoch，保存单调时间和整数秒 baseline；没有退回 RTOS 毫秒时基。
- 旧 `AirPlayMediaClock.o` 仍留在 v19 archive，且与 v18 字节完全一致。本项目用 `--whole-archive` 链接 Accessory2，再由 `--gc-sections` 清除未引用的旧 clock sections；任何旧 SDK 观测 forwarding 若仍引用旧符号，就会保留旧实现、建立第二份 baseline/epoch。必须把 forwarding 改成新符号，并检查最终 ELF 不含旧公开 clock 函数及重复状态。
- `Log.o` 唯一差异是 `.rodata.CLog_GetInstance.str1.1` 内版本 `20261110214403_CF6A`→`20261113102845_B2C8`，代码/reloc 均不变。
- 本范围未发现客户漏带原生时基修复；导航算法所在 lib_CarPlay.a 在父任务 archive inventory 中 unchanged，本子任务未重复审查音频/导航实现。

## ABI / 状态

| 新导出 | 兼容声明 | code size |
|---|---|---|
| AirPlayClock_GetTime | void (struct timeval *) | 0x2c |
| AirPlayClock_StartTime | void (struct timeval *) | 0x20 |
| AirPlayClock_VideoTimestamp | uint64_t (void) | 0x74 |

GetTime 在入参 +0/+4 写 32-bit 秒/微秒；StartTime 同时更新 baseline 秒；VideoTimestamp 返回相对整数秒 baseline 的 NTP 32.32 值（r0/r1）。这些签名由调用约定、内存访问及已知旧接口推断，archive 无 C 原型信息。

本地 BSS 符号：`media_baseline_s` 4 bytes，`screen_hw_epoch_offset_us` 8 bytes，`screen_hw_epoch_valid` 1 byte，`screen_hw_last_us` 8 bytes。函数内部使用 vPortEnterCritical/ExitCritical 保护共享状态。新旧各有独立副本，因此不要混用。

## 必须同步的 SDK 检查/观测

1. `verify_customer_native_nav.py`：Screen/Timer whole-object hash 换成下表 v19 值，clock member 检查由 AirPlayMediaClock.o 改为 AirPlayClock.o 和对应 hash；音频项由父任务负责。
2. `patch_screen_wait_relocation.py`：+0x3e expected target 改 `AirPlayClock_VideoTimestamp`。所有代码 hash、poll/write/quiet offsets 保持原值。
3. `patch_screen_flow_evidence.py`：五处 clock hook 的 old target 全部换 `AirPlayClock_*`。第六处 lwip_sendto +0x120 不变；所有六处 code hash/offset 不变。
4. `screen_flow_evidence.c`：三个 extern 和五个 forwarding 调用换新名字。
5. `check_media_clock_elf.py`、`test_screen_flow_evidence.py`：同步新 clock 名。`test_customer_media_clock.py`：提取新 AirPlayClock.o，保留既有测试。
6. `patch_screen_session.py`：所有受保护函数 hash 原值仍成立，不需修改 offsets/hashes。
7. `patch_rx_crypto_archive.py` / `check_screen_rx_poly_vendor.sh` 使用更旧 Screen whole-object guard `8c1a...`，连当前 v18 原库都不匹配。若启用该可选 profile，需单独检查整个 profile 的输入顺序/ABI 后更新 guard；不能把它当本次新增回归或只放宽 hash。

## Hash

v18 AirPlayScreen.o 13048 e01a7d141a9b68443a175160041465b330e56eab0417d8e6c0846559413c2399
v19 AirPlayScreen.o 13044 1bd7c24a72bfa5e10c889f3f2b0c9fcb2b5940eddbfa452b6baf6efe125ab74c
DIFF .strtab 1623 1618 536b3cff75f43bc5669cd98f796c982f7ca830db9f5001c9e7f5714a4b59f837
DIFF .symtab 2688 2688 359ebe36be446bcec01f26b9740e2d2641bedb96f8d6257dadae1877080d6d64
v18 AirPlayTimer.o 5084 efb9d89938b8a6856b38cdfde418381c74b0dc164939ee6308efce6224976f64
v19 AirPlayTimer.o 5076 fd7b70bda6e39ccec05e49f5b0a5e37149fec8c391b18ff24d7f4f9f6b4e0055
DIFF .strtab 711 701 2229566fbc07e5efb022693cdba76d1f88f2b7f9142dc276479df8fc500fff9a
DIFF .symtab 1200 1200 1060e2ea20631f4baf8abe7d0bc592d93ec068f8371b382d31a632e7df0ee204
v18 AirPlayMediaClock.o 3224 4f6645a25c9717c507e2704d93b039a112b21e617b10c5177a6f0fff56391e38
v19 AirPlayMediaClock.o 3224 4f6645a25c9717c507e2704d93b039a112b21e617b10c5177a6f0fff56391e38
v18 AirPlayClock.o ABSENT
v19 AirPlayClock.o 3188 4d6c19fb946bc622f1dd439b0cbf64a5d53001f1453b494b40e7eddf3ba149f4
v18 Log.o 4676 c003d8f738b4550da0b6d678ae89ce08cbfb3525d6bf989621d88a225dfeda9a
v19 Log.o 4676 d03dd5e6354d466efd36bbfb44179b619a621c37a2364997c26e670715a4caef
DIFF .rodata.CLog_GetInstance.str1.1 103 103 16f2a7852f2aaf2a793fcd4634849c453d23edc3cefbad99cba502fe69b49b74

## 执行验证

`PYTHONPATH=/tmp/media-clock-test-packages python3 /tmp/rt8715-v19-review/test_customer_media_clock_v19.py` → 1 test passed (0.211 s)。临时 harness 只把原生测试的 archive 路径与 clock member 改为 v19，实际执行 v19 Timer/Screen/Clock Thumb code。覆盖 current frame headers、RTCP epoch/fraction、整数起点、重连、HAL 32-bit 微秒 wrap 与 RTOS tick wrap，均 PASS。
