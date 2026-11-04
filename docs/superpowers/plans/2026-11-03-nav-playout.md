# Navigation continuity implementation plan

Goal: reduce avoidable navigation PCM holes while preserving bounded latency and stream ownership.
Architecture: preserve a pending wake under the existing condition mutex; apply a fixed 30 ms startup/rebuffer margin to 480-sample 44.1/48 kHz navigation contexts. Keep the current reader timestamp semantics and purge-head fix. Independent AUDIO_NAV_PLAYOUT switch, default off; test image on.

- [x] Reproduce early signal loss with a host model of the actual POSIX condition semantics; cover wait/signals, coalescing, shutdown, slot reuse and non-navigation passthrough.
- [x] Implement bounded four-session state, lifecycle wrappers and member-local symbol redirection. Preserve vendor archives and pinned native layout.
- [x] Enable 48 kHz diagnostic registration; report wake bypass counts and effective latency, plus existing gap/skip/zero/purge counters.
- [x] Run state/ABI/archive tests, build sequential LP if needed and parallel IS only, inspect linked calls, archive image and SHA256.

Ruling: do not call raw FDK conceal through an opaque AudioConverter handle. The library owns that handle, its buffered decoder state, and frame accounting. PLC is deferred until that ownership/ABI is established; this image preserves original handling of truly missing audio. Extra latency is capped at 400 ms total and is not repeatedly added per packet. Navigation has an existing 50-node minimum pool; no new audio buffers or polling task.
Ruling: work in the existing dirty workspace because the coherent mono bundle and instrumentation are the tested baseline; preserve unrelated changes. No commit/push requested.

Verification: state, ABI (policy on/off), archive integrity, purge regression and linked ELF checks passed. Independent review found no blocking issue; corrected mode label. Device listening test pending.

Built 20261103173602_ABFE; flash SHA256 95f7ac2c2f00d5a267c68976e3119dc9907de8ba608880cc7f73b16a6bd50dda.
