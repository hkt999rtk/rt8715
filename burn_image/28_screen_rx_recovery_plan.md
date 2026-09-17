# Proposed audio/video RX separate-buffer recovery

Status: implemented for board testing; see `29_audio_video_rx_recovery.md` for
the delivered build, implementation refinements and remaining board validation.
Based on `27_screen_rx_separate_buffers_trace.md` and the currently linked ELF.

## Objective and scope

Keep each hardware-eligible screen RX record's ciphertext until decryption
and authentication finish. Decrypt directly into a separately owned plaintext
buffer and deliver that pointer without a full-record copy back. If a hardware
operation fails, recover the same record in software before returning to the
receiver. This mitigates stream disruption; it does not fix the cause of IRQ
timeouts or remove the configured 20 ms wait.

Scope expanded on 2026-09-17 to both screen and audio RX local mode-2 paths.
Completion requires both audio receive paths described below, not just video.
Other callers retain their current contracts. This is important because the
generic streaming API cannot assume that every caller preserves source storage
until verify. Keep its existing fallback paths for unrecognized layouts.

## Recommended integration: receiver-local symbol adapters

The following adapters describe the screen integration; audio has its own
caller/buffer adapters and shares the recovery core, as detailed below.

Use the existing derived-archive build, preserving the customer's input `.a`.
Redirect only the audited receiver object's relevant unresolved references;
do not modify ARM instruction bytes or globally redirect all ChaCha callers.

Proposed new module: `src/carbox/screen_rx_crypto_buffers.c/.h`.
It keeps bounded per-record metadata keyed by producer task, allocation A and
crypto state identity. No single global active-frame pointer. State lifetime:
allocated -> crypto pending -> verified -> delivered -> producer released.
Receive/verify errors may go directly to cleanup.

Adapters cover:

1. Payload allocation / cleanup: retain existing unrelated frees such as the
   calloc-created receiver context through a recognized-pointer passthrough.
2. Decrypt: bind source A and destination B and enter an explicitly registered
   direct RX transaction. Preserve normal byte-count return semantics.
3. Verify: translate the receiver's `A + written` output argument to
   `B + written`, while tag still refers to A. Call through the existing
   key/nonce-alias wrapper chain, not around it.
4. ScreenStreamProcessData: substitute verified B for A, forwarding the full
   original ABI, length, timestamp and callback arguments unchanged.
5. Cleanup(A): release ciphertext A and the producer reference on B, regardless
   of whether delivery succeeded. Never free B while a consumer still owns it.

The vendor caller may continue holding A internally: it sees normal lengths
and verify status, while delivery and cleanup adapters resolve the mapping.
Before enabling, audit the exact ScreenStreamProcessData prototype/calling
convention and all receiver relocation sites. Unknown objects/layouts fail
the build audit; never silently accept a new vendor version.

The current ownership audit checks an empty ScreenUtils.o, while the link map
shows ScreenUtilsStub.o implements ScreenStreamProcessData. The new audit must
cover the actual implementation as well as receiver, AppStub and AirPlayScreen.

If customer source is available, the simpler long-term alternative is to put
the A/B allocation, explicit output argument, delivery and cleanup directly
in ProcessFrames. The same crypto and ownership work below remains necessary.
The proposed first version does not depend on obtaining that source.

## Buffers and steady-state performance

- A holds received ciphertext and wire tag, with the existing optional aligned
  prefix/tail used by hardware Poly1305. Ciphertext bytes remain unchanged.
- B is writable, aligned, padded through hardware access and cache-line bounds,
  and registered as the plaintext source for zero-copy handover.
- Keep original tag and AAD valid across all hardware attempts and software
  retry. Metadata writes in the Poly1305 prefix/tail must restore the wire tag
  on every exit, including timeouts.
- Initially use a separate allocation per eligible live record and bounded
  metadata, consistent with existing allocation behavior. B can remain queued
  after A is released; a single reusable scratch B is therefore unsuitable.
- If B allocation/registration fails before submission, select an explicit
  software-only record path using A and the existing delivery contract. Do not
  silently run hardware in place and lose the promised retry input.

Local ChaCha needs a registered direct RX route. Its current decrypt function
stages ciphertext into dst, and verify passes output_base as both hardware
input/output. Skip staging only when the adapter proves source lifetime and
layout, then submit A -> B. Do not enlarge the closed-library-visible state
ABI without auditing the key-alias translation and allocation sizes; keep
additional transaction ownership in the local side table.

Extend the managed rounded single-pass raw-ChaCha route to separate buffers.
Keep logical payload length, AAD and counter unchanged. Audit readable rounded
source extent, writable rounded destination extent, tag preservation and cache
maintenance separately. Preserve the existing three-operation standalone path
(key derivation, Poly1305, payload XOR) for previously eligible records; avoid
accidentally falling back to separate prefix/tail XOR submissions.

No extra full-payload staging/copy-back is intended on the eligible normal path.
Allocation, metadata and cache costs still require measurement. Existing small
frame / handover-copy fallbacks retain their documented behavior.

## Recovery contract

Current `chacha_rtl_recover_locked()` returns 1 irrespective of reinitialization
success. Replace this ambiguous internal contract with operation-local recovery
results distinguishing at least DMA quiescence from future hardware readiness.
Audit the linked HAL/ROM deinit/reset implementation: return code alone is not
an independent observation that no DMA can write again.

For hardware-operation failure:

1. Preserve the existing pre-reset timeout snapshot.
2. While owning the shared crypto lock, disable/stop the failed operation using
   the audited HAL/reset sequence; settle pending IRQ/completion state so a late
   completion cannot satisfy a later generation's wait. Establish DMA quiescence
   using the documented hardware conditions with a bounded wait.
3. Reinitialize/rebind for future hardware use and report that result separately.
   DMA quiescent with re-init failure still permits software recovery of this
   record; do not advertise the engine as ready in that case.
4. Once DMA can no longer access B, run the explicit software implementation
   from intact A into B using the same record key, nonce, AAD and original tag.
   Do not call the automatic public route recursively. Release the device lock
   before CPU-only work when its state contract allows, so unrelated AES users
   are not blocked by software decryption.
5. Return success and make B deliverable only after software tag verification
   succeeds. The vendor receiver then performs its normal nonce increment once.
6. On software verification failure, return the real error and clean up through
   normal ownership rules. If DMA quiescence cannot be established, do not reuse
   or free its buffers; retain the existing fatal containment behavior.

Retry only once in software for a submitted operation failure. No hardware retry
loop and no bypass of authentication. A hardware-produced tag mismatch remains
an authentication failure in this first version, rather than being hidden as a
successful reset. Preserve keys/nonce in the record context until retry finishes;
hardware staging buffers are cleared by the backend and cannot be the only copy.

The existing post-timeout nonce +/-1 probe/resync must not advance a successfully
recovered record again. For this new screen path, a successful same-record retry
uses the original nonce and suppresses corrective resync. Keep any diagnostics
read-only there and preserve other callers' existing behavior. Add a regression
test for the record immediately following recovery.

## Logging

Keep the four pre-reset Crypto timeout lines. On the exceptional path add a
record-correlated result for `dma_quiesced`, `engine_ready`, software retry/tag
result and elapsed recovery time. A software success must be labeled separately
from hardware recovery; it does not prove the engine is working again.

After a recovery, report only the first subsequent screen hardware operation
completion plus its tag result, and the recovered record's handover result.
This answers whether the engine and the screen pipeline resumed. No per-record
success printing. Counters may be reported on the existing optional profiler.
Do not emit key/nonce values, ciphertext or plaintext.

## Files and build integration

| Area | Expected changes |
| --- | --- |
| screen_rx_crypto_buffers.c/.h (new) | Record mapping and receiver adapters |
| audio_rx_crypto_buffers.c/.h (new, proposed) | Audio call-site registration and audio buffer lifetime adapters |
| video_handover_zero_copy.c/.h | Register B and preserve producer/consumer release rules |
| screen_rx_poly_buffer.c/.h | A/B alignment, padding and metadata lifetime helpers |
| ChaCha20Poly1305.c | Registered direct RX and same-record software recovery |
| ChaCha20Poly1305_rtl8195b.c/.h | A -> B single-pass support and explicit recovery outcome |
| crypto_priority_lock.c/.h | Completion generation/reset handling if HAL audit requires it |
| chacha_key_alias_fix.c | Preserve wrapper order; prevent double nonce correction on this route |
| patch_video_handover_archive.sh, audit tools | Receiver-local redirects and actual object audits |
| Audio archive relocation audit/adapter tooling | Scope redirects to the two audio RX functions in AirPlayReceiverSession.o |
| application.is.mk, chacha_m33/Makefile | Feature switch and complete configuration dependencies |

Proposed A/B switch: `SCREEN_RX_SEPARATE_BUFFER=0/1`, default off until tests
pass; the test firmware explicitly enables it. Disabling restores the existing
derived-archive route. Keep CPU clock, USB/TCP priorities and 1 KiB threshold
fixed for comparisons. No production behavior change occurs in this planning step.
Add an independent `AUDIO_RX_SEPARATE_BUFFER=0/1` switch for isolated and combined
tests; the final test build enables both. Do not globally change unrelated
pairing/control/TX call sites while adding the common RX recovery mechanism.

## Audio scope added after caller trace (2026-09-17)

Disassembly of the current `lib_CarPlay.a(AirPlayReceiverSession.o)` shows two
ChaCha RX call sites. These are separate integration and validation items:

| Caller | Observed behavior | Planned handling |
| --- | --- | --- |
| `_GeneralAudioDecryptPacket` | `+0x40` calls decrypt with R1=packet input, R3=separately supplied output. ReadAudio passes its context buffer at offset 0x1bf0 as output and consumes it through audio decode or PCM conversion after success. | Audit non-overlap, capacity and lifetime; reuse the existing output buffer when eligible rather than allocating another unnecessary B. Enable direct source retention and same-packet retry. |
| `_MainAltAudioThread` | `+0x27a` calls decrypt with R1=R3=packet payload. `+0x294` verifies in the same buffer. | Add separate output storage and audit packet-node/jitter-buffer consumers, pointer substitution, reuse and final release. Do not apply screen queue/refcount assumptions to audio nodes. |

Both paths initialize ChaCha from the packet's final 8-byte nonce, authenticate
using the tag preceding that nonce, and decrypt the preceding payload (wire
length minus 24). GeneralAudio's observed ReadAudio calls pass 8-byte AAD;
MainAlt selects 8 or 12 bytes. Retry must preserve these exact bytes and must
not substitute the screen path's 128-byte AAD or implicit nonce progression.
Although the audio code also updates context state on successful verification,
the packet-derived nonce is the actual input to the observed decrypt calls.

Audio hardware eligibility is determined by encrypted payload length and other
backend checks, not by codec name or decoded PCM size. With the current 1024-byte
threshold, smaller payloads take software; payloads at or above it may use
hardware. Small AAC packets do not justify excluding audio from recovery.

Use a shared registered RX transaction/retry core for audio and video, with
caller-specific lifetime adapters. Keep small software-only packets efficient;
do not force extra allocations or hardware use merely to enable recovery.
The generic streaming API must still not retain arbitrary unregistered sources.
Audit the customer's newer AirPlayReceiverSession.o separately before enabling
redirects there; receiver object layouts are version-specific.

Audio-specific acceptance tests: both RX branches, payloads around 1024 bytes,
original packet nonce/AAD/tag, out-of-order/duplicate packets, packet-pool reuse,
decoder/PCM delivery, injected timeout with partial output, reconnect and
concurrent audio/video use of the shared engine. Measure underruns/playback
continuity and recovery latency as well as tag success; saving a packet after a
20 ms timeout does not guarantee it meets the audio playback deadline.
Log RX kind/record correlation on failures and first post-recovery success so
audio and video recovery evidence cannot be confused.

## Verification and delivery sequence

1. Verify wrapper ABI, scoped relocations, source archive immutability and
   deinit/reset/late-IRQ behavior. Reject incompatible archives in negative tests.
2. Host tests for adapter byte counts, verify tail/tag mapping, interleaved
   tasks/records, unknown pointers and exactly-once cleanup. Queue consumer may
   finish before producer; cover queue failure, conversion/copy fallback,
   disconnect, allocation failure and metadata exhaustion.
3. Crypto tests across alignment residues, threshold and 64 KiB boundaries;
   verify direct hardware A/B pointer identity and operation count. Inject
   failure at key derivation, Poly1305 and payload XOR, including partially
   overwritten B. Compare recovered plaintext to software vectors, confirm A
   unchanged, corrupted tags rejected, and the next record's nonce correct.
4. Model reset failure and late completion; use ASan/UBSan for buffer lifetime.
   Run existing ChaCha matrix and screen Poly1305 tests. These do not substitute
   for physical DMA/cache validation.
5. Build firmware using the repository's LP/IS sequencing and external toolchain.
   Inspect final linked call targets and archive/hash guards. Retain matching
   flash_is.bin, ELF/map, build log, configuration and SHA-256.
6. On board, first inject one deterministic timeout with the DMA stop path still
   enforced, verify software recovery and next-frame delivery. Then reproduce
   a real timeout and run reconnect/queue-pressure soak. An injected error after
   hardware completion alone cannot validate stopping a still-active DMA.
7. Compare the same screen workload with feature off/on: FPS, CPU, submit count,
   normal latency and peak memory. Recovery acceptance requires verified delivery
   and continuing subsequent frames, not merely an "engine recovered" message.
