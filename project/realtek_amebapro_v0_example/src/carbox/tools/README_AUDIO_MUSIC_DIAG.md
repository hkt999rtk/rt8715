# Music stop diagnostics (2026-11-03)

## Purpose and test scope

Investigate audio-lag11: incoming AAC-LC music repeatedly stops after roughly
8.3–8.7 seconds, navigation has RTP sequence gaps, and one black-screen event
was reported without a matching reboot in the log.

`AUDIO_MUSIC_DIAG=1` enables the observation hooks and output epoch lifetime.
It defaults to 1 because `AUDIO_NAV_PACING` uses that lifetime.
`AUDIO_DIAG_LOG=0` suppresses diagnostic UART output by default; use 1 to report.
Observation alone does not change audio buffers,
timeouts, sequence processing, decode selection, or control decisions. The
navigation purge correction remains separately controlled by
`AUDIO_NAV_PURGE_FIX`; the test build retains its previous setting to avoid
changing behavior and instrumentation simultaneously.

Incoming GeneralAudio music uses stream **102**; outgoing car-side main audio
uses **100**. Do not equate these identifiers. Navigation output uses 101.

## Evidence

- `MUSIC_LIFE` / `MUSIC_ARGS`: retained control events, paired by event id.
  The producer records task, original caller, timestamp and raw native args.
  Events distinguish incoming SETUP, FLUSH, TEARDOWN, connection-close teardown,
  setup-failure cleanup, parsed stream types, and outgoing audio start/stop,
  PCM exit and client TearDown. Phase 0 is entry; phase 1 is native return.
  Return args are result registers, not the original input arguments.
- `MUSIC_PROGRESS` and its matching id lines: cumulative incoming GeneralAudio
  RX-thread lifetime counters. Receive, RTP sequence/SSRC, decrypt, decode,
  actual copied PCM, explicit zero filling and native queue/state snapshots.
  Look at gaps, SSRC transitions and FLUSH events together before interpreting
  a large sequence delta as lost audio. This counter observes primary RTP socket
  delivery; packets recovered through RTCP retransmission bypass that socket
  probe. A FLUSH does not itself create a new
  diagnostic RX-thread lifetime.
- `MUSIC_OUT` / `MUSIC_OUT_LOSS`: separate lifetime counters for ring push/pop and UDP
  send. PCM byte counts differ from framed/encrypted UDP datagram byte counts.
- Lost-event/slot counters and shared UART `log_skip` are evidence limitations.
  Missing log lines never establish that an event did not occur.

All producer storage is bounded and no producer prints or allocates diagnostic
memory. Reports reuse the priority-1 `wlan_rx_diag` task; periodic counters are
limited to approximately five seconds, with final/control events eligible on
its next wake. Timestamp fields describe the producer event, not UART arrival.
Logging has finite overhead and may skip lines when UART serialization is busy.

## Analysis order

1. Find outgoing type-100 TearDown and its original caller in the exact build ELF.
2. Match earlier incoming teardown/connection-close/setup-failure events by
   session/task/time, including nested parsed stream-type events.
3. If no corresponding control event is retained, inspect local output-stop and
   PCM-exit callers; do not assume an incoming request from absence alone.
4. Compare final receive/decrypt/decode/PCM counters and last-progress ages with
   the output epoch's push/pop/send results.
5. Inspect SETUP/FLUSH and native sequence/timestamp snapshots to distinguish
   discontinuity/reset problems from forward losses within one stream.

These probes do not establish sender intent, USB delivery, or actual car-speaker
playback. A successful send means local socket acceptance. They also do not
measure total CPU utilization or locate losses before UDP input.

## Build

Use the repository build procedure and external toolchain. Test flags:

```
AUDIO_DIAG_LOG=1 AUDIO_NAV_DIAG=1 AUDIO_NAV_PURGE_FIX=1 AUDIO_MUSIC_DIAG=1
```

Vendor archives are never overwritten. Derived CarPlay archives apply the
existing navigation probes, then lifecycle and GeneralAudio probes. The derived
Accessory2 archive adds output evidence on top of the selected production base.
Archive scripts reject unknown function hashes or unexpected call relocations.
Keep `flash_is.music_test.json` and the exact ELF with the tester's log.

## Verification for the first test image

Build `20261103150314_90F9` completed with the flags above. Final ELF checks
confirmed 21 lifecycle/progress callsites, five output callsites, four outgoing
wrappers and all three reporter calls. The lifecycle event ring resides in
zero-initialized external RAM; internal SRAM BSS ends at `0x20179730`, below
the unchanged `0x2017a000` safety limit.

Host tests cover native register/stack arguments and return values, patch
identity/rejection guards, bounded lifetime retention and overflow, PCM/drop
accounting, and logger contention. The output formatter's maximum-width test
record is 596 bytes; emitted UART lines are at most 157 bytes. Test scripts:
`test_music_lifecycle*.py`, `test_music_progress_*.py`,
`test_audio_music_output*.py`, and `test_patch_airplay_music_output.py`.

The exact image, ELF, map and hash manifest are retained under
`GCC-RELEASE/application_is/music_diag_archive/20261103150314_90F9/`.
On-device playback and diagnostic overhead still require customer testing.
