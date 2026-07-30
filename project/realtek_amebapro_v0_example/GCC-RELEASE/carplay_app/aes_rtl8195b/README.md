# RTL8195B AESUtils

This directory preserves the modified CarPlay `AESUtils.c` and `AESUtils.h`.

## Configuration

- `AES_UTILS_USE_RTL_HW_AES=1`: use the RTL8195B hardware AES engine.
- `AES_UTILS_USE_RTL_HW_AES=0`: use the original software AES implementation.
- If not specified, hardware AES is enabled when `CONFIG_PLATFORM_8195BHP=1`.
- Hardware operations keep the synchronous AESUtils API, but the calling task
  sleeps on a semaphore until the RTL crypto completion IRQ fires.
- `AES_UTILS_RTL_KEY_REGISTRY_SIZE` controls the fixed-capacity private raw-key
  registry and defaults to 32 simultaneously initialized contexts.
- `AES_UTILS_RTL_IRQ_TIMEOUT_MS` controls the completion wait and defaults to
  1000 ms. The original bounded poll is used only after an IRQ timeout.
- `AES_UTILS_RTL_VERIFY_SW=1` runs hardware and software AES over the same
  runtime input and compares their output. It is disabled by default after
  hardware validation; software fallback remains enabled.
- `AES_UTILS_RTL_VERIFY_CHUNK_SIZE` is the bounded comparison buffer size and
  defaults to 15360 bytes. It must be a multiple of 16 and no larger than
  15360 bytes.

## Behavior

- CTR, CBC and ECB use hardware when available.
- Every hardware sequence is serialized with `RT_DEV_LOCK_CRYPTO`, covering
  engine initialization/self-test, AES key/mode initialization, the operation,
  and IRQ completion.
- AESUtils replaces the HAL adapter's `pre_exec_func`, `wait_done_func`, and IRQ
  callback with an IRQ-to-semaphore bridge. The original IRQ bookkeeping,
  descriptor handling, and pre/post-DMA cache maintenance remain in the HAL.
- On an IRQ timeout, the original bounded polling wait is used once as a
  recovery check. A timeout or HAL operation error resets the crypto engine
  before later software fallback is allowed. If reset fails, software fallback
  remains blocked to prevent CPU and DMA from touching the same buffer.
- In verification mode, the first successful comparison for each mode prints
  `AES HW/SW MATCH`. Every mismatch prints the mode and first differing byte.
  A mismatch returns the software result to the caller so the runtime test can
  continue. Hardware remains enabled so every later mismatch is also reported.
- If a HAL operation returns an error, that error is returned to the caller and
  software is not run over the same buffer because the HAL does not guarantee
  that DMA has stopped.
- GCM retains the original software streaming implementation.
- Initialization, self-test or runtime hardware errors permanently fall back to
  software AES and print one prominent `AES HW DISABLED` message. Hardware
  initialization logging remains unchanged.
- Calls from ISR context use software AES without printing from the ISR.
- A startup hardware/software self-test validates ECB, CBC and CTR behavior.

## Integration

- Public AES context layouts remain unchanged. Raw hardware keys are stored in
  a private registry keyed by context address.
- Rebuild `AESUtils.c` and replace the corresponding AES object/functions in
  `lib_CarPlay.a`, or rebuild the complete customer library with these files.
- Keep the original software AES objects linked for self-test and fallback.
- The RTL crypto HAL, `device_lock`, and OS mutex/semaphore services must remain
  linked.
- Keep the RTL SDK includes at the top of `AESUtils.c`, before `AESUtils.h`.
  Realtek defines `u8/u16/u32/u64` as macros, and application headers may
  undefine them while leaving the `basic_types.h` include guard set.
