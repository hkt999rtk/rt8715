# Navigation AAC-ELD stall evidence

## Design and build

`AUDIO_NAV_DIAG=1` observes 44100 or 48000 Hz / 480-sample jitter contexts.
The hooks are enabled by default because continuity uses their audited call sites.
`AUDIO_DIAG_LOG=0` is the release default; set it to 1 to print these records.
Observation hooks alone do not change buffer capacity, latency or condition waits.
`AUDIO_NAV_PURGE_FIX=1` separately refreshes the reader head after purge; if
purge leaves no node, the original full-output zero/wait path is retained.
`AUDIO_NAV_PLAYOUT=1` independently retains early condition signals and adds
30 ms to navigation latency, capped at 400 ms (already higher values unchanged).
`AUDIO_NAV_CONTINUITY=1` enables the tested mono continuity policy and requires
all three switches above. It preserves the first startup head unless the pool
is nearly full, and uses bounded FDK concealment for confirmed 1–3-frame gaps.
All four policy/hook switches default to 1; UART statistics default to off. Do not combine with AUDIO_STREAM_DIAG or
AUDIO_JITTER_ADAPTIVE: their wrappers overlap.

The purge fix pins AirPlayUtils.o SHA256 and redirects one validated call.
The jitter diagnostic patch accepts the original or exactly patched object and
adds 25 probes (purge probe 21 is nested inside the fix trampoline).
The RX patch pins _MainAltAudioThread instruction bytes and validates seven
original call relocations before adding probes.
Native instructions/data and original archives are unchanged. Generated Thumb
trampolines preserve arguments, return values and callee-saved registers,
including the converter's two stack arguments.

Implementation plan: audit object/layout; test relocation-only patch; add bounded
state capture; test ABI/queue/lifetime behavior; build and inspect final ELF.
The test image includes the purge-head correction, retained condition wakes and
30 ms latency margin. Native latency also controls the timestamp gap gate and
purge threshold, so this margin affects those native decisions as well. Native reorder waiting is retained. Once the decoder commits to a future packet,
continuity mode calls FDK AACDEC_CONCEAL before decoding it. Up to four substitute
PCM frames are cached by timestamp, including split-read support. The reader
substitutes these only at gap/future-fill sites; startup/underrun silence remains
native. Large/unaligned gaps, invalid concealment, and exhausted cache keep the
native fallback and are counted. The cache is allocated once (~5 KiB/session),
freed after decoder join, and does not use per-packet allocation.

With a valid LP image, build from GCC-RELEASE using repository build rules:

```
PYTHONPATH=/tmp/rtl8715-vfs-deps make -j8 ram_is \
  CROSS_COMPILE=/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi- \
  AUDIO_DIAG_LOG=1 AUDIO_NAV_DIAG=1 AUDIO_NAV_PURGE_FIX=1 AUDIO_NAV_PLAYOUT=1 AUDIO_NAV_CONTINUITY=1 AUDIO_MUSIC_DIAG=1
```

The existing priority-1 wlan_rx_diag thread drains a static 16-record ring every
two seconds. Queue snapshots occur every five seconds and on anomalies/lifecycle.
Queue walks run under the native context semaphore, capped at 256 nodes; they
are outside our IRQ critical section. No new task or producer allocation/UART.
The original Re-buffering printf still exists. Logger uses zero-wait UART lock,
<=160-byte lines and a tick between lines. Instrumentation cost is not zero;
on-device timing still needs testing.

## Reading logs

`NAV_DIAG enabled` confirms reporting is active; observation_only is 0 when
either playback correction is enabled; `NAV_EVT
kind=1` confirms registration of an eligible navigation context. Session IDs
handle context address reuse. Event IDs join continuation lines. Counters are
cumulative per session. Use trigger_us (trigger) / at_us (copied state), not the
host serial timestamp: output is deferred. Times wrap modulo 2^32 microseconds.
An age of 4294967295 means that event has not been observed.

| Kind | Meaning |
| --- | --- |
| 1 | Init; qvalid=0 |
| 2 | Periodic snapshot at reader unlock |
| 3 | PCM underrun before printf/zero-fill; cursor is actual current read TS; missing_samples is remainder of this output request |
| 4 | Native rebuffer wait ended, before next PCM read; not proof of audible recovery |
| 5 | Future PCM head / timestamp gap, captured AFTER fill/substitution (at most once/second); missing_samples is requested fill, NAV_PCM counts actual zeros |
| 6 / 7 | Before / after original purge |
| 8 | Close, after native Free joins decoder; copied counters retained, qvalid=0 |

- NAV_Q: encoded/PCM node counts, start/end-exclusive timestamps, spans and PCM
  sample count. These are sample frames: divide by rate/1000 for milliseconds (44.1 or 48).
  Encoded nominal duration = enc_n * 480 * 1000 / rate ms. Span includes holes;
  pcm_samples is actual queued PCM. holes counts timestamp discontinuities,
  including overlaps, not missing packets.
- NAV_PLAY: busy/capacity, next-read TS, rebuffer state and native late/gap/
  partial-skip/rebuffer counters. walk_truncated must be zero for complete data.
- NAV_RX: application PutBusyNode entry observations, host-order seq/TS at node
  +10/+12. missing counts forward sequence gaps, which reordering may recover.
  This is not a WLAN capture. put_fail/node_fail identify local rejection and
  node exhaustion. GetFreeNode error behavior is unchanged (no added yield).
- NAV_DEC: converter elapsed time including preemption, error/zero-output counts,
  selected decode TS and phase. It is not exclusive CPU time.
- NAV_WAIT: completed wait maxima. cond includes normal idle time. signal_wake
  measures first observed signal during condition wait through its return,
  including scheduling and mutex reacquisition.
- NAV_GATE: discontinuity checks, expected decode TS / encoded head TS, wake/
  signal ages, purge calls and number of removed nodes.
- NAV_PCM: zero bytes for wait / future head / PCM exhausted / timestamp gap;
  divide by output bytes per sample and rate for duration. Recovery duration is
  underrun to native rebuffer-end; repeated underruns can follow immediately.
- NAV_LIVE: copied phase/ages every two seconds even if threads stop progressing.
  active bit N indicates an outstanding call site N. No live-context reads.
- NAV_LOST: event ring overflow, session-slot overflow, skipped UART log counts.
  Missing evidence must not be interpreted as absence of anomalies.

### Phase / active-bit IDs

| ID | Call |
| --- | --- |
| 1 | decoder condition wait |
| 2 / 5 | decoder context semaphore, initial / after decode |
| 3 / 4 | decoder node semaphore / converter |
| 6 / 7 | decoder context unlock, before decode / before next wait |
| 8 / 9 | reader context wait / unlock |
| 10 / 11 / 12 / 13 | memset: rebuffer wait / future PCM / exhausted PCM / gap |
| 14 | buffered-samples query at underrun |
| 15 / 16 | enqueue context wait / unlock |
| 17 / 18 | condition signal from enqueue / reader |
| 19 | decoder timestamp gate query |
| 20 / 21 | purge from enqueue / rebuffer read |
| 22 / 23 / 24 | condition mutex from decoder / enqueue / reader |
| 25 | buffered-samples query at rebuffer end |

Phase N is before decoder call; 100+N means returned. Other hooks do not replace
decoder phase. A long condition wait alone is not evidence of a fault.

## Decision sequence

1. Locate kind=3 with nonzero cursor (exclude initial prefill).
2. Encoded nonempty + PCM empty: check decoder phase, waits, elapsed decode and
   timestamp gate to distinguish backlog from failure to wake or discontinuity.
3. Both empty: examine enqueue gaps and node failures. Separate WLAN/UDP evidence
   is still needed to locate loss before application enqueue.
4. PCM span > PCM samples or future head TS: examine holes/skip counters.
5. Compare purge-before/after and zero-byte changes. Do not equate rebuffer-end
   with actual successful PCM output.

## Verification

From src/carbox:

```
python3 tools/test_nav_diag_patch.py
python3 tools/test_nav_diag_state.py
PYTHONPATH=/tmp/wlan-mp-test-deps python3 tools/test_nav_diag_abi.py
python3 tools/test_recvframe_diag_disabled.py
python3 tools/test_rx_handover_report.py
```

Unicorn executes all 25 production Thumb trampolines. State tests execute the
production collector with synthetic queues: 28 encoded frames but no PCM,
timestamp holes, clock wrap, close/address reuse, ring overflow and deferred
logging. These are host checks, not an on-device audio reproduction.

## Raw RX and UDP evidence (2026-11-03)

`NAV_DIAG purge_head_fix=1` identifies the test configuration. `observation_only`
is 0 when purge correction or playout policy is enabled. The fix works with diagnostics disabled
as well. Archive scripts never overwrite vendor libraries; final link uses the
production fix followed by optional derived jitter/RX diagnostic archives.

- `NAV_APP_RX/SEQ/TIME/CHECK/VERIFY/DROP/DROP2`: cumulative raw SocketRecvFrom
  results before native validation, accepted queue results, and actual native
  PutFree discards. Correlate by session and port. Reports every 5 seconds and
  on RX thread exit; these snapshots need not have the same time as NAV_EVT.
- `NAV_UDP/RX/SEQ/POST`: one snapshot ID joins pre-socket UDP callback sequence
  counters and exact outcomes: accepted / no_conn / invalid_mbox / receive-byte
  limit / netbuf allocation / mailbox-full. No packet body is retained.
- `NAV_APP_LOST/NAV_UDP_LOST` and `NAV_LOST`: evidence loss; never interpret
  incomplete output as absence of failures.

UDP registration occurs at RX thread start, before its first receive. Packets
already queued before thread start are outside that observation interval. Compare
sequence ranges, not bare packet totals; the first observed sequence establishes
baseline and cannot establish loss before registration. Forward gap counts are
not reduced by later reorder/retransmission arrivals; inspect reorder counters.
There is no driver/WLAN hook in these new counters, so loss already present at
UDP callback still requires lower-layer evidence to distinguish driver, air and
sender. `select_max_us` includes normal waiting for new packets.

`free_recv` is a node returned after socket error, not necessarily a received
packet discarded. Auth/length/short/queue counters classify actual native frees.
The original code keeps its error handling, authentication, nonce and ownership
semantics. No new sleep or blocking logging is inserted in producer hooks.

Checks:

```
PYTHONPATH=/tmp/nav-purge-test-deps python3 tools/test_nav_purge_fix.py
PYTHONPATH=/tmp/nav-purge-test-deps python3 tools/test_nav_rx_abi.py
python3 tools/test_nav_rx_patch.py
python3 tools/test_nav_rx_diag.py
python3 tools/test_nav_udp_diag.py
```

The purge test executes original vendor Read instructions under Unicorn with
PCM/encoded head replacement, no removal and empty queues, with diagnostics
on/off. RX ABI tests exercise all six SocketRecvFrom stack arguments. UDP tests
exercise actual recv_udp with diagnostics and SO_RCVBUF independently enabled
and disabled, including pbuf ownership on each return path.

## Retained wake policy (2026-11-03)

`NAV_POLICY` reports requested/effective latency, signals, retained_wakes, waits,
pending state and slot_overflow. A retained wake is a coalesced signal consumed
before the next native condition wait; it is not proof of prevented audible loss.
The latch uses the existing condition mutex and does not acquire the jitter
context semaphore in reversed order. Native Free still wakes and joins the
decoder before state is released. Non-navigation contexts pass through unchanged.
There are four static policy slots, no new audio buffer or worker thread.

Verification includes `test_nav_playout.py`, `test_nav_playout_archive.py`, both
policy modes of `test_nav_diag_abi.py`, and `verify_nav_playout.py ELF`. Tests
cover early signals, coalescing, wait completion, teardown, slot reuse, capacity,
failed init, latency cap, native byte preservation and linked call targets.
Audible improvement still requires device testing.

## Continuity evidence (audio-lag13 follow-up)

- `NAV_CONT startup_keep=1`, `head_ts=0`, and corresponding `purge_nodes=0`
  show the initial frames were preserved. `startup_pressure` means the initial
  pool was near full and native emergency recovery was retained. Later rebuffer
  and enqueue/emergency purge are unchanged.
- `gap_events` / `last_gap_ts/frames`: forward aligned holes confirmed by native
  decoder after its reorder wait. `plc_ok/fail` counts FDK calls, not playback.
- `plc/fallback_bytes`: actual substitute PCM copied / residual zero bytes at
  gap/future-fill sites. One mono 480-sample frame is 960 bytes, 10.884 ms at
  44100 Hz or 10 ms at 48000 Hz. `plc_ok>0` with `plc_bytes=0` needs inspection
  of cache expiry, native purge/late skips, and read timing.
- `cache_full/expired` exposes substitutes not retained or already past their
  timestamp. Cache is bounded; it never replaces still-playable entries.
- `NAV_STOP_PCM`: snapshot after native decoder join and before queue clearing,
  linked via member-local mutex-destroy shim. `samples` sums remaining decoded
  PCM, not RTP timestamp span; `encoded_nodes` is separate, undecoded backlog.
  `duration_us`, `nonzero`, `mean_abs`, `peak` describe queued audio that native
  teardown will discard. Amplitude units are absolute signed-16 PCM values;
  nonzero/energy alone does not prove speech. `truncated=0` and `valid=1` are
  required for a complete snapshot. No teardown delay is added.
- `NAV_CONT_LOST` and existing `NAV_LOST` reveal lost evidence/allocation failure.
  No additional periodic task; the existing low-priority reporter prints copied
  counters and completed-session records. `closed=1` is the final counter set.

Verification includes production-C host tests with ASan/UBSan, Thumb ABI tests,
archive integrity and linked-call checks. `test_nav_fdk_conceal.cpp` additionally
uses a host build of the exact local FDK source: mono ELD 44100/48000 Hz, 480
samples, losses of 1/2/3 frames, valid nonzero substitute output, following normal
decode, output guard bounds. This verifies the API sequence, not target timing
or audible quality; customer device testing remains necessary.
