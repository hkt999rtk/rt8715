# Current-frame timestamp implementation plan

User requested the reviewed timestamp fix, profile evidence and a new firmware.
Implement in the existing workspace, preserving prior changes; no commit/push.

Design: retain the current local monotonic media clock and native timestamp
format. In AirPlayScreen_SendScreenNormalFrame, sample the current frame time
before writing params[0], and retain the native screenNTPTime update. Do not
attempt to propagate iPhone PTS across the timestamp-free decoder ABI in this
patch. A guarded final archive transformation makes the fix independent of
diagnostic hooks. Unknown function bytes or relocation layouts fail the build.

- [x] Execute original and patched Thumb timestamp block under Unicorn;
  verify current header/global values, register/stack preservation, idle and
  reconfiguration cases; verify unrelated archive sections/relocations unchanged.
- [x] Add SCREEN_CURRENT_TIMESTAMP=1 final archive selection with opt-out.
- [x] Extend existing MEDIA_VIDEO/clock profile with pre-encrypt header age,
  timestamp intervals/backwards/repeats, and timing sendto duration. Print only
  from existing reporter; preserve send result, payload and errno.
- [x] Run production C host tests with profile off/on, related screen tests,
  build IS using the external toolchain and inspect final linked instructions.
- [x] Record firmware path/hash and test limits. Hardware rendering and actual
  vehicle receipt are not established by these host tests or socket counters.

## Verification / handoff

- Native Thumb execution reproduced the old previous-frame header after a
  5-second idle, then passed with current-frame header/global values on vendor,
  resource450 and evidence+monotonic archive variants. Other members and all
  unrelated sections/relocations are byte-identical; double patch and input
  overwrite are rejected.
- Production C profile tests passed with diagnostics off/on: wall time changes,
  timer wrap, timestamp duplicate/backward/old detection, and delayed sendto
  including failure returns. 2 clock tests and 10 screen tests passed.
- Initial SDK compile rejected libc errno access (SDK uses global lwip_err).
  Removed unnecessary save/restore: post-send instrumentation only reads the
  HW/RTOS clock and updates counters, never writes the shared socket error.
- Incremental IS build succeeded (existing LP verified), with MOVE_LAG_PROFILE=1,
  SCREEN_SESSION_FIX=0, SCREEN_SETUP_EVIDENCE=1, AIRPLAY_TIMER_MONOTONIC=1,
  AIRPLAY_TIMER_CLOCK_PROFILE=1, SCREEN_CURRENT_TIMESTAMP=1.
- Final linked instructions: 0x701d1068 UpTicks; 0x701d106c wrapped conversion;
  0x701d1070 store current global; 0x701d1074 store current header. Both timing
  functions retain their monotonic adapter. Final archive suffix is
  `_screen_evidence_mono_p1_current_ts.a`.
- Firmware: project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin
  (8,298,496 bytes).
- SHA256: 5e5916b2723c8b23d2fefd8116741a03f2c861ccd3d050dfba7043559451ce3e
- Build and test evidence: /tmp/rt8715-current-ts-20261110/.

## 客户日志判读

- `MEDIA_VIDEO current_ts=1`：本版启用了本帧时间戳修复。
- `age_last/max_us`：加密前采样时间减 header 时间；超过 20ms 计入
  `old20ms`。20ms 只是观测门槛，不是丢帧或超时条件。
- `back/repeat`：相邻普通帧 header 时间倒退／相同次数。
- `gap_max_us`：相邻普通帧 header 最大正向间隔；静止画面也可能出现长
  间隔，不能单凭该值判定异常。数值为累计统计，超过 UINT32_MAX 饱和。
- `MEDIA_CLOCK send_avg/max_us`：timing 回复调用 sendto 的平均／最大耗时，
  包括失败调用；不是 USB 实际完成时间或车机显示时间。
- 所有统计由既有统计线程输出，没有新增逐帧 UART 打印。iPhone 原始 PTS
  仍未跨 DecoderRender ABI 传递；本次修复的是本地重打时间戳使用旧值。
  时间戳值不代表车机已渲染，仍须客户实际验证卡顿改善。
