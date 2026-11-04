# Quiet audio release (2026-11-04)

This release retains the behavior tested with audio-lag19 and the customer mono
CarPlay libraries. Native screen poll and duplicate-copy removal are recognized
in the customer archive; the obsolete duplicate-copy patch is removed. Derived
archives still apply the reviewed HID response, navigation and output hooks.

## Defaults

| Setting | Value | Purpose |
| --- | --- | --- |
| `AUDIO_NAV_PURGE_FIX` | 1 | Refresh the reader head after native purge |
| `AUDIO_NAV_PLAYOUT` | 1 | Retain condition signals and the tested latency margin |
| `AUDIO_NAV_CONTINUITY` | 1 | Preserve startup PCM and conceal bounded missing frames |
| `AUDIO_NAV_PACING` | 1 | Gradual queue correction and bounded phase catch-up |
| `AUDIO_OUTPUT_TASK_PRIORITY` | 5 | Audio output precedes priority-4 screen writes |
| `AUDIO_DECODER_TASK_PRIORITY` | 6 | Retain decoder priority |
| `SCREEN_TX_WRITE_CHUNK_BYTES` | 4096 | Bound each normal screen write submission |
| `SCREEN_TX_POLL_WAIT` / `SCREEN_TX_POLL_TIMEOUT_MS` | 1 / 250 | Retain native poll and the write helper |
| `AUDIO_DIAG_LOG` | 0 | Suppress NAV/MUSIC statistics and lifecycle UART records |
| `AUDIO_NAV_TX_TRACE` | 0 | Remove detailed send/core-lock/NCM/FDK trace probes |
| `SCREEN_TX_BATCH_DIAG` | 0 | Remove screen write counters and reports |
| `CARBOX_MODE_DIAG` | 0 | Disable mode-change instrumentation |

The historical `AUDIO_NAV_DIAG` and `AUDIO_MUSIC_DIAG` names also select hooks
needed by continuity and pacing, so they remain 1. **Do not turn them off merely
to silence logs.** `audio_diag_log.h` compiles the UART calls away independently.
The common low-priority reporter continues its bounded housekeeping, including
marking closed output epochs reusable. Counters and required hooks remain;
this is not a claim that every observation instruction has been removed.
Native errors, teardown messages and rebuffer messages are retained.

To restore the investigation reports, build with:

```sh
AUDIO_DIAG_LOG=1 AUDIO_NAV_TX_TRACE=1 SCREEN_TX_BATCH_DIAG=1
```

The diagnostic and policy settings have build stamps. No special command-line
policy flags are needed for the quiet release. Use the repository's sequential
clean/LP/IS procedure and external Realtek toolchain.

## Verification

- Host tests cover quiet and logging modes, native argument/result forwarding,
  navigation pacing, and 40 consecutive output lifetimes without slot exhaustion.
- Existing tests cover partial writes/EAGAIN, the 4 KB cap, audio priority,
  purge/continuity and Thumb trampoline ABI.
- Run `verify_audio_music_output.py`, `verify_nav_output_pacing.py`, and
  `verify_nav_playout.py` against the final ELF.
- Quiet `flash_is.bin` must contain none of the `[NAV_`, `[MUSIC_`,
  `[SCREEN_WRITE]`, `[MODE_DIAG]` investigation tags. Detailed `navtx_*`
  core-lock/send/report implementations must be absent from the linked image.

The quiet build needs device testing; host and ELF checks do not establish
that all audible artifacts are eliminated. FDK concealment behavior is unchanged.
