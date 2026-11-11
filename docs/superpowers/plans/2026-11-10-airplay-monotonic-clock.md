# AirPlay timing monotonic clock implementation plan

**Goal:** Prevent iAP2 wall-clock updates from stepping vehicle-facing AirPlay timing replies.
**Approved design:** Share the existing video hardware/RTOS uptime clock with AirPlayTimer; retain native baseline arithmetic and NTP packet layout. Scope the symbol substitution to AirPlayTimer.o inside a derived lib_Accessory2 archive. Preserve vendor source archives and all unrelated workspace changes.
**Execution:** Native implementation in the current workspace, as requested. No commit/push requested.

## Tasks

- [x] Add regression tests exercising the production clock with wall-clock steps, hardware microsecond rollover, RTOS tick rollover, idle and timer restart. Check exact relocation scope and unchanged archive members, rejection of unsupported objects and atomic output publication.
- [x] Export `carbox_media_monotonic_us()` from the existing video clock; implement a timeval adapter for AirPlayTimer only. Share the same clock state, preserving the video timestamp format.
- [x] Derive the finally selected Accessory2 archive and rename only AirPlayTimer.o's gettimeofday reference. Add optional timing send observation with untouched payload/return semantics. Collect actual outgoing video header timestamps in the existing observer. Print snapshots from the existing statistics thread; no prints from timing/video hot paths.
- [x] Verify tests, build with Realtek toolchain and existing move-lag8 diagnostics (session fix disabled), inspect final linked calls, record flash image SHA-256.

## Review focus

- Wall-clock changes must not change elapsed media time, including after screen restart.
- Retain overflow handling and no new global gettimeofday wrapper.
- Preserve all function bytes/relocations except approved symbol targets; reject unknown Timer object.
- Optional diagnostics must not change sendto payload, retry policy, errno or result.
- Both enabled and disabled diagnostics must compile; no per-frame UART output.

## Evidence / limitations

Vehicle rendering causality still needs customer A/B testing. This change fixes the established clock discontinuity only. Previous-frame timestamp sequencing, HID filter, TCP/RTO, and NAL packaging remain outside this change.

## Verification results

- RED: both new tests failed before the clock adapter and isolated archive patch existed.
- GREEN: `python3 project/realtek_amebapro_v0_example/src/carbox/tools/test_airplay_timer_clock.py` passed (2 tests; production C executed with profile both disabled/enabled).
- Related screen suite: `python3 -m unittest discover -s project/realtek_amebapro_v0_example/src/carbox/tools -p 'test_screen*.py'` passed (9 tests).
- Read-only independent review: no critical/important findings. Host preemption and physical hardware timer behavior remain outside test coverage.
- Incremental IS build succeeded; pre-existing LP AXF verified before build. Build command used external Realtek toolchain, `make -j8 ram_is`, `MOVE_LAG_PROFILE=1 SCREEN_SESSION_FIX=0 SCREEN_SETUP_EVIDENCE=1 AIRPLAY_TIMER_MONOTONIC=1 AIRPLAY_TIMER_CLOCK_PROFILE=1`, and `PYTHONPATH=/tmp/rt8715-build-pydeps`.
- Final ELF verified: three AirPlayTimer gettimeofday calls target `carbox_airplay_timer_gettimeofday`; timing send targets the observation wrapper; video observer calls the header sampler; real gettimeofday is retained by the statistics reporter.
- `git diff --check` passed. Build emitted existing awk escape and FDK submake jobserver warnings, no compile/link errors.
- Build/link evidence: `/tmp/rt8715-media-clock-20261110/`.
- Firmware: `project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`, 8,298,496 bytes.
- SHA-256: `45c50825595b0e4485f047ac0d6a451814a729481e6d37fbc22fcca6568c6481`.

## Customer log interpretation

`MEDIA_CLOCK` reports actual timing reply RX/TX NTP words and the monotonic sample near submission. `wall_now_s` is sampled by the reporter and may jump at IAP2 synchronization; timing/media elapsed time must stay continuous. `MEDIA_VIDEO` records the actual header timestamp before encryption (including the unchanged previous-frame sequencing). Compare elapsed increments across records, not absolute numeric equality between the vendor timing epoch and video epoch. Counts are cumulative; sent counts mean successful socket submission, not vehicle rendering. Printing is done by the existing profiler task only.
