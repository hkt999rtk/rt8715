# Music lifecycle diagnostic evidence

Enabled only by the `AUDIO_MUSIC_DIAG=1` build profile. These probes change call
relocations, not native control behavior. They do not decide whether a request
should stop music. Producers copy a fixed register snapshot into a 64-entry ring;
the existing deferred diagnostic reporter emits it. A full ring drops evidence
and increments `MUSIC_LIFE_LOST`, never blocks the caller.

## Reading the log

`MUSIC_LIFE` contains event ID, capture time in RTOS milliseconds, event name,
phase (`0` entry, `1` return), task handle, original call-site LR and session
pointer when available. `MUSIC_ARGS` with the same ID contains the register
values. Entry/return records have different IDs; join by task and caller. Return
registers are not incoming arguments. The existing output strings are not
sufficient to establish who initiated a teardown; the event chain is.

| Event | Exact evidence | Arguments |
|---|---|---|
| `incoming_setup` | Receiver server HTTP handler invokes SessionSetup | entry a0=session, a1=request dictionary; return a0=status |
| `setup_stream_type` | Native SessionSetup parses a stream's `type` | return a0=type (low 32 bits), a1=high 32 bits; session from r4 |
| `incoming_flush` | Receiver server handles FLUSH and invokes FlushAudio | entry a0=session, a1=RTP timestamp, a2=sequence; return a0=status |
| `incoming_teardown` | Receiver server TEARDOWN branch invokes SessionTearDown | entry a0=session, a1=request (0 means no dictionary), a2=reason=0 |
| `connection_close` | Receiver server connection-close callback invokes teardown | entry a0=session, a1=0, a2=close reason; **not an incoming TEARDOWN request** |
| `setup_failed_teardown` | SessionSetup failure cleanup invokes teardown | entry a0=session, a1=request, a2=setup error |
| `teardown_stream_type` | Native SessionTearDown parses each requested stream type | return a0=type; a1=high 32 bits; session from r4 |
| `output_start` | Cross-object call to AirPlayAudio_StartAudio | entry a0=local audio type, a1=rate, a2=channels |
| `output_stop` | Cross-object call to AirPlayAudio_StopAudio | entry a0=local audio type; caller identifies originating path |
| `outgoing_teardown` | Cross-object call to AirPlayClient_SendMessageTearDown | entry a0=outgoing stream type (e.g. 100 or 101) |
| `pcm_exit` | Cross-object call to lib_pcm_exit | entry a0=local PCM type |

Do not equate incoming stream type 102 (general audio) with outgoing type 100.
Do not infer that absence of `teardown_stream_type` means no teardown: a whole
session teardown uses a NULL/missing stream array and has no per-stream parsing.
Teardown and output-stop APIs are void; their phase-1 a0 is unspecified and must
not be interpreted as a status code. Phase 1 proves the call returned.

The native receiver tears down platform output before walking the core stream
array. Thus an outgoing teardown can precede `teardown_stream_type` while still
following the corresponding `incoming_teardown` entry.

Outbound entry-only linker wrappers preserve the original caller LR and arbitrary
stack arguments by tailcalling `__real_*`. Only cross-object references subject
to GNU ld `--wrap` are covered; final ELF call targets must be verified. A direct
same-object call bypassing linker wrapping is not evidence of absence.

## Audited sites

`AirPlayReceiverServer.o`:
- `_HandleHTTPConnectionClose+0x38`: SessionTearDown.
- `_HandleHTTPConnectionMessage+0x42a`: FlushAudio (timestamp r1, sequence r2).
- `_HandleHTTPConnectionMessage+0xa1a`: SessionSetup.
- `_HandleHTTPConnectionMessage+0xc74`: SessionTearDown in TEARDOWN dispatch.

`AirPlayReceiverSession.o`:
- `AirPlayReceiverSessionSetup+0xb0`: failure cleanup SessionTearDown.
- `AirPlayReceiverSessionSetup+0x3a0`: native CFDictionaryGetInt64 stream type.
- `AirPlayReceiverSessionTearDown+0xda`: native CFDictionaryGetInt64 stream type.

The patch rejects unknown instruction-section SHA-256 hashes and unexpected
relocations. It preserves every function byte and the number of relocations.
It can be chained after navigation/progress patches because those modify other
call sites/sections without altering these instruction bytes.

## Verification

`test_music_lifecycle.py` checks instruction and relocation preservation and
executes all 11 production Thumb probes in Unicorn. It verifies native arguments,
r0-r3 return values (including int64), callee-saved registers, stack alignment,
and the tail wrappers' original LR and extra stack arguments.

`test_music_lifecycle_ring.py` compiles the production collector with host stubs
and checks the full-ring/drop path, absence of output from producers, capture
time wrapping, and maximum formatted line size below the logger's 160-byte bound.
Hardware performance and causal interpretation still require a test capture.
