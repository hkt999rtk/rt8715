# Reusable ChaCha AAD snapshots

`CHACHA_AAD_POOL=1` is the firmware default. Set it to `0` for an independent
A/B baseline. The standalone crypto Makefile defaults it to 0; the firmware
build passes its selected value and tracks it in the archive configuration.
The screen Poly1305 and single-pass switches remain independent.

The local replacement uses sixteen 128-byte, 32-byte-aligned LPDDR slots and a
32-bit ownership bitmap. A nonzero-mode snapshot of up to 128 bytes tries the
pool without waiting. A slot belongs to one crypto state until encrypt final
or decrypt verify releases it, not to a particular task. The existing state
structure and customer ABI are unchanged. Software-only mode has no pool.

Multiple AAD updates fitting the slot reuse the same pointer. Growth beyond
128 bytes allocates heap storage, preserves the old capacity, then releases
the slot. The AAD append immediately fills the newly requested bytes. If
allocation fails, the original pointer and data stay valid so the existing
software fallback and final cleanup still work. Requests larger than 128
bytes, pool exhaustion, and already heap-backed snapshots use ordinary
realloc. Only the two AAD cleanup sites recognize pool storage; generic free
and RX/TX payload ownership are untouched.

The bounded atomic bitmap operations provide publication/acquisition ordering
without a pool mutex or wait. AAD contents are retained in a free slot, like
the previous non-clearing heap release; every requested snapshot byte is
initialized by the AAD append before use. No key material is stored in this
pool. Added static storage is 2048 bytes plus the 4-byte bitmap. Profiles and
debug-message defaults are unchanged.

## Validation

From `carplay_app/chacha_m33`:

```
make host-check
make host-aad-pool-check
make host-screen-rx-poly-check
```

The host mode matrix covers modes 0/1/2/3, both nonaligned-Poly policies, and
pool off/on (16 combinations). Existing snapshot mutation, software fallback,
bad-tag and hardware-failure cases use the actual pooled allocator through
the fault-injection seam.

The dedicated ASan/UBSan test checks pool off/on, zero heap realloc calls for
the first sixteen live 128-byte snapshots, the seventeenth heap fallback,
snapshot independence, 63+65-byte AAD accumulation, 128-to-129-to-512 growth,
growth failure, invalid-tag cleanup, slot reuse and zero-length AAD. A
32-thread barrier test checks distinct slots and fallback allocations while
all owners are live, then validates data and concurrent release.

The screen regression target tests all four Poly1305/single-pass combinations
with the AAD pool enabled. This environment runs sanitizer tests with
`ASAN_OPTIONS=detect_leaks=0:handle_segv=0` because LeakSanitizer is unsupported;
address and undefined-behavior checks remain enabled. Host threading does not
validate Cortex-M33 interrupts or the hardware DMA/cache path.

For firmware A/B, after verifying the LP image exists, run from `GCC-RELEASE`:

```
make -j8 ram_is CHACHA_AAD_POOL=0 CROSS_COMPILE=/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-
make -j8 ram_is CROSS_COMPILE=/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-
```

The second command uses the enabled default. No board was flashed as part of
implementation. Allocation tests demonstrate the removed heap operations;
actual throughput and long-run board behavior remain unmeasured.

### Workspace results (2026-09-15)

All 16 mode combinations, both pool sanitizer variants, and all four screen
regression combinations passed. Both pool-off and default-on firmware builds
linked successfully using the existing validated LP image. Final ELF symbols
confirm the enabled 2048-byte pool and 4-byte bitmap. Compile flags confirm
Poly1305 reuse and single-pass RX remain enabled and runtime profiling is off.

* Enabled `application_is/flash_is.bin` SHA-256:
  `7d715a72e4ef474b25005108d1e32ee08ab0c70dea1f279d7cd252d95b861e10`.
* AAD-pool-off baseline `application_is/flash_is_chacha_aad_pool_off.bin`:
  `4932f7a3592aeccbb3bf92f75afb886434cc1cc0ec2b1d7b94ea6f93b504ebdf`.

Paths are relative to `GCC-RELEASE`; generated images are ignored by git.
