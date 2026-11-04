# Navigation continuity test firmware

User approved implementation after audio-lag13 review. Scope: mono AAC-ELD,
480 samples, 44100/48000 Hz. Keep music, reorder waiting, and runtime emergency
purge unchanged. No blanket teardown delay. Keep existing dirty integration.

## Contract

- First Read recovery must preserve queued head unless pool is nearly full.
- On a confirmed forward timestamp gap of 1–3 frames, decoder runs FDK
  AACDEC_CONCEAL before the next real decode. Cache timestamped substitute PCM
  for native Read's gap fill; support partial reads and timestamp wrap.
- Unsupported format, allocation failure, large/unaligned gaps, unavailable
  substitute PCM: preserve native behavior and count fallback.
- Capture PCM sample count, duration, nonzero count, mean absolute amplitude,
  peak, encoded backlog before Free; never infer speech from nonzero alone.
- Deferred reports, no per-packet UART, no per-packet allocation. Cache lifetime
  ends only after native decoder join. Context semaphore protects PCM cache.
- Explicit test switch AUDIO_NAV_CONTINUITY=1 requires DIAG/PLAYOUT/PURGE_FIX=1.
  This uses already hash-pinned diagnostic call sites, not a new binary rewrite.

## Execution

1. Production-C host tests: startup vs emergency, FDK order, 1–3 loss,
   split reads, large/backward/wrapped timestamps, cache-full/late, failure,
   stop energy/lifecycle/unsupported formats.
2. Implement bounded state and call-site shims. Correct zero-fill diagnostics.
3. Unicorn ABI tests and regression suite; one independent final code review.
4. Build IS with external Realtek toolchain, inspect linked call targets and
   memory boundary, archive exact ELF/map/image and publish SHA256.

## Decisions

- Use an explicit diagnostic test switch first; the current requested artifact
  is a customer test build. Production/off-mode behavior remains unchanged.
- No global purge disable, no increase in buffering, no arbitrary silence
  insertion as primary fix, no changes to sender teardown policy.
- No commit/push in this task; user requested fixes, evidence and test firmware.

## Progress

- Pre-flight: native Init/Read/converter layouts and FDK CONCEAL API inspected.

- Host C RED→GREEN: startup preservation, bounded concealment, split reads,
  timestamp wrap, 2-frame loss/valid error output, cache pressure/expiry,
  decoder failure, allocator failure, format guard, stop energy and reuse.
- Thumb ABI: 25 existing probes plus 3 continuity shims passed; native Read
  purge regression, archive isolation/guards, production linked calls passed.
- Real local FDK host build: 44.1/48 kHz mono ELD 480, lost 1/2/3 frames,
  concealment output valid/nonzero and next frame decodes successfully.
- Final independent reviewer: no Critical; one Important diagnostic snapshot
  issue. Reproduced false early snapshot with a failing test, deferred gap event
  until after fill, regression now passes. Other planned cases expanded.
- Stop snapshot refined to native mutex destruction AFTER decoder join and
  BEFORE native queue clearing (member-local symbol redirect).
- Hardware/acoustic tests remain pending; no claim of real-device resolution.

- Shared session-table scans use a short critical section so concurrent teardown
  of another context cannot free a slot during lookup. Matching context lifetime
  remains governed by the native decoder/reader stop contract.
