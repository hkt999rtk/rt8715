# Navigation output pacing implementation plan

> Execute inline using executing-plans; user already approved implementation.

**Goal:** Smooth navigation PCM transmission and expose pacing/empty/send timing.
**Architecture:** Pure C controller plus existing output instrumentation, one guarded
archive relocation and Thumb adapter; existing low-priority reporter prints snapshots.
**Tech Stack:** C99, Thumb M33, Python archive verification, Realtek GCC 6.4.1.
**Spec:** ../specs/2026-11-04-nav-output-pacing-design.md

## Global constraints
- Preserve current customer mono/PLC/playout fixes and all unrelated dirty work.
- Only type 101, 48 kHz stereo 16-bit output is enabled initially.
- Target 50 ms; filter 80 ms; max boost 5%; slew 10 percentage points/s.
- No packet/sample deletion or extra prefill; no hot-path prints/allocations.
- Build using rtl8715-build external toolchain, never parallel make all.

## Review focus
- Timer wrap, missed deadlines and fractional accumulation: deterministic controller tests.
- 40 ms producer batches: simulation must stay bounded without on/off catch-up.
- Empty/restart/end: startup, resumed and terminal counters distinguish lifetime stages.
- Music/unsupported formats and lost diagnostic slots: native path remains functional.
- ARM LR/SP and ELF relocations: archive and final linked target checks.

## Tasks
- [x] 1. Add failing host controller tests, then implement audio_nav_pacing.[ch].
- [x] 2. Integrate lifecycle/counters/reporting and hook; test native fallback, stats,
  archive bytes/relocations and Thumb return paths. Add opt-in AUDIO_NAV_PACING flag.
- [x] 3. Review, run affected regressions, build same baseline flags plus pacing,
  verify linked image and SRAM, archive artifact/manifests and report SHA256.

## Execution ledger
- User approved option 1; continue without another design/permission loop.
- Work in the requested existing workspace: substantial uncommitted customer bundle
  and prior diagnostics are required inputs; do not move/revert or commit them.
- No commit/push requested for this task.

- Task 1 complete: host controller tests observed missing-code failure, then pass.
- Task 2 complete: integration and archive tests observed missing-helper/flag failure,
  then pass. Reviewer found encrypted1416 payload coverage; regression failed first,
  guard corrected and test passes. Both Thumb return paths executed in Unicorn.
- Task 3 complete: nine host suites pass, final linked pacing/output/playout verified,
  final incremental ram_is build exit0. Build 20261104103050_AC8A. SRAM bss end
  0x20179a30 below 0x2017a000. Existing Python deps used via /tmp paths.
- Source/report edits only in scoped files; no commit/push. Device listening test pending.


## Regression correction, log15/log16

1. Reproduce ring overflow using production pacer, 40 ms producer, 1 ms wake
   quantization and recurrent 20/38 ms send stalls (RED observed).
2. Separate media phase from wake deadline; bounded debt/spacing, no extra wait
   after stalls; preserve existing lifecycle reset and wrapping time semantics.
3. Add periodic debt/spacing evidence and tests for both native payload sizes,
   timer wrap, 164 ms stall, empty resume, unchanged music fallback and stop.
4. Build with the prior test flags, verify final ELF hooks, archive exact image,
   source checksums and test results; customer playback remains required.
