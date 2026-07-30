# RTL8195B AES optional diagnostic build

## Purpose

This source package runs the RTL8195B AES hardware path first and then runs the
original software AES implementation over a saved copy of the same input and
initial state. The software result is used only for verification and to replace
a mismatched hardware result.

## Production setting

The validated production build disables the hardware/software comparison while
keeping software fallback and its `AES HW DISABLED` log:

```c
#define AES_UTILS_USE_RTL_HW_AES          1
#define AES_UTILS_RTL_VERIFY_SW           0
#define AES_UTILS_RTL_IRQ_TIMEOUT_MS      1000
```

## Optional re-validation settings

Only enable these settings when another hardware/software comparison run is
required:

```c
#define AES_UTILS_USE_RTL_HW_AES          1
#define AES_UTILS_RTL_VERIFY_SW           1
#define AES_UTILS_RTL_VERIFY_CHUNK_SIZE   15360
#define AES_UTILS_RTL_KEY_REGISTRY_SIZE   32
```

`AES_UTILS_RTL_VERIFY_CHUNK_SIZE` must be a positive multiple of 16 and must not
exceed 15360.

The build must link the RTL crypto HAL, `device_lock`, the OS mutex service, and
the original software AES implementation.

## Hardware locking and completion

Every hardware sequence uses `RT_DEV_LOCK_CRYPTO`:

```text
device_mutex_lock(RT_DEV_LOCK_CRYPTO)
    crypto_init/self-test when needed
    AES key and mode initialization
    submit HAL AES DMA
    block the calling task on the completion semaphore
        crypto IRQ: acknowledge + give semaphore from ISR
    HAL destination-cache maintenance and return
device_mutex_unlock(RT_DEV_LOCK_CRYPTO)
```

Software verification and diagnostic printing run after the device lock is
released. The AESUtils private mutex remains held to protect the raw-key
registry and shared verification buffers.

AESUtils replaces `g_rtl_cryptoEngine_s.pre_exec_func`, `wait_done_func`, and
the IRQ callback. It preserves the HAL's original IRQ bookkeeping and leaves
descriptor setup plus source/destination cache maintenance inside the HAL.
The ISR does not access output data, invalidate cache, take a mutex, or print.

If the semaphore wait times out, AESUtils prints `AES HW IRQ TIMEOUT` and calls
the original bounded polling wait once. If completion still fails, it resets
the crypto engine before allowing a later software fallback. HAL operation
errors use the same reset rule. If reset fails, AESUtils prints
`software fallback blocked` and continues returning an error rather than
risking concurrent DMA/CPU access to the caller buffer.

## Expected logs

The first successful comparison for each mode/direction prints:

```text
AES HW/SW MATCH: mode=...
```

Every mismatch prints:

```text
AES HW/SW MISMATCH: mode=... offset=... chunk_len=... byte=...
hw=... sw=... src_align=... dst_align=... in_place=...
```

Mismatch reporting is not suppressed after the first failure. The software
result replaces the mismatched hardware result so the diagnostic run can
continue.

Any mismatch is a test failure.

## HAL errors

If a HAL operation returns an error, AESUtils disables future hardware use and
returns that error to the caller. It does not immediately run software over the
same buffer because a HAL error does not prove that DMA has stopped.

Initialization failures occur before an operation starts and may safely use the
software path.

## Acceptance criteria

- No `AES HW/SW MISMATCH` messages.
- No `AES HW DISABLED` messages.
- No `AES HW IRQ TIMEOUT` messages.
- No key-registry full or miss messages.
- No TLS, certificate, hash, or HMAC failures while CarPlay AES is active.
- No buffer-canary corruption in aligned, unaligned, in-place, and separate
  buffer tests.
- CBC encrypt/decrypt and CTR carry tests pass at 16, 32, 1024, 15360, and
  multi-fragment lengths.
- CPU-load tracing confirms the AES task is blocked, rather than runnable,
  between DMA submission and the crypto completion IRQ.
