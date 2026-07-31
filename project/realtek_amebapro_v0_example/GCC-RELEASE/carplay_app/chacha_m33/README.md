# chacha

This project contains a C implementation of ChaCha20 stream XOR optimized for ARM Cortex-M targets.

## RTL8195B CarPlay integration

Updated from
`/home/kevin/chacha-m33-customer-source-20260729-165055.tgz` on 2026-07-29.
The unhelpful standalone M33 assembly backend has been removed. Cortex-M33
uses the maintained C/inline-core path for every build.
`Makefile.customer` preserves the original multi-platform build file; its
host/QEMU targets reference tests that were not included in the customer
archive. The active Makefile now includes separate host tests for the three
CarPlay backend modes.

The active `Makefile` implements the CarPlay replacement flow:

1. Build the optimized Cortex-M33 software implementation and the selected
   RTL8195B backend mode.
2. Copy `../lib_CarPlay.a` without modifying the vendor archive.
3. Remove the embedded vendor `ChaCha20Poly1305.o` and any bundled
   `chacha_m33_asm.o` from the copy.
4. Insert the new `ChaCha20Poly1305.o` and
   `ChaCha20Poly1305_rtl8195b.o`.

Build and validate:

```bash
make
make check
```

Output:

```text
build/lib_CarPlay_chacha_m33.a
```

## Gradual RTL8195B hardware rollout

The application build exposes `CARBOX_CHACHA_MODE`; the chacha sub-build uses
the equivalent `CHACHA_MODE` value.

| Value | Mode | Authoritative result |
|---:|---|---|
| `0` | `SOFTWARE_ONLY` | Software only; production-safe default |
| `1` | `SOFTWARE_HW_VERIFY` | Software; hardware is run as a shadow comparison |
| `2` | `HARDWARE_ONLY` | Hardware when eligible; software only before HW submission |

While records are flowing, the routing counters print every 5 seconds. The
`route` line reports the authoritative hardware/software operation and byte
ratios. Mode 1 shadow-hardware work is listed separately as `shadow_hw`.
Set `CARBOX_CHACHA_STATS_INTERVAL_MS=0` to disable this diagnostic.

Examples:

```bash
# Baseline
make -f application.is.mk all \
  CARBOX_EXPERIMENTAL_SMART_A_LINK=1 CARBOX_CHACHA_MODE=0

# Customer validation: software output remains authoritative
make -f application.is.mk all \
  CARBOX_EXPERIMENTAL_SMART_A_LINK=1 CARBOX_CHACHA_MODE=1

# One-time target HAL capability test (diagnostic image only)
make -f application.is.mk all \
  CARBOX_EXPERIMENTAL_SMART_A_LINK=1 CARBOX_CHACHA_MODE=1 \
  CARBOX_CHACHA_HW_SELFTEST=1

# Hardware preferred; submitted HW failures are logged and not retried
make -f application.is.mk all \
  CARBOX_EXPERIMENTAL_SMART_A_LINK=1 CARBOX_CHACHA_MODE=2

# A/B copy-reduction policy: non-aligned HW ChaCha + software Poly1305
make -f application.is.mk all \
  CARBOX_EXPERIMENTAL_SMART_A_LINK=1 CARBOX_CHACHA_MODE=2 \
  CARBOX_CHACHA_NONALIGNED_SW_POLY=1
```

Messages smaller than `CARBOX_CHACHA_HW_MIN_LEN` (default 4096 bytes) stay in
software. Larger contiguous CarPlay records use one of four paths:

1. `combined-chacha-poly1305`: payload at most 64 KiB, 16-byte aligned, and
   AAD at most 496 bytes. The ROM combined engine handles ChaCha and Poly1305.
2. `standalone-chacha+hardware-poly1305`: non-combined records whose padded AEAD
   Poly1305 input fits in the standalone ROM Poly1305 64 KiB limit. Standalone
   hardware ChaCha handles a padded tail and standalone hardware Poly1305
   authenticates the true ciphertext length.
3. `standalone-chacha+software-poly1305`: when
   `CARBOX_CHACHA_NONALIGNED_SW_POLY=1`, non-16-byte-aligned records at most
   64 KiB use hardware ChaCha and streaming software Poly1305. This avoids the
   payload-sized contiguous Poly1305 input allocation and copy.
4. `chunked-chacha+software-poly1305`: records outside the standalone
   Poly1305 limit. Hardware ChaCha processes 64 KiB chunks with a continuous
   block counter; software Poly1305 authenticates the complete ciphertext.

`CARBOX_CHACHA_NONALIGNED_SW_POLY` defaults to `0`, preserving the standalone
hardware Poly1305 baseline. Build otherwise identical Mode 2 images with values
`0` and `1` to compare CPU, heap, latency, and the periodic backend counters
before changing the product default.

`CARBOX_CHACHA_HW_SELFTEST=1` adds a one-time board diagnostic immediately
before the first real hardware transaction. It checks the RFC Poly1305
one-shot vector, `init/process` single-call behavior, full-block streaming,
AEAD-segment streaming, arbitrary-byte streaming, cumulative 128 KiB Poly1305
streaming, raw ChaCha aliasing across a 64 KiB transaction boundary, and
raw/combined ChaCha input-output aliasing with cache-line offsets and guard
bytes. The test does not change production routing and defaults to `0`. Use it
only for a customer diagnostic image because it temporarily allocates about
200 KiB and delays the first eligible hardware operation while the matrix
runs. A HAL error or IRQ timeout aborts the current category after recovery;
an engine recovery failure disables hardware before the real transaction.

RTL8195B board results from 2026-07-31 establish two different boundaries:

- raw and combined ChaCha in-place tests, including cache-line offsets and a
  64 KiB chunk transition, passed;
- Poly1305 split `init/process` tests produced incorrect tags even though a
  single process call passed. Do not treat that SDK API as streaming state.

Mode 1 therefore performs shadow encryption and decryption in-place in its
disposable scratch buffers. Mode 2 is performance-first and also submits the
payload in-place, eliminating the payload-sized commit allocation and the
successful-path copy. Exact `src == dst` staging copies are skipped.

All hardware paths also require the contiguous layout observed in the supplied
CarPlay libraries, task context, and successful temporary-buffer allocation.
Mode 2 may select software only before a hardware transaction is submitted
(for example below threshold, unsupported layout, interrupt context, or
snapshot allocation failure). A submitted DMA/HAL failure is always printed as
`[CHACHA][HW][FAIL]` and is not retried because the in-place input may already
be partially overwritten. Decrypt returns the negative HAL wrapper status via
`out_error`; the legacy encrypt ABI has no error return, so it prints the
failure and clears the tag. Callers must never consume decrypt output after an
authentication or hardware error.

Every hardware transaction is serialized by `RT_DEV_LOCK_CRYPTO`. Completion
uses the RTL crypto IRQ and an RTOS semaphore with a 1000 ms timeout; timeout
falls through to the ROM's bounded completion check. Key, nonce, plaintext and
ciphertext contents are never printed.

The RTL8195B in-place capability has been board-tested, but customer rollout
must still start with mode `1`. Do not promote mode `2` until the target log shows no
`[CHACHA][VERIFY][MISMATCH]`, no IRQ timeout, and acceptable fallback coverage.
The Traditional Chinese customer procedure and acceptance criteria are in
`CUSTOMER_CHACHA_ROLLOUT_TEST_ZH-TW.md`.

Host regression, including an OpenSSL hardware mock:

```bash
make host-check
```

It checks all three modes, fragmented streaming, in-place decrypt, valid and
invalid tags, 4095/4096/4097-byte threshold behavior, standalone Poly1305
boundaries, 64 KiB and multi-chunk counter continuity, and injected failures
after a partial hardware operation. In verify
mode the original ciphertext is retained until the deferred hardware
comparison completes, because the supplied CarPlay libraries decrypt in
place. The temporary ciphertext copy is released by `verify`; allocation
failure skips only the shadow comparison and leaves the software result
authoritative. Host testing does not validate the physical RTL engine, IRQ
wiring, DMA or cache behavior.

When the repository-local toolchain is unavailable:

```bash
make CROSS_COMPILE=/absolute/path/to/newlib/bin/arm-none-eabi-
```

The replacement covers every ChaCha20-Poly1305 symbol currently referenced by
`lib_CarPlay.a` and `lib_Accessory2.a`. It intentionally does not retain unused
legacy 96x32 and raw Poly1305 entry points from the old object.

`CHACHA_ENABLE_CLEAR` remains `0` by default to preserve the delivered
performance behavior. Override it explicitly if sensitive temporary-buffer
clearing is required:

```bash
make CHACHA_ENABLE_CLEAR=1
```

The default hardware threshold can be changed explicitly:

```bash
make CHACHA_HW_MIN_LEN=4096
```

The application build forwards the equivalent
`CARBOX_CHACHA_HW_MIN_LEN` value. Keep 4096 until target measurements justify
a different crossover.

The old M33 assembly build option no longer exists. The replacement always
removes the legacy assembly object embedded in the vendor archive before
inserting the maintained objects.

## What `ChaCha20Poly1305.c` contains

- RFC7539-style ChaCha20 core:
  - 256-bit key (`32` bytes)
  - 96-bit nonce (`12` bytes)
  - 32-bit block counter
- Public API:
  - `void chacha20_xor(...)`
  - `#define chacha20_encode chacha20_xor`
  - `#define chacha20_decode chacha20_xor`
- Target split:
  - `M0` path: C implementation, alignment-safe byte/LE helpers
  - `M4` path: aligned `uint32_t` XOR with a Thumb-2 inline-assembly core
  - `M33` path: maintained M4-compatible C/inline core
  - M4/M33 retain unaligned-safe 32-bit LE fallbacks
- Auto target selection:
  - `__ARM_ARCH_8M_MAIN__` selects M33, `__ARM_ARCH_7EM__` /
    `__ARM_ARCH_7M__` selects M4-family, otherwise M0.

## Build

Local object build example:

```bash
cc -O3 -Wall -Wextra -c ChaCha20Poly1305.c -o chacha.o
```

Force target macro examples:

```bash
cc -O3 -DCHACHA_TARGET_M0=1 -c ChaCha20Poly1305.c -o chacha_m0.o
cc -O3 -DCHACHA_TARGET_M4=1 -c ChaCha20Poly1305.c -o chacha_m4.o
```

M33 compile example:

```bash
arm-none-eabi-gcc -O3 -Wall -Wextra -ffreestanding -fno-builtin \
  -mcpu=cortex-m33 -mthumb -mno-unaligned-access \
  -fomit-frame-pointer \
  -c ChaCha20Poly1305.c -o chacha_m33.o
arm-none-eabi-ar rcs libchacha_m33.a chacha_m33.o
```

## Compile-time knobs

- `CHACHA_ENABLE_CLEAR`:
  - default `0` for maximum performance
  - set `-DCHACHA_ENABLE_CLEAR=1` to clear temporary sensitive buffers
- `CHACHA_FORCE_C_M4_CORE`:
  - force the M4 block core to use C
  - useful when disabling Thumb-2 inline assembly on specific toolchains
- `CHACHA_TARGET_M33`:
  - selected automatically when the compiler defines `__ARM_ARCH_8M_MAIN__`
  - no explicit define is needed with `-mcpu=cortex-m33`

## Make Targets

- `make`: build host test runner (`chacha_runner`)
- `make arm`: build ARM bare-metal static libraries (`libchacha_m0.a`, `libchacha_m4.a`, `libchacha_m33.a`)
- `make arm-linux`: cross-build ARM Linux test runner (`chacha_runner_arm_linux`)
- `make run-arm-linux`: run ARM Linux test runner via:
  - `qemu-arm` if installed
  - Docker fallback (`linux/arm/v7`) when `qemu-arm` is unavailable

### ARM Linux test setup (brew-first)

Install cross-compiler (brew):

```bash
brew tap messense/macos-cross-toolchains
brew install messense/macos-cross-toolchains/arm-unknown-linux-gnueabihf
```

Optional user-mode QEMU (`qemu-arm`) is not provided by Homebrew on all macOS setups.
If absent, `make run-arm-linux` automatically falls back to Docker.

### ARM test workflow (same as host logic)

Build ARM Linux runner:

```bash
make arm-linux
```

Run ARM test (correctness + throughput):

```bash
make run-arm-linux
```

Run with custom benchmark parameters (`bytes_per_round rounds`):

```bash
make run-arm-linux ARGS="16777216 20"
```

### QEMU-only validation (no hardware)

When local `qemu-arm` is unavailable, you can still run ARM user-mode tests with Docker + `qemu-arm-static`:

```bash
make arm-linux
docker run --rm -v "$PWD:/work" -w /work \
  --entrypoint /usr/bin/qemu-arm-static \
  multiarch/qemu-user-static:latest \
  ./chacha_runner_arm_linux 1048576 5
```

Example multi-size validation set:

```bash
docker run --rm -v "$PWD:/work" -w /work \
  --entrypoint /usr/bin/qemu-arm-static \
  multiarch/qemu-user-static:latest \
  ./chacha_runner_arm_linux 64 50000

docker run --rm -v "$PWD:/work" -w /work \
  --entrypoint /usr/bin/qemu-arm-static \
  multiarch/qemu-user-static:latest \
  ./chacha_runner_arm_linux 256 200000

docker run --rm -v "$PWD:/work" -w /work \
  --entrypoint /usr/bin/qemu-arm-static \
  multiarch/qemu-user-static:latest \
  ./chacha_runner_arm_linux 1024 100000

docker run --rm -v "$PWD:/work" -w /work \
  --entrypoint /usr/bin/qemu-arm-static \
  multiarch/qemu-user-static:latest \
  ./chacha_runner_arm_linux 4096 50000

docker run --rm -v "$PWD:/work" -w /work \
  --entrypoint /usr/bin/qemu-arm-static \
  multiarch/qemu-user-static:latest \
  ./chacha_runner_arm_linux 1048576 200
```

Expected pass criteria:

- `"[OK] correctness (RFC7539 vector + decode round-trip)"`
- `"[OK] M4 alignment-path consistency (in/out offsets 0..3)"`
- `"[OK] throughput: ..."`

Run the bare-metal Cortex-M33 correctness suite on QEMU MPS2-AN505:

```bash
make qemu-m33
```

Run assembly, Poly1305 C fallback, and full C fallback variants:

```bash
make qemu-m33-all
```

Run the local byte-for-byte compatibility matrix against the saved reference:

```bash
make check-reference
```

Notes:

- ARM correctness is checked with the same RFC7539 vector used by host tests.
- Throughput from QEMU/Docker is useful for regression tracking, not real MCU performance.

## API usage

```c
uint8_t out[len];
chacha20_xor(out, in, len, key32, nonce12, counter);
```

Notes:

- Encryption and decryption are identical (`XOR` stream cipher).
- Reusing the same `(key, nonce, counter range)` combination is unsafe.

## ChaCha20-Poly1305 compatibility API

`ChaCha20Poly1305.h` provides the 64-bit nonce/counter streaming interface used
by Apple CoreUtils-style callers. The destination buffer must have an extra
16 bytes for the authentication tag.

Supported compatibility entry points:

- `chacha20_poly1305_init_64x64`, `chacha20_poly1305_add_aad`
- `chacha20_poly1305_encrypt`, `chacha20_poly1305_final`
- `chacha20_poly1305_decrypt`, `chacha20_poly1305_verify`
- `chacha20_poly1305_encrypt_all_64x64`
- `chacha20_poly1305_decrypt_all_64x64`

```c
chacha20_poly1305_state state;
size_t encrypted_len;

chacha20_poly1305_init_64x64(&state, key32, nonce8);
chacha20_poly1305_add_aad(&state, header, header_len);
encrypted_len = chacha20_poly1305_encrypt(
  &state, frame, frame_len, frame
);
encrypted_len += chacha20_poly1305_final(
  &state, frame + encrypted_len, frame + frame_len
);
```

For a static-library integration, place the ChaCha library after objects and
libraries that reference it:

```make
LDLIBS += path/to/libchacha_m33.a
```

Equivalent link-order example:

```bash
arm-none-eabi-gcc ... carplay_app/lib_CarPlay.a \
  carplay_app/lib_Accessory2.a path/to/libchacha_m33.a -o application
```
