# Screen RX Poly1305 input reuse

`SCREEN_RX_POLY_INPLACE=1` enables an experimental mode-2 screen RX fast path.
The firmware default is **1**, enabled for board validation at the user's
request. All existing runtime profiling defaults remain off. The standalone
crypto-library Makefile retains 0 unless the firmware build passes the option,
because it does not itself supply the screen allocation/ownership hooks.
Customer archives are not edited; the existing derived-archive hooks are used.

## Layout and lifetime

The receiver reads a 128-byte screen header separately, adds it as ChaCha AAD,
and allocates `ciphertext_length + 16` bytes for ciphertext and the received tag.
The allocation hook can now return a 32-byte-aligned payload pointer with a
128-byte prefix. The allocation also retains the previous 15-byte readable
tail allowance and owns the entire final cache line.

```
malloc base ... alignment ... | AAD (128) | ciphertext | padding | lengths (16)
                                          ^ unchanged RX/payload pointer
```

Only non-16-aligned ciphertext of at least 4096 bytes whose complete Poly1305
input fits 65536 bytes receives a prefixed allocation. A separate 64-entry
table records the malloc base, payload, wire length and allocating task.
Allocation failure or a full table falls back to the old padded allocation.
No code reads metadata before an unrecognized pointer.

The fast path requires the exact payload pointer and length, 128-byte AAD and
the allocating task. It is used only by standalone mode-2 RX authentication,
never by TX. It saves the received tag, writes AAD/padding/lengths around the
unchanged ciphertext, submits hardware Poly1305, and restores the tag even on
HAL failure. Restoration happens before raw ChaCha decrypt, final comparison,
or nonce recovery. No retry is introduced after hardware submission. Existing
HAL cache maintenance is retained. The 64 KiB hardware limit is unchanged.

## Single-pass raw ChaCha RX

`SCREEN_RX_CHACHA_SINGLE_PASS=1` is now the firmware default. After standalone
hardware Poly1305 has consumed the logical ciphertext, an owned, nonaligned
screen RX buffer can be decrypted in a single raw-ChaCha submission with
`counter=1` and hardware length `ROUND_UP(ciphertext_length, 16)`. Wire length,
AAD length, authentication input and the caller's returned byte count do not
change. This does not use the unsupported partial combined-decrypt API.

The helper saves the original 16-byte wire tag, zeroes only the rounded tail,
submits in-place raw ChaCha, and restores the tag on both success and failure.
It propagates hardware errors without retrying. Existing HAL cache handling
and the transaction lock remain in place. For eligible records, the main
hardware calls fall from four (key generation, Poly1305, prefix XOR, tail XOR)
to three (key generation, Poly1305, one rounded XOR).

This optimization borrows the allocation metadata of `SCREEN_RX_POLY_INPLACE`.
It requires mode 2, an exact in-place payload, matching producer task, 128-byte
AAD and a Poly1305 input that fits 64 KiB. Unknown or exhausted-table fallback
allocations, out-of-place calls, combined decrypt and other backends retain
their existing behavior. Disabling `SCREEN_RX_POLY_INPLACE` also removes the
eligible managed allocations, even if the single-pass switch stays at 1.

For an independent A/B comparison, keep `SCREEN_RX_POLY_INPLACE=1` and build
with `SCREEN_RX_CHACHA_SINGLE_PASS=0` or `1`. The firmware build forwards both
options to the replacement archive and tracks both in its configuration
dependencies. A standalone library build defaults the new option to 0.
`CHACHA_RUNTIME_PROFILE=1` adds a submitted-operation count `rx_single_pass`
to the existing transaction report; profiling remains disabled by default.

The existing handover producer/consumer reference counts determine the final
release. At that point the new release helper finds and frees the malloc base.
Untracked pointers, including calloc-created receiver contexts and ordinary
TX buffers, continue through ordinary free. The separate allocation table is
not cleared by handover-disable/error handling, so those paths can still free
prefixed buffers correctly.

## Closed-library audit

The enabled build checks hashes of the following vendor objects using
`src/carbox/tools/check_screen_rx_poly_vendor.sh`. A different object requires
a new audit; do not simply update the expected hash.

* `AirPlayReceiverSessionScreen.o`: ProcessFrames has one payload malloc
  relocation at +0x16c, a receive-error free at +0x1b8, and a common payload
  cleanup free at +0x44a. Decrypt/verify errors flow to the common cleanup.
  Delete's free at +0x2a releases a separately calloc-created context. All
  malloc/free relocations in this object are redirected by the existing
  receiver archive patch. No realloc reference exists.
* `ScreenUtils.o`: ScreenStreamProcessData borrows the RX pointer and does not
  free it. Its optional conversion allocation has its own free. SetAVCC
  converts into separately allocated storage; Stop releases that storage.
* `AppleCarPlay_AppStub.o`: DecoderRender passes a borrowed pointer/length to
  the synchronous application callback; it does not free the RX buffer.
* `AirPlayScreen.o`: normal screen-consumer release and queue teardown frees
  are object-wide redirected to the existing consumer hook. The wire buffer
  uses its existing direct-crypto/TCP ACK ownership path.

This audit applies to the current forwarding callback and existing handover
wrappers. Any new callback that takes ownership must use the same release
contract. Host tests cannot prove closed-library scheduling or DMA behavior.

## Tests and builds

From this directory:

```
make host-check
make host-screen-rx-poly-check
```

The second target runs all four combinations of Poly1305 reuse and single-pass
RX with AddressSanitizer and
UndefinedBehaviorSanitizer and an OpenSSL hardware mock. It checks actual
Poly1305 input pointer identity, plaintext/tag equivalence, all nonaligned
length residues, 4 KiB/64 KiB boundaries, streaming updates, corrupted tags,
key-generation/Poly1305/raw-ChaCha failures, restoration of the original tag,
wrong task/AAD/span fallback, table exhaustion and slot reuse. It also tests
all 48 non-16-aligned residues within a 64-byte ChaCha block, asserts the raw
submission count, counter, rounded length and input/output pointer identity,
and injects a failure after raw output has already been written. FreeRTOS is
modeled as single-threaded: IRQ/cache/queue stress still needs the board.
Environments without LeakSanitizer support can use
`ASAN_OPTIONS=detect_leaks=0:handle_segv=0`; address/undefined checks remain on.

From `GCC-RELEASE`, after a valid LP build:

```
make -j8 ram_is SCREEN_RX_POLY_INPLACE=1 CROSS_COMPILE=/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-
```

Use `SCREEN_RX_POLY_INPLACE=0` for the scratch-buffer A/B baseline. The allocation object
and derived crypto archive have configuration dependencies for both directions
of this switch. A clean build must follow the repository's sequential
clean, `-j1 -B ram_lp`, then `-j8 ram_is` procedure.

For board A/B measurement, `CHACHA_RUNTIME_PROFILE=1` adds `inplace` and
`saved_bytes` to the existing scratch report, when that report is scheduled
by the selected profiler configuration. Compare the same screen workload;
iperf does not use this crypto path. Validate reconnects, queue pressure,
authentication errors, memory usage and long runs on the board.

## Initial implementation validation (2026-09-15, before changing the default)

* Existing host matrix: modes 0/1/2/3 with both nonaligned-Poly policies passed.
* New enabled/disabled sanitizer tests passed, including malloc failure. This
  environment used `ASAN_OPTIONS=detect_leaks=0:handle_segv=0`; LeakSanitizer
  was not used. Board cache coherency and task interleaving were not tested.
* Vendor-object guard accepted current inputs and rejected an incorrect
  archive. No vendor `.a` was modified.
* Sequential clean/LP/IS enabled build passed. Link-map cross references show
  the local ChaCha object calling `carbox_screen_rx_poly_input`.
* Incremental switch back to 0 passed. Its final ELF contains neither the
  allocation tracking table nor the Poly1305 prefix lookup function.
* Enabled test image: `application_is/flash_is_rx_poly_inplace.bin`, SHA-256
  `6419f814272237b74cbba11efaac2bb6836f07a9b7eb29da72fe23099e60ac08`.
  Matching ELF/map: `application_is/Debug/bin/application_is_rx_poly_inplace.*`.
* Initial disabled image: `application_is/flash_is.bin`, SHA-256
  `4fb8a3164f3c46a011e15ac18e931c80af45ff575f008d8737169d24f7ad0e86`.

Image paths above are relative to `GCC-RELEASE`. Generated images are ignored
by git and a later clean build removes them. Throughput improvement remains
unmeasured until the enabled image is tested on the board.

The firmware default was subsequently changed to 1. A normal build now writes
the enabled image to `application_is/flash_is.bin`; the disabled-image hash
above describes the earlier validation artifact, not the current default.

## Single-pass validation (2026-09-15)

All four Poly1305/single-pass host combinations passed ASan/UBSan with the
environment options above. The existing eight-case mode/nonaligned-Poly
matrix also passed. The mock verified one rounded payload call at counter 1
instead of prefix/tail calls, original tag restoration after output-writing
failure, and no retry after submission. Both single-pass-off and default-on
incremental firmware builds linked successfully. ARM object disassembly
shows the owned-buffer check and saved-tag/rounded-XOR path in verify.

Current firmware defaults: Poly1305 reuse **1**, single-pass RX **1**, touch
move sampling **0**; diagnostic profiles remain off. No board was flashed.

* Default enabled image: `application_is/flash_is.bin`, SHA-256
  `99fc5400a85c5ae42da8d9663ac9f992a5cfa44839e756cac2cc72152da2c7d7`.
* Single-pass-off baseline (Poly1305 reuse still on):
  `application_is/flash_is_rx_chacha_single_pass_off.bin`, SHA-256
  `ac3da993c331b1cc06d9e39cb0ccb33d9a61256de1c1d071a4fa6275d5353c15`.

These are build-time and mock results, not measured hardware throughput or
cache-coherency validation. Board A/B and reconnect/queue-pressure soak tests
remain necessary to measure the gain and validate DMA completion behavior.
