# Audio/video RX preserved-ciphertext recovery

Implemented 2026-09-17. Test firmware: **20260917081901_B1C4**.
Host validation and firmware link pass. Physical DMA reset/cache behavior,
playback continuity, reconnect soak, CPU/FPS and peak memory remain board tests.

## Runtime behavior

Three audited receive paths use an explicit single-update RX API without
changing the vendor's 0x118-byte state ABI:

- Screen: keep received ciphertext A, allocate plaintext B using the existing
  handover allocator, decrypt A -> B, and pass B to ScreenStreamProcessData.
  Cleanup releases A and B's producer reference; queued consumers retain B.
- GeneralAudio: use the caller's existing distinct input/output buffers.
- MainAlt audio: receive encrypted RTP into a temporary 1472-byte A, copy only
  the 12-byte RTP header and 24-byte tag/nonce metadata to the original node,
  then decrypt directly to that node's payload B. Jitter-buffer and decoder
  ownership do not change. Free A on either node publication or error cleanup.
  This implementation allocates A per encrypted MainAlt packet, including small
  packets, because payload size is not known until receive completes. Measure
  allocation cost on board; no extra payload-sized staging/copy-back is added.

For screen below the hardware threshold, or a buffer/metadata allocation miss,
use the original buffer with an explicitly software-only record. MainAlt A
allocation failure similarly receives into the original node and uses software.
GeneralAudio overlapping nonidentical source/destination is rejected; identical
source/destination uses software only. Registered sources stay valid until verify.

The common core preserves the original software key/nonce/AAD accumulator. A
hardware operation failure returns only after reset/quiescence, then software
retries the same record once from intact A. Delivery requires tag success.
Hardware tag mismatch is returned as authentication failure, without retry.
There is no nonce +/-1 recovery on these registered paths; the vendor performs
its normal success handling exactly once. Other callers retain their routing.

Managed nonaligned screen frames retain the three-operation hardware route
(derive Poly key, Poly1305, rounded raw ChaCha), now with distinct A and B.
Source/destination extent checks both use the managed-buffer registry. Other
layouts keep the existing exact-length/chunk-tail handling.

Persistent keys overlap the tail of vendor crypto state in both audio and
video. The existing init/verify alias wrappers still save and restore them.
Recovery outcomes are delivered to external adapter records BEFORE that restore;
they must not be read from the state tail afterward.

## DMA/IRQ recovery boundary

The SDK's `rtl8195bhp_crypto_type.h` documents secure CSR bit 31 as resetting
both crypto and DMA, and bit 3 as DMA busy. While holding the crypto device lock:

1. Disable SCrypto IRQ 29 and mark the controller inactive.
2. Write CSR reset bit 31 while the engine clock is still enabled; wait for DMA
   busy to clear, bounded by a 1 ms timeout. Clear the NVIC pending interrupt.
3. HAL deinit, reset/drain controller completion state, HAL init, rebind IRQ.
4. Return operation-local status -2 (quiesced, reinitialized) or -3 (quiesced,
   future ChaCha hardware disabled). The latter still allows software recovery.
5. Release the device lock before CPU-only software work.

If reset does not establish quiescence, fatal containment prevents buffer
reuse/free. This uses an explicit documented register condition instead of
assuming that HAL/ROM deinit returning zero proves DMA has stopped. Hardware
validation of reset and late interrupts is still required on the board.

## Build and rollback

`application.is.mk` has independent switches, both default **0**:

```sh
make -j8 ram_is SCREEN_RX_SEPARATE_BUFFER=1 AUDIO_RX_SEPARATE_BUFFER=1 \
  CROSS_COMPILE=/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-
```

The delivered image explicitly enables both. It uses CPU 300 MHz, ChaCha
threshold 1024 bytes and the existing 20 ms hardware completion timeout.
Build LP first if `application_lp/Debug/bin/application_lp.axf` is missing.
Do not run parallel `make all`. Set both switches to 0 for the existing RX route;
the tightened backend reset handling remains in either build.

The archive patcher changes only audited RX relocations in a derived archive.
Customer input archives are untouched. Exact guards cover the screen receiver,
actual `ScreenUtilsStub.o`, AppStub, AirPlayScreen and audio receiver object.
A changed object requires a fresh ABI review; do not just update its hash.

## Validation completed

- Existing host matrix: 16 combinations (four modes, two Poly1305 policies,
  AAD pool off/on).
- Existing screen Poly1305 tests: four allocation/single-pass combinations;
  AAD pool lifecycle tests off/on.
- New ASan/UBSan tests with OpenSSL vectors and the actual alias wrapper:
  lengths around 1024 and 64 KiB, 16 alignment residues, direct A/B pointers,
  three-operation screen path, unchanged ciphertext/tag, failure at key
  derivation/Poly1305/payload, corrupted partial output, combined-operation
  failure, re-init-disabled status, bad tags, AAD allocation failure, following
  nonce, overlapping persistent key/nonce restore, interleaved records and
  metadata exhaustion, screen allocation failure, MainAlt allocation failure,
  malformed encrypted packet, modeled early/late consumers and queue rejection.
- Four archive flag combinations and negative modified-vendor test; rejected
  input leaves the derived archive unchanged.
- Both feature-off and feature-on firmware link. Final ELF inspection follows
  all three receiver routes through adapters and linker veneers to the existing
  alias wrapper, and confirms the strong recovery-result callback.
- `git diff --check`.

Host tests model sockets, handover boundaries and HAL failures. They do not
validate physical DMA, the RTOS scheduler, actual audio underruns or hardware
late interrupts. A saved audio packet can still miss its playback deadline after
waiting 20 ms; the IRQ timeout's original cause is not fixed by this change.

## Logs to compare on board

`kind=0` screen, `kind=1` GeneralAudio, `kind=2` MainAlt audio.
Only failure/recovery and the first subsequent HW completion are printed:

```text
[CHACHA][HW] dma_quiesced=1 engine_ready=1 (reinitialized)
[CHACHARXREC] id=... kind=0 len=5265 hw_status=-2 dma_quiesced=1 engine_ready=1 sw_retry=1 tag_ok=1 verify_ms=...
[CHACHARXREC] id=... kind=0 recovered_delivery=0 len=5265
[CHACHARXREC] id=... kind=0 next_hw_complete=1 tag_ok=1 len=...
```

MainAlt reports `recovered_queue=0`; GeneralAudio reports
`recovered_verified=1` before returning to the caller. Screen delivery status is
ScreenStreamProcessData's result, not proof of display. Neither successful
reinitialization nor software retry proves hardware is working; the subsequent
`next_hw_complete=1 tag_ok=1` is the separate evidence. `verify_ms` includes the
failed hardware wait and software retry. Existing pre-reset timeout details stay.

## Retained artifacts

Under `GCC-RELEASE/application_is/releases/rx-recovery-20260917081901_B1C4/`:
image, matching ELF/map, build log, host logs, linked-call audit and JSON manifest.
This retains the original test image; the normal output is updated by later builds.

- Bytes: `8298496`
- SHA-256: `3b2413ec05cb0f8ed90d9ada4e2db403b085e707bae386a428bbd4380f8795a6`

## Review fix: publish the archive only after all audits pass

Fixed 2026-09-17 in `application.is.mk`. Previously the handover patch wrote the
final archive before the RX audit ran. A failed RX audit left a newer, incomplete
archive, allowing the next make to skip the audit and use the old RX call sites.
Both patch stages now operate in a unique staging directory beside the target;
only a successful result is renamed into place. Failure or handled interruption
cleans staging and leaves the previous target and its timestamp unchanged.
The recipe also depends on `application.is.mk` so this fix rebuilds an archive
left by the previous recipe.

`src/carbox/tools/test_rx_archive_publish.py` exercises the actual make recipe
with real toolchain/archive copies. It passes repeated invalid-object builds
with an existing target and with no target, verifies staging cleanup, and checks
that restored valid inputs publish all three RX adapter bindings. No customer
archive or firmware output is modified by the test.

Firmware build with both RX switches enabled passed. Current normal output:

- Version: `20260917084131_06BD`
- File: `GCC-RELEASE/application_is/flash_is.bin`
- SHA-256: `d997102cf57499b10a51d57a1afff9ba13bdf09b23570cc2c6584e40e353f544`
- Build log: `/tmp/rt8715-rx-atomic-build.log`

This fix changes archive publication, not the RX recovery algorithm. Board
validation requirements above still apply.
