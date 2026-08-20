#include "ChaCha20Poly1305.h"
#include "ChaCha20Poly1305_rtl8195b.h"

#if CARBOX_CHACHA_MODE != CARBOX_CHACHA_MODE_SOFTWARE_ONLY

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basic_types.h"
#include "crypto_api.h"
#include "hal_crypto.h"
#include "rtl8195bhp_crypto_ctrl.h"
#include "device_lock.h"
#include "osdep_service.h"
#include "crypto_priority_lock.h"
#include "crypto_engine_profiler.h"

#define CHACHA_RTL_MAX_MESSAGE 65536u
#define CHACHA_RTL_MAX_AAD       496u

extern hal_crypto_adapter_t g_rtl_cryptoEngine_s;
extern int rtw_in_interrupt(void);

extern hal_crypto_func_stubs_t hal_crypto_stubs_s;

typedef char chacha_combined_encrypt_stub_offset_must_match_rom[
  (offsetof(hal_crypto_func_stubs_t, rtl_crypto_chacha_poly1305_encrypt) ==
   0x13cu) ? 1 : -1
];
typedef char chacha_combined_decrypt_stub_offset_must_match_rom[
  (offsetof(hal_crypto_func_stubs_t, rtl_crypto_chacha_poly1305_decrypt) ==
   0x140u) ? 1 : -1
];

static int g_chacha_hw_state;
static uint8_t g_chacha_hw_key[32] __attribute__((aligned(32)));
static uint8_t g_chacha_hw_nonce[32] __attribute__((aligned(32)));
static uint8_t g_chacha_hw_aad[CHACHA_RTL_MAX_AAD] __attribute__((aligned(32)));
static uint8_t g_chacha_hw_tag[32] __attribute__((aligned(32)));

#if CARBOX_CHACHA_HW_SELFTEST
static int g_chacha_hw_selftest_started;
static int chacha_rtl_selftest_locked(void);
#endif

#if CARBOX_CHACHA_COMBINED_PARTIAL_SELFTEST
static int g_chacha_partial_selftest_started;
static void chacha_rtl_partial_selftest_locked(void);

typedef struct {
  volatile uint32_t state; /* 0=not run, 1=running, 2=pass, 3=fail */
  volatile uint32_t cases;
  volatile uint32_t passed;
  volatile uint32_t api_errors;
  volatile uint32_t mismatches;
  volatile uint32_t guard_errors;
  volatile uint32_t rounded_tail_write_cases;
  volatile uint32_t rounded_tail_bytes_changed;
  volatile uint32_t rounded_tail_max_changed;
  volatile uint32_t encrypt_ok;
  volatile uint32_t decrypt_ok;
  volatile uint32_t last_len;
  volatile uint32_t last_layout;
  volatile uint32_t last_stage;
  volatile int32_t last_error;
  uintptr_t encrypt_stub;
  uintptr_t decrypt_stub;
} chacha_partial_selftest_stats_t;

static chacha_partial_selftest_stats_t g_chacha_partial_selftest;
#endif

void chacha_rtl8195b_partial_selftest_report(unsigned window_index) {
#if CARBOX_CHACHA_COMBINED_PARTIAL_SELFTEST
  const chacha_partial_selftest_stats_t *s = &g_chacha_partial_selftest;
  printf(
    "[CHACHAPARTIAL][%u] state=%lu cases/pass=%lu/%lu "
    "enc/dec_ok=%lu/%lu api_error/mismatch/guard=%lu/%lu/%lu "
    "rounded_tail cases/bytes/max=%lu/%lu/%lu "
    "last len/layout/stage/error=%lu/%lu/%lu/%ld "
    "stub enc/dec=%08lx/%08lx production_route=unchanged\n",
    window_index, (unsigned long)s->state, (unsigned long)s->cases,
    (unsigned long)s->passed, (unsigned long)s->encrypt_ok,
    (unsigned long)s->decrypt_ok, (unsigned long)s->api_errors,
    (unsigned long)s->mismatches, (unsigned long)s->guard_errors,
    (unsigned long)s->rounded_tail_write_cases,
    (unsigned long)s->rounded_tail_bytes_changed,
    (unsigned long)s->rounded_tail_max_changed,
    (unsigned long)s->last_len, (unsigned long)s->last_layout,
    (unsigned long)s->last_stage, (long)s->last_error,
    (unsigned long)s->encrypt_stub, (unsigned long)s->decrypt_stub
  );
#else
  (void)window_index;
#endif
}

/*
 * The public streaming encrypt API cannot report a DMA-quiesce failure.
 * Continuing would permit fallback code to reuse/free memory still referenced
 * by DMA, so the safe default is fail-stop and let the watchdog reset the
 * platform. A product may override this weak hook with its reset routine.
 */
__attribute__((weak, noreturn))
void carbox_chacha_hw_fatal(const char *reason) {
  printf("[CHACHA][HW][FATAL] %s; fallback blocked\n", reason);
  for (;;) {}
}

static void chacha_rtl_clear_key_material(void) {
  volatile uint8_t *p;
  size_t i;

  p = g_chacha_hw_key;
  for (i = 0; i < sizeof(g_chacha_hw_key); ++i) p[i] = 0u;
  p = g_chacha_hw_nonce;
  for (i = 0; i < sizeof(g_chacha_hw_nonce); ++i) p[i] = 0u;
  p = g_chacha_hw_tag;
  for (i = 0; i < sizeof(g_chacha_hw_tag); ++i) p[i] = 0u;
}

/* RT_DEV_LOCK_CRYPTO must be held. */
static int chacha_rtl_prepare_locked(void) {
  int result;

  if (g_chacha_hw_state < 0) return CHACHA_RTL_ERROR_INIT;

  if (!g_rtl_cryptoEngine_s.isInit) {
    result = rtl_cryptoEngine_init();
    if (result != 0) {
      g_chacha_hw_state = -1;
      printf("[CHACHA][HW] crypto_init failed: err=%d\n", result);
      return CHACHA_RTL_ERROR_INIT;
    }
  }

  result = carbox_crypto_irq_controller_enable();
  if (result != 0) {
    g_chacha_hw_state = -1;
    printf("[CHACHA][HW] unified IRQ controller init failed\n");
    return CHACHA_RTL_ERROR_INIT;
  }

#if CARBOX_CHACHA_COMBINED_PARTIAL_SELFTEST
  if (!g_chacha_partial_selftest_started) {
    g_chacha_partial_selftest_started = 1;
    chacha_rtl_partial_selftest_locked();
    if (g_chacha_hw_state < 0) return CHACHA_RTL_ERROR_INIT;
  }
#endif

#if CARBOX_CHACHA_HW_SELFTEST
  if (!g_chacha_hw_selftest_started) {
    g_chacha_hw_selftest_started = 1;
    result = chacha_rtl_selftest_locked();
    if (result != CHACHA_RTL_OK) return result;
  }
#endif

  if (g_chacha_hw_state == 0) {
    g_chacha_hw_state = 1;
    printf(
      "[CHACHA][HW] RTL8195B unified IRQ backend initialized "
      "(not board-validated)\n"
    );
  }
  return CHACHA_RTL_OK;
}

/*
 * A failed HAL operation may leave a late DMA completion behind. Quiesce and
 * reinitialize the shared engine before the caller touches its software
 * fallback buffers. Reinitializing also avoids leaving AES with a stale
 * "available" state after a ChaCha-side reset.
 */
static int chacha_rtl_recover_locked(int dma_may_be_active) {
  int deinit_result = rtl_cryptoEngine_deinit();
  int init_result = -1;

  carbox_crypto_irq_controller_engine_reset();

  if (deinit_result != 0) {
    g_chacha_hw_state = -1;
    printf(
      "[CHACHA][HW] engine quiesce failed: deinit_err=%d\n",
      deinit_result
    );
    if (dma_may_be_active) {
      carbox_chacha_hw_fatal("DMA quiesce failed");
    }
    return 1;
  }

  /*
   * A successful deinit is the safety boundary: DMA is stopped and software
   * fallback may touch caller buffers. Re-init failure only disables future HW.
   */
  init_result = rtl_cryptoEngine_init();
  if (init_result == 0) {
    g_chacha_hw_state = 0;
    if (carbox_crypto_irq_controller_enable() == 0) {
      printf("[CHACHA][HW] engine recovered after operation failure\n");
    } else {
      g_chacha_hw_state = -1;
      init_result = -1;
    }
  } else {
    g_chacha_hw_state = -1;
    printf(
      "[CHACHA][HW] engine recovery failed: deinit_err=%d init_err=%d; "
      "future hardware use disabled\n",
      deinit_result, init_result
    );
  }
  return 1;
}

static int chacha_rtl_validate(size_t msg_len, size_t aad_len) {
  if ((msg_len == 0u) || (msg_len > CHACHA_RTL_MAX_MESSAGE) ||
      ((msg_len & 15u) != 0u)) {
    return CHACHA_RTL_SKIP_LENGTH;
  }
  if (aad_len > CHACHA_RTL_MAX_AAD) {
    return CHACHA_RTL_SKIP_AAD_LENGTH;
  }
  if (rtw_in_interrupt()) {
    return CHACHA_RTL_SKIP_INTERRUPT;
  }
  return CHACHA_RTL_OK;
}

static int chacha_rtl_validate_raw(size_t msg_len, int require_alignment) {
  if ((msg_len == 0u) || (msg_len > CHACHA_RTL_MAX_MESSAGE) ||
      (require_alignment && ((msg_len & 15u) != 0u))) {
    return CHACHA_RTL_SKIP_LENGTH;
  }
  if (rtw_in_interrupt()) {
    return CHACHA_RTL_SKIP_INTERRUPT;
  }
  return CHACHA_RTL_OK;
}

int chacha_rtl8195b_precheck_context(void) {
  return rtw_in_interrupt() ?
         CHACHA_RTL_SKIP_INTERRUPT : CHACHA_RTL_OK;
}

static void chacha_rtl_stage_parameters(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len
) {
  memset(g_chacha_hw_nonce, 0, sizeof(g_chacha_hw_nonce));
  memset(g_chacha_hw_aad, 0, sizeof(g_chacha_hw_aad));
  memcpy(g_chacha_hw_key, key, 32);

  /*
   * The CarPlay API is the original 64-bit nonce/64-bit counter variant.
   * RTL uses the IETF 96-bit nonce form. CarPlay records start at counter 1,
   * so word 0 of the 96-bit nonce is the zero high half of the counter.
   */
  memcpy(g_chacha_hw_nonce + 4, nonce, 8);
  if (aad_len != 0u) memcpy(g_chacha_hw_aad, aad, aad_len);
}

#if CARBOX_CHACHA_COMBINED_PARTIAL_SELFTEST

#define CHACHA_PARTIAL_GUARD_SIZE 32u

static int chacha_partial_equal(
  const uint8_t *a, const uint8_t *b, size_t len
) {
  size_t i;
  uint8_t diff = 0u;
  for (i = 0u; i < len; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0u;
}

static int chacha_partial_guard_ok(
  const uint8_t *data, size_t len, uint8_t value
) {
  size_t i;
  for (i = 0u; i < len; ++i) {
    if (data[i] != value) return 0;
  }
  return 1;
}

static void chacha_partial_fill(uint8_t *data, size_t len, uint8_t seed) {
  size_t i;
  uint32_t state = 0x9e3779b9u ^ seed;
  for (i = 0u; i < len; ++i) {
    state = state * 1664525u + 1013904223u;
    data[i] = (uint8_t)(state >> 24);
  }
}

static void chacha_partial_record_tail_change(
  const uint8_t *before, const uint8_t *after, size_t len
) {
  chacha_partial_selftest_stats_t *s = &g_chacha_partial_selftest;
  size_t i;
  uint32_t changed = 0u;
  for (i = 0u; i < len; ++i) {
    if (before[i] != after[i]) ++changed;
  }
  if (changed != 0u) {
    ++s->rounded_tail_write_cases;
    s->rounded_tail_bytes_changed += changed;
    if (changed > s->rounded_tail_max_changed) {
      s->rounded_tail_max_changed = changed;
    }
  }
}

static int chacha_partial_direct_operation(
  int decrypt, const uint8_t *input, size_t len, uint8_t *output,
  uint8_t tag[16]
) {
  int result = rtl_crypto_chacha_poly1305_init(g_chacha_hw_key);
  if (result != 0) return result;

  /*
   * Reverse-engineering note (RTL8195B ROM):
   *
   * The public rtl_crypto_chacha_poly1305_{encrypt,decrypt} functions in
   * lib_soc_is.a reject (msglen & 15) before entering ROM.  Disassembly of
   * hal_crypto_s.o shows that, after this policy check, they tail-call these
   * exact members of hal_crypto_stubs_s (offsets 0x13c and 0x140).  Calling
   * the members directly therefore bypasses only the public alignment guard;
   * key setup, descriptors, DMA, and the unified IRQ completion path remain
   * the vendor implementation.
   *
   * Board probing established an asymmetric result.  Partial ENCRYPT consumes
   * the original logical len and produces the correct ciphertext/tag, but its
   * destination is written through ROUND_UP(len, 16).  Partial DECRYPT is
   * rejected by the ROM stub with -14.  Therefore this bypass is currently an
   * encrypt-only capability; it must not be used for the RX/decrypt route.
   *
   * Every encrypt caller must provide at least ROUND_UP(len, 16) writable
   * destination capacity.  Never pass the rounded length to ROM: doing so
   * changes the AEAD record length and therefore its authentication tag.  This
   * probe is deliberately disabled in normal builds because recovering the
   * shared crypto engine after an experimental failure during PairSetup can
   * disrupt pairing.
   */
  if (decrypt) {
    result = hal_crypto_stubs_s.rtl_crypto_chacha_poly1305_decrypt(
      input, (uint32_t)len, g_chacha_hw_nonce,
      g_chacha_hw_aad, 2u, output, g_chacha_hw_tag
    );
  } else {
    result = hal_crypto_stubs_s.rtl_crypto_chacha_poly1305_encrypt(
      input, (uint32_t)len, g_chacha_hw_nonce,
      g_chacha_hw_aad, 2u, output, g_chacha_hw_tag
    );
  }
  if (result == 0) memcpy(tag, g_chacha_hw_tag, 16u);
  return result;
}

/* RT_DEV_LOCK_CRYPTO is held and the unified IRQ controller is installed. */
static void chacha_rtl_partial_selftest_locked(void) {
  static const size_t lengths[] = {
    1u, 15u, 17u, 31u, 33u, 4095u, 4097u, 65535u
  };
  static const uint8_t key[32] = {
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f
  };
  static const uint8_t nonce[8] = {
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27
  };
  static const uint8_t aad[2] = {0x50, 0x51};
  const size_t max_len = 65535u;
  const size_t max_rounded_len = (max_len + 15u) & ~(size_t)15u;
  const size_t arena_len =
    max_rounded_len + 2u * CHACHA_PARTIAL_GUARD_SIZE;
  chacha_partial_selftest_stats_t *s = &g_chacha_partial_selftest;
  uint8_t *plain_raw = NULL;
  uint8_t *reference_raw = NULL;
  uint8_t *work_raw = NULL;
  uint8_t *decrypt_raw = NULL;
  uint8_t *plain;
  uint8_t *reference;
  uint8_t *work_arena;
  uint8_t *decrypt_arena;
  uint8_t reference_tag[16];
  uint8_t hardware_tag[16];
  uint8_t tail_before[16];
  size_t li;
  unsigned layout;

  memset(s, 0, sizeof(*s));
  s->state = 1u;
  s->encrypt_stub = (uintptr_t)
    hal_crypto_stubs_s.rtl_crypto_chacha_poly1305_encrypt;
  s->decrypt_stub = (uintptr_t)
    hal_crypto_stubs_s.rtl_crypto_chacha_poly1305_decrypt;

  plain_raw = (uint8_t *)malloc(max_rounded_len + 31u);
  reference_raw = (uint8_t *)malloc(max_rounded_len + 31u);
  work_raw = (uint8_t *)malloc(arena_len + 31u);
  decrypt_raw = (uint8_t *)malloc(arena_len + 31u);
  if (!plain_raw || !reference_raw || !work_raw || !decrypt_raw) {
    s->api_errors = 1u;
    s->last_error = -1001;
    s->state = 3u;
    goto cleanup;
  }

  plain = (uint8_t *)(((uintptr_t)plain_raw + 31u) & ~(uintptr_t)31u);
  reference =
    (uint8_t *)(((uintptr_t)reference_raw + 31u) & ~(uintptr_t)31u);
  work_arena =
    (uint8_t *)(((uintptr_t)work_raw + 31u) & ~(uintptr_t)31u);
  decrypt_arena =
    (uint8_t *)(((uintptr_t)decrypt_raw + 31u) & ~(uintptr_t)31u);
  chacha_partial_fill(plain, max_rounded_len, 0x71u);

  for (li = 0u; li < sizeof(lengths) / sizeof(lengths[0]); ++li) {
    const size_t len = lengths[li];
    const size_t rounded_len = (len + 15u) & ~(size_t)15u;
    const size_t tail_len = rounded_len - len;

    chacha20_poly1305_reference_encrypt_all_64x64(
      key, nonce, aad, sizeof(aad), plain, len, reference, reference_tag
    );

    for (layout = 0u; layout < 2u; ++layout) {
      uint8_t *work = work_arena + CHACHA_PARTIAL_GUARD_SIZE;
      uint8_t *decrypted =
        decrypt_arena + CHACHA_PARTIAL_GUARD_SIZE;
      const uint8_t *encrypt_input;
      uint8_t *encrypt_output;
      const uint8_t *decrypt_input;
      uint8_t *decrypt_output;
      int result;
      int guards_ok;

      ++s->cases;
      s->last_len = (uint32_t)len;
      s->last_layout = layout; /* 0=out-of-place, 1=in-place */
      memset(work_arena, 0xa5, arena_len);
      memset(decrypt_arena, 0x5a, arena_len);
      if (layout != 0u) memcpy(work, plain, len);
      encrypt_input = (layout != 0u) ? work : plain;
      encrypt_output = work;
      if (tail_len != 0u) memcpy(tail_before, work + len, tail_len);

      chacha_rtl_stage_parameters(key, nonce, aad, sizeof(aad));
      s->last_stage = 1u; /* direct ROM encrypt */
      result = chacha_partial_direct_operation(
        0, encrypt_input, len, encrypt_output, hardware_tag
      );
      s->last_error = result;
      if (result != 0) {
        ++s->api_errors;
        (void)chacha_rtl_recover_locked(1);
        s->state = 3u;
        goto cleanup;
      }
      ++s->encrypt_ok;

      if (tail_len != 0u) {
        chacha_partial_record_tail_change(
          tail_before, work + len, tail_len
        );
      }

      guards_ok =
        chacha_partial_guard_ok(
          work - CHACHA_PARTIAL_GUARD_SIZE,
          CHACHA_PARTIAL_GUARD_SIZE, 0xa5
        ) &&
        chacha_partial_guard_ok(
          work + rounded_len, CHACHA_PARTIAL_GUARD_SIZE, 0xa5
        );
      s->last_stage = 2u; /* encrypt comparison */
      if (!guards_ok) ++s->guard_errors;
      if (!chacha_partial_equal(work, reference, len) ||
          !chacha_partial_equal(hardware_tag, reference_tag, 16u)) {
        ++s->mismatches;
      }
      if (!guards_ok || s->mismatches != 0u) {
        s->state = 3u;
        goto cleanup;
      }

      decrypt_input = work;
      decrypt_output = (layout != 0u) ? work : decrypted;
      if (tail_len != 0u) {
        memcpy(tail_before, decrypt_output + len, tail_len);
      }
      chacha_rtl_stage_parameters(key, nonce, aad, sizeof(aad));
      s->last_stage = 3u; /* direct ROM decrypt */
      result = chacha_partial_direct_operation(
        1, decrypt_input, len, decrypt_output, hardware_tag
      );
      s->last_error = result;
      if (result != 0) {
        ++s->api_errors;
        (void)chacha_rtl_recover_locked(1);
        s->state = 3u;
        goto cleanup;
      }
      ++s->decrypt_ok;

      if (tail_len != 0u) {
        chacha_partial_record_tail_change(
          tail_before, decrypt_output + len, tail_len
        );
      }

      guards_ok = (layout != 0u) ?
        (chacha_partial_guard_ok(
           work - CHACHA_PARTIAL_GUARD_SIZE,
           CHACHA_PARTIAL_GUARD_SIZE, 0xa5
         ) &&
         chacha_partial_guard_ok(
           work + rounded_len, CHACHA_PARTIAL_GUARD_SIZE, 0xa5
         )) :
        (chacha_partial_guard_ok(
           decrypted - CHACHA_PARTIAL_GUARD_SIZE,
           CHACHA_PARTIAL_GUARD_SIZE, 0x5a
         ) &&
         chacha_partial_guard_ok(
           decrypted + rounded_len, CHACHA_PARTIAL_GUARD_SIZE, 0x5a
         ));
      s->last_stage = 4u; /* decrypt comparison */
      if (!guards_ok) ++s->guard_errors;
      if (!chacha_partial_equal(decrypt_output, plain, len) ||
          !chacha_partial_equal(hardware_tag, reference_tag, 16u)) {
        ++s->mismatches;
      }
      if (!guards_ok || s->mismatches != 0u) {
        s->state = 3u;
        goto cleanup;
      }
      ++s->passed;
    }
  }
  s->state = 2u;

cleanup:
  printf(
    "[CHACHAPARTIAL] %s cases/pass=%lu/%lu "
    "api_error/mismatch/guard=%lu/%lu/%lu "
    "rounded_tail=%lu/%lu/%lu last=%lu/%lu/%lu/%ld\n",
    (s->state == 2u) ? "PASS" : "FAIL",
    (unsigned long)s->cases, (unsigned long)s->passed,
    (unsigned long)s->api_errors, (unsigned long)s->mismatches,
    (unsigned long)s->guard_errors,
    (unsigned long)s->rounded_tail_write_cases,
    (unsigned long)s->rounded_tail_bytes_changed,
    (unsigned long)s->rounded_tail_max_changed,
    (unsigned long)s->last_len,
    (unsigned long)s->last_layout, (unsigned long)s->last_stage,
    (long)s->last_error
  );
  chacha_rtl_clear_key_material();
  if (decrypt_raw) free(decrypt_raw);
  if (work_raw) free(work_raw);
  if (reference_raw) free(reference_raw);
  if (plain_raw) free(plain_raw);
}

#endif /* CARBOX_CHACHA_COMBINED_PARTIAL_SELFTEST */

#if CARBOX_CHACHA_HW_SELFTEST

#define CHACHA_SELFTEST_GUARD_SIZE 32u
#define CHACHA_SELFTEST_MAX_RAW    65536u
#define CHACHA_SELFTEST_CHUNKED_RAW (65536u + 64u)
#define CHACHA_SELFTEST_POLY_LARGE  (2u * 65536u)
#define CHACHA_SELFTEST_MISMATCH     (-32767)

static int chacha_selftest_equal(
  const uint8_t *a, const uint8_t *b, size_t len
) {
  size_t i;
  uint8_t diff = 0u;
  for (i = 0u; i < len; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0u;
}

static int chacha_selftest_guard(
  const uint8_t *buffer, size_t len, uint8_t value
) {
  size_t i;
  for (i = 0u; i < len; ++i) {
    if (buffer[i] != value) return 0;
  }
  return 1;
}

static void chacha_selftest_fill(uint8_t *buffer, size_t len, uint8_t seed) {
  size_t i;
  uint32_t value = (uint32_t)seed + 1u;
  for (i = 0u; i < len; ++i) {
    value = value * 1664525u + 1013904223u;
    buffer[i] = (uint8_t)(value >> 24);
  }
}

static int chacha_selftest_poly_process(
  const uint8_t *message, const size_t *parts, size_t part_count,
  const uint8_t reference[16]
) {
  uint8_t digest[32] __attribute__((aligned(32)));
  size_t offset = 0u;
  size_t i;
  int result;

  memset(digest, 0, sizeof(digest));
  result = rtl_crypto_poly1305_init(g_chacha_hw_key);
  if (result != 0) {
    (void)chacha_rtl_recover_locked(0);
    return result;
  }
  for (i = 0u; i < part_count; ++i) {
    result = rtl_crypto_poly1305_process(
      message + offset, (uint32_t)parts[i], digest
    );
    if (result != 0) {
      (void)chacha_rtl_recover_locked(1);
      return result;
    }
    offset += parts[i];
  }
  return chacha_selftest_equal(digest, reference, 16u)
    ? 0 : CHACHA_SELFTEST_MISMATCH;
}

static unsigned chacha_rtl_selftest_poly1305_locked(void) {
  static const uint8_t rfc_key[32] = {
    0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33,
    0x7f, 0x44, 0x52, 0xfe, 0x42, 0xd5, 0x06, 0xa8,
    0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d, 0xb2, 0xfd,
    0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b
  };
  static const uint8_t rfc_message[] =
    "Cryptographic Forum Research Group";
  static const uint8_t rfc_tag[16] = {
    0xa8, 0x06, 0x1d, 0xc1, 0x30, 0x51, 0x36, 0xc6,
    0xc2, 0x2b, 0x8b, 0xaf, 0x0c, 0x01, 0x27, 0xa9
  };
  static const size_t single_parts[] = {64u};
  static const size_t block_parts[] = {16u, 16u, 32u};
  static const size_t aead_parts[] = {16u, 32u, 16u};
  static const size_t byte_parts[] = {2u, 14u, 17u, 15u, 16u};
  static const size_t large_parts[] = {65536u, 65536u};
  static const uint8_t large_key[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
  };
  /*
   * Poly1305(key=00..1f, message=the deterministic 128 KiB fill below).
   * Generated independently from RFC 8439's Poly1305 definition.
   */
  static const uint8_t large_tag[16] = {
    0x7b, 0x5e, 0x1b, 0xff, 0xe1, 0x25, 0x8b, 0xe0,
    0x69, 0xe6, 0xda, 0x58, 0xd6, 0x71, 0x81, 0x17
  };
  uint8_t message[64] __attribute__((aligned(32)));
  uint8_t reference[32] __attribute__((aligned(32)));
  uint8_t *large_raw = NULL;
  uint8_t *large_message;
  unsigned failures = 0u;
  size_t i;
  int result;

  memcpy(g_chacha_hw_key, rfc_key, sizeof(rfc_key));
  memset(reference, 0, sizeof(reference));
  result = rtl_crypto_poly1305(
    rfc_message, (uint32_t)(sizeof(rfc_message) - 1u),
    g_chacha_hw_key, reference
  );
  if ((result == 0) && chacha_selftest_equal(reference, rfc_tag, 16u)) {
    printf("[POLY1305][SELFTEST] oneshot-known PASS\n");
  } else {
    ++failures;
    printf("[POLY1305][SELFTEST] oneshot-known FAIL err=%d\n", result);
    if (result != 0) {
      (void)chacha_rtl_recover_locked(1);
      return failures;
    }
  }

  /*
   * RFC 8439 AEAD MAC layout:
   * AAD(2) || pad(14) || ciphertext(17) || pad(15) || lengths(16).
   */
  memset(message, 0, sizeof(message));
  message[0] = 0x50u;
  message[1] = 0x51u;
  for (i = 0u; i < 17u; ++i) message[16u + i] = (uint8_t)(0x80u + i);
  message[48] = 2u;
  message[56] = 17u;

  memcpy(g_chacha_hw_key, rfc_key, sizeof(rfc_key));
  memset(reference, 0, sizeof(reference));
  result = rtl_crypto_poly1305(
    message, (uint32_t)sizeof(message), g_chacha_hw_key, reference
  );
  if (result != 0) {
    ++failures;
    printf("[POLY1305][SELFTEST] layout-reference FAIL err=%d\n", result);
    (void)chacha_rtl_recover_locked(1);
    return failures;
  }

#define CHACHA_RUN_POLY_TEST(label, parts) do {                         \
    result = chacha_selftest_poly_process(                              \
      message, (parts), sizeof(parts) / sizeof((parts)[0]), reference   \
    );                                                                  \
    if (result == 0) {                                                  \
      printf("[POLY1305][SELFTEST] " label " PASS\n");                 \
    } else {                                                            \
      ++failures;                                                       \
      printf("[POLY1305][SELFTEST] " label " FAIL err=%d\n", result);  \
      if (result != CHACHA_SELFTEST_MISMATCH) return failures;          \
    }                                                                   \
  } while (0)

  CHACHA_RUN_POLY_TEST("init-process-single", single_parts);
  CHACHA_RUN_POLY_TEST("block-stream", block_parts);
  CHACHA_RUN_POLY_TEST("aead-segments", aead_parts);
  CHACHA_RUN_POLY_TEST("byte-stream", byte_parts);
#undef CHACHA_RUN_POLY_TEST

  large_raw = (uint8_t *)malloc(CHACHA_SELFTEST_POLY_LARGE + 31u);
  if (!large_raw) {
    ++failures;
    printf("[POLY1305][SELFTEST] cumulative-128k FAIL allocation\n");
    return failures;
  }
  large_message =
    (uint8_t *)(((uintptr_t)large_raw + 31u) & ~(uintptr_t)31u);
  chacha_selftest_fill(
    large_message, CHACHA_SELFTEST_POLY_LARGE, 0x5bu
  );
  memcpy(g_chacha_hw_key, large_key, sizeof(large_key));
  result = chacha_selftest_poly_process(
    large_message, large_parts,
    sizeof(large_parts) / sizeof(large_parts[0]), large_tag
  );
  if (result == 0) {
    printf("[POLY1305][SELFTEST] cumulative-128k PASS\n");
  } else {
    ++failures;
    printf(
      "[POLY1305][SELFTEST] cumulative-128k FAIL err=%d\n", result
    );
  }
  free(large_raw);

  return failures;
}

static int chacha_selftest_raw_operation(
  int decrypt, uint32_t counter,
  const uint8_t *input, size_t len, uint8_t *output
) {
  int result = rtl_crypto_chacha_init(g_chacha_hw_key);
  if (result != 0) {
    (void)chacha_rtl_recover_locked(0);
    return result;
  }
  if (decrypt) {
    result = rtl_crypto_chacha_decrypt(
      input, (uint32_t)len, g_chacha_hw_nonce, counter, output
    );
  } else {
    result = rtl_crypto_chacha_encrypt(
      input, (uint32_t)len, g_chacha_hw_nonce, counter, output
    );
  }
  if (result != 0) (void)chacha_rtl_recover_locked(1);
  return result;
}

static unsigned chacha_rtl_selftest_raw_inplace_locked(void) {
  static const size_t lengths[] = {16u, 64u, 4096u, 65536u};
  static const size_t offsets[] = {0u, 1u, 15u, 16u, 31u};
  static const uint8_t key[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
  };
  static const uint8_t nonce[8] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17
  };
  const size_t arena_size =
    CHACHA_SELFTEST_MAX_RAW + 3u * CHACHA_SELFTEST_GUARD_SIZE + 31u;
  uint8_t *plain_raw = NULL;
  uint8_t *reference_raw = NULL;
  uint8_t *arena_raw = NULL;
  uint8_t *plain;
  uint8_t *reference;
  uint8_t *arena;
  unsigned failures = 0u;
  unsigned cases = 0u;
  size_t li;
  size_t oi;

  plain_raw = (uint8_t *)malloc(CHACHA_SELFTEST_MAX_RAW + 31u);
  reference_raw = (uint8_t *)malloc(CHACHA_SELFTEST_MAX_RAW + 31u);
  arena_raw = (uint8_t *)malloc(arena_size + 31u);
  if (!plain_raw || !reference_raw || !arena_raw) {
    printf("[CHACHA][SELFTEST][INPLACE-RAW] FAIL allocation\n");
    failures = 1u;
    goto cleanup;
  }
  plain = (uint8_t *)(((uintptr_t)plain_raw + 31u) & ~(uintptr_t)31u);
  reference =
    (uint8_t *)(((uintptr_t)reference_raw + 31u) & ~(uintptr_t)31u);
  arena = (uint8_t *)(((uintptr_t)arena_raw + 31u) & ~(uintptr_t)31u);
  chacha_selftest_fill(plain, CHACHA_SELFTEST_MAX_RAW, 0x35u);

  for (li = 0u; li < sizeof(lengths) / sizeof(lengths[0]); ++li) {
    size_t len = lengths[li];
    int result;

    chacha_rtl_stage_parameters(key, nonce, NULL, 0u);
    result = chacha_selftest_raw_operation(0, 1u, plain, len, reference);
    if (result != 0) {
      ++failures;
      printf(
        "[CHACHA][SELFTEST][INPLACE-RAW] reference FAIL len=%lu err=%d\n",
        (unsigned long)len, result
      );
      goto cleanup;
    }

    for (oi = 0u; oi < sizeof(offsets) / sizeof(offsets[0]); ++oi) {
      size_t offset = offsets[oi];
      uint8_t *buffer = arena + CHACHA_SELFTEST_GUARD_SIZE + offset;
      int guards_ok;

      ++cases;
      memset(arena, 0xa5, arena_size);
      memcpy(buffer, plain, len);
      chacha_rtl_stage_parameters(key, nonce, NULL, 0u);
      result = chacha_selftest_raw_operation(0, 1u, buffer, len, buffer);
      guards_ok =
        chacha_selftest_guard(
          buffer - CHACHA_SELFTEST_GUARD_SIZE,
          CHACHA_SELFTEST_GUARD_SIZE, 0xa5
        ) &&
        chacha_selftest_guard(
          buffer + len, CHACHA_SELFTEST_GUARD_SIZE, 0xa5
        );
      if (result != 0) {
        ++failures;
        printf(
          "[CHACHA][SELFTEST][INPLACE-RAW] encrypt FAIL "
          "len=%lu offset=%lu err=%d guard=%d\n",
          (unsigned long)len, (unsigned long)offset, result, guards_ok
        );
        goto cleanup;
      }
      if (!chacha_selftest_equal(buffer, reference, len) ||
          !guards_ok) {
        ++failures;
        printf(
          "[CHACHA][SELFTEST][INPLACE-RAW] encrypt FAIL "
          "len=%lu offset=%lu err=%d guard=%d\n",
          (unsigned long)len, (unsigned long)offset, result, guards_ok
        );
        continue;
      }

      chacha_rtl_stage_parameters(key, nonce, NULL, 0u);
      result = chacha_selftest_raw_operation(1, 1u, buffer, len, buffer);
      guards_ok =
        chacha_selftest_guard(
          buffer - CHACHA_SELFTEST_GUARD_SIZE,
          CHACHA_SELFTEST_GUARD_SIZE, 0xa5
        ) &&
        chacha_selftest_guard(
          buffer + len, CHACHA_SELFTEST_GUARD_SIZE, 0xa5
        );
      if (result != 0) {
        ++failures;
        printf(
          "[CHACHA][SELFTEST][INPLACE-RAW] decrypt FAIL "
          "len=%lu offset=%lu err=%d guard=%d\n",
          (unsigned long)len, (unsigned long)offset, result, guards_ok
        );
        goto cleanup;
      }
      if (!chacha_selftest_equal(buffer, plain, len) ||
          !guards_ok) {
        ++failures;
        printf(
          "[CHACHA][SELFTEST][INPLACE-RAW] decrypt FAIL "
          "len=%lu offset=%lu err=%d guard=%d\n",
          (unsigned long)len, (unsigned long)offset, result, guards_ok
        );
      }
    }
  }

cleanup:
  printf(
    "[CHACHA][SELFTEST][INPLACE-RAW] %s cases=%u failures=%u\n",
    failures ? "FAIL" : "PASS", cases, failures
  );
  if (arena_raw) free(arena_raw);
  if (reference_raw) free(reference_raw);
  if (plain_raw) free(plain_raw);
  return failures;
}

static int chacha_selftest_raw_chunks(
  int decrypt, const uint8_t *input, size_t len, uint8_t *output
) {
  size_t offset = 0u;
  uint32_t counter = 1u;

  while (offset < len) {
    size_t chunk_len = len - offset;
    uint32_t blocks;
    int result;

    if (chunk_len > CHACHA_RTL_MAX_MESSAGE) {
      chunk_len = CHACHA_RTL_MAX_MESSAGE;
    }
    result = chacha_selftest_raw_operation(
      decrypt, counter, input + offset, chunk_len, output + offset
    );
    if (result != 0) return result;
    blocks = (uint32_t)((chunk_len + 63u) / 64u);
    counter += blocks;
    offset += chunk_len;
  }
  return 0;
}

static unsigned chacha_rtl_selftest_raw_chunked_inplace_locked(void) {
  static const size_t offsets[] = {0u, 1u, 31u};
  static const uint8_t key[32] = {
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f
  };
  static const uint8_t nonce[8] = {
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67
  };
  const size_t arena_size =
    CHACHA_SELFTEST_CHUNKED_RAW +
    3u * CHACHA_SELFTEST_GUARD_SIZE + 31u;
  uint8_t *plain_raw = NULL;
  uint8_t *reference_raw = NULL;
  uint8_t *arena_raw = NULL;
  uint8_t *plain;
  uint8_t *reference;
  uint8_t *arena;
  unsigned failures = 0u;
  unsigned cases = 0u;
  size_t oi;
  int result;

  plain_raw = (uint8_t *)malloc(CHACHA_SELFTEST_CHUNKED_RAW + 31u);
  reference_raw = (uint8_t *)malloc(CHACHA_SELFTEST_CHUNKED_RAW + 31u);
  arena_raw = (uint8_t *)malloc(arena_size + 31u);
  if (!plain_raw || !reference_raw || !arena_raw) {
    failures = 1u;
    printf("[CHACHA][SELFTEST][INPLACE-CHUNKED] FAIL allocation\n");
    goto cleanup;
  }
  plain = (uint8_t *)(((uintptr_t)plain_raw + 31u) & ~(uintptr_t)31u);
  reference =
    (uint8_t *)(((uintptr_t)reference_raw + 31u) & ~(uintptr_t)31u);
  arena = (uint8_t *)(((uintptr_t)arena_raw + 31u) & ~(uintptr_t)31u);
  chacha_selftest_fill(plain, CHACHA_SELFTEST_CHUNKED_RAW, 0x49u);

  chacha_rtl_stage_parameters(key, nonce, NULL, 0u);
  result = chacha_selftest_raw_chunks(
    0, plain, CHACHA_SELFTEST_CHUNKED_RAW, reference
  );
  if (result != 0) {
    ++failures;
    printf(
      "[CHACHA][SELFTEST][INPLACE-CHUNKED] reference FAIL err=%d\n",
      result
    );
    goto cleanup;
  }

  for (oi = 0u; oi < sizeof(offsets) / sizeof(offsets[0]); ++oi) {
    size_t offset = offsets[oi];
    uint8_t *buffer = arena + CHACHA_SELFTEST_GUARD_SIZE + offset;
    int guards_ok;

    ++cases;
    memset(arena, 0xc3, arena_size);
    memcpy(buffer, plain, CHACHA_SELFTEST_CHUNKED_RAW);
    chacha_rtl_stage_parameters(key, nonce, NULL, 0u);
    result = chacha_selftest_raw_chunks(
      0, buffer, CHACHA_SELFTEST_CHUNKED_RAW, buffer
    );
    guards_ok =
      chacha_selftest_guard(
        buffer - CHACHA_SELFTEST_GUARD_SIZE,
        CHACHA_SELFTEST_GUARD_SIZE, 0xc3
      ) &&
      chacha_selftest_guard(
        buffer + CHACHA_SELFTEST_CHUNKED_RAW,
        CHACHA_SELFTEST_GUARD_SIZE, 0xc3
      );
    if (result != 0) {
      ++failures;
      printf(
        "[CHACHA][SELFTEST][INPLACE-CHUNKED] encrypt FAIL "
        "offset=%lu err=%d guard=%d\n",
        (unsigned long)offset, result, guards_ok
      );
      goto cleanup;
    }
    if (!chacha_selftest_equal(
          buffer, reference, CHACHA_SELFTEST_CHUNKED_RAW
        ) || !guards_ok) {
      ++failures;
      printf(
        "[CHACHA][SELFTEST][INPLACE-CHUNKED] encrypt FAIL "
        "offset=%lu err=0 guard=%d\n",
        (unsigned long)offset, guards_ok
      );
      continue;
    }

    chacha_rtl_stage_parameters(key, nonce, NULL, 0u);
    result = chacha_selftest_raw_chunks(
      1, buffer, CHACHA_SELFTEST_CHUNKED_RAW, buffer
    );
    guards_ok =
      chacha_selftest_guard(
        buffer - CHACHA_SELFTEST_GUARD_SIZE,
        CHACHA_SELFTEST_GUARD_SIZE, 0xc3
      ) &&
      chacha_selftest_guard(
        buffer + CHACHA_SELFTEST_CHUNKED_RAW,
        CHACHA_SELFTEST_GUARD_SIZE, 0xc3
      );
    if (result != 0) {
      ++failures;
      printf(
        "[CHACHA][SELFTEST][INPLACE-CHUNKED] decrypt FAIL "
        "offset=%lu err=%d guard=%d\n",
        (unsigned long)offset, result, guards_ok
      );
      goto cleanup;
    }
    if (!chacha_selftest_equal(
          buffer, plain, CHACHA_SELFTEST_CHUNKED_RAW
        ) || !guards_ok) {
      ++failures;
      printf(
        "[CHACHA][SELFTEST][INPLACE-CHUNKED] decrypt FAIL "
        "offset=%lu err=0 guard=%d\n",
        (unsigned long)offset, guards_ok
      );
    }
  }

cleanup:
  printf(
    "[CHACHA][SELFTEST][INPLACE-CHUNKED] %s cases=%u failures=%u "
    "len=%lu\n",
    failures ? "FAIL" : "PASS", cases, failures,
    (unsigned long)CHACHA_SELFTEST_CHUNKED_RAW
  );
  if (arena_raw) free(arena_raw);
  if (reference_raw) free(reference_raw);
  if (plain_raw) free(plain_raw);
  return failures;
}

static int chacha_selftest_combined_operation(
  int decrypt, const uint8_t *input, size_t len, uint8_t *output,
  uint8_t tag[16]
) {
  int result = rtl_crypto_chacha_poly1305_init(g_chacha_hw_key);
  if (result != 0) {
    (void)chacha_rtl_recover_locked(0);
    return result;
  }
  if (decrypt) {
    result = rtl_crypto_chacha_poly1305_decrypt(
      input, (uint32_t)len, g_chacha_hw_nonce,
      g_chacha_hw_aad, 2u, output, g_chacha_hw_tag
    );
  } else {
    result = rtl_crypto_chacha_poly1305_encrypt(
      input, (uint32_t)len, g_chacha_hw_nonce,
      g_chacha_hw_aad, 2u, output, g_chacha_hw_tag
    );
  }
  if (result == 0) memcpy(tag, g_chacha_hw_tag, 16u);
  else (void)chacha_rtl_recover_locked(1);
  return result;
}

static unsigned chacha_rtl_selftest_combined_inplace_locked(void) {
  static const size_t lengths[] = {16u, 4096u, 65536u};
  static const size_t offsets[] = {0u, 1u, 31u};
  static const uint8_t key[32] = {
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f
  };
  static const uint8_t nonce[8] = {
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27
  };
  static const uint8_t aad[2] = {0x50, 0x51};
  const size_t max_len = 65536u;
  const size_t arena_size =
    max_len + 3u * CHACHA_SELFTEST_GUARD_SIZE + 31u;
  uint8_t *plain_raw = NULL;
  uint8_t *reference_raw = NULL;
  uint8_t *arena_raw = NULL;
  uint8_t *plain;
  uint8_t *reference;
  uint8_t *arena;
  uint8_t reference_tag[16];
  uint8_t inplace_tag[16];
  unsigned failures = 0u;
  unsigned cases = 0u;
  size_t li;
  size_t oi;

  plain_raw = (uint8_t *)malloc(max_len + 31u);
  reference_raw = (uint8_t *)malloc(max_len + 31u);
  arena_raw = (uint8_t *)malloc(arena_size + 31u);
  if (!plain_raw || !reference_raw || !arena_raw) {
    printf("[CHACHA][SELFTEST][INPLACE-COMBINED] FAIL allocation\n");
    failures = 1u;
    goto cleanup;
  }
  plain = (uint8_t *)(((uintptr_t)plain_raw + 31u) & ~(uintptr_t)31u);
  reference =
    (uint8_t *)(((uintptr_t)reference_raw + 31u) & ~(uintptr_t)31u);
  arena = (uint8_t *)(((uintptr_t)arena_raw + 31u) & ~(uintptr_t)31u);
  chacha_selftest_fill(plain, max_len, 0x71u);

  for (li = 0u; li < sizeof(lengths) / sizeof(lengths[0]); ++li) {
    size_t len = lengths[li];
    int result;

    chacha_rtl_stage_parameters(key, nonce, aad, sizeof(aad));
    result = chacha_selftest_combined_operation(
      0, plain, len, reference, reference_tag
    );
    if (result != 0) {
      ++failures;
      printf(
        "[CHACHA][SELFTEST][INPLACE-COMBINED] reference FAIL "
        "len=%lu err=%d\n",
        (unsigned long)len, result
      );
      goto cleanup;
    }

    for (oi = 0u; oi < sizeof(offsets) / sizeof(offsets[0]); ++oi) {
      size_t offset = offsets[oi];
      uint8_t *buffer = arena + CHACHA_SELFTEST_GUARD_SIZE + offset;
      int guards_ok;

      ++cases;
      memset(arena, 0x5a, arena_size);
      memcpy(buffer, plain, len);
      chacha_rtl_stage_parameters(key, nonce, aad, sizeof(aad));
      result = chacha_selftest_combined_operation(
        0, buffer, len, buffer, inplace_tag
      );
      guards_ok =
        chacha_selftest_guard(
          buffer - CHACHA_SELFTEST_GUARD_SIZE,
          CHACHA_SELFTEST_GUARD_SIZE, 0x5a
        ) &&
        chacha_selftest_guard(
          buffer + len, CHACHA_SELFTEST_GUARD_SIZE, 0x5a
        );
      if (result != 0) {
        ++failures;
        printf(
          "[CHACHA][SELFTEST][INPLACE-COMBINED] encrypt FAIL "
          "len=%lu offset=%lu err=%d guard=%d\n",
          (unsigned long)len, (unsigned long)offset, result, guards_ok
        );
        goto cleanup;
      }
      if (!chacha_selftest_equal(buffer, reference, len) ||
          !chacha_selftest_equal(inplace_tag, reference_tag, 16u) ||
          !guards_ok) {
        ++failures;
        printf(
          "[CHACHA][SELFTEST][INPLACE-COMBINED] encrypt FAIL "
          "len=%lu offset=%lu err=%d guard=%d\n",
          (unsigned long)len, (unsigned long)offset, result, guards_ok
        );
        continue;
      }

      chacha_rtl_stage_parameters(key, nonce, aad, sizeof(aad));
      result = chacha_selftest_combined_operation(
        1, buffer, len, buffer, inplace_tag
      );
      guards_ok =
        chacha_selftest_guard(
          buffer - CHACHA_SELFTEST_GUARD_SIZE,
          CHACHA_SELFTEST_GUARD_SIZE, 0x5a
        ) &&
        chacha_selftest_guard(
          buffer + len, CHACHA_SELFTEST_GUARD_SIZE, 0x5a
        );
      if (result != 0) {
        ++failures;
        printf(
          "[CHACHA][SELFTEST][INPLACE-COMBINED] decrypt FAIL "
          "len=%lu offset=%lu err=%d guard=%d\n",
          (unsigned long)len, (unsigned long)offset, result, guards_ok
        );
        goto cleanup;
      }
      if (!chacha_selftest_equal(buffer, plain, len) ||
          !chacha_selftest_equal(inplace_tag, reference_tag, 16u) ||
          !guards_ok) {
        ++failures;
        printf(
          "[CHACHA][SELFTEST][INPLACE-COMBINED] decrypt FAIL "
          "len=%lu offset=%lu err=%d guard=%d\n",
          (unsigned long)len, (unsigned long)offset, result, guards_ok
        );
      }
    }
  }

cleanup:
  printf(
    "[CHACHA][SELFTEST][INPLACE-COMBINED] %s cases=%u failures=%u "
    "(capability-only)\n",
    failures ? "FAIL" : "PASS", cases, failures
  );
  if (arena_raw) free(arena_raw);
  if (reference_raw) free(reference_raw);
  if (plain_raw) free(plain_raw);
  return failures;
}

static int chacha_rtl_selftest_locked(void) {
  unsigned poly_failures = 0u;
  unsigned raw_failures = 0u;
  unsigned chunked_failures = 0u;
  unsigned combined_failures = 0u;

  printf(
    "[CHACHA][SELFTEST] BEGIN diagnostic-only; "
    "production routing unchanged\n"
  );
  poly_failures = chacha_rtl_selftest_poly1305_locked();
  if (g_chacha_hw_state < 0) goto finish;
  raw_failures = chacha_rtl_selftest_raw_inplace_locked();
  if (g_chacha_hw_state < 0) goto finish;
  chunked_failures = chacha_rtl_selftest_raw_chunked_inplace_locked();
  if (g_chacha_hw_state < 0) goto finish;
  combined_failures = chacha_rtl_selftest_combined_inplace_locked();

finish:
  chacha_rtl_clear_key_material();
  printf(
    "[CHACHA][SELFTEST] END poly_failures=%u raw_inplace_failures=%u "
    "chunked_inplace_failures=%u combined_inplace_failures=%u\n",
    poly_failures, raw_failures, chunked_failures, combined_failures
  );
  if (g_chacha_hw_state < 0) {
    printf(
      "[CHACHA][SELFTEST] hardware disabled after recovery failure\n"
    );
    return CHACHA_RTL_ERROR_INIT;
  }
  return CHACHA_RTL_OK;
}

#endif /* CARBOX_CHACHA_HW_SELFTEST */

static int chacha_rtl_run(
  int decrypt,
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *input, size_t input_len,
  void *output, uint8_t tag[16]
) {
  int result;

  result = chacha_rtl_validate(input_len, aad_len);
  if (result != CHACHA_RTL_OK) return result;

  carbox_crypto_chacha_device_lock(RT_DEV_LOCK_CRYPTO);
  result = chacha_rtl_prepare_locked();
  if (result == CHACHA_RTL_OK) {
    chacha_rtl_stage_parameters(key, nonce, aad, aad_len);
    result = rtl_crypto_chacha_poly1305_init(g_chacha_hw_key);
    if (result == 0) {
      if (decrypt) {
        result = rtl_crypto_chacha_poly1305_decrypt(
          (const uint8_t *)input, (uint32_t)input_len,
          g_chacha_hw_nonce, g_chacha_hw_aad, (uint32_t)aad_len,
          (uint8_t *)output, g_chacha_hw_tag
        );
      } else {
        result = rtl_crypto_chacha_poly1305_encrypt(
          (const uint8_t *)input, (uint32_t)input_len,
          g_chacha_hw_nonce, g_chacha_hw_aad, (uint32_t)aad_len,
          (uint8_t *)output, g_chacha_hw_tag
        );
      }
      if (result == 0) {
        memcpy(tag, g_chacha_hw_tag, 16);
        result = CHACHA_RTL_OK;
      } else {
        printf("[CHACHA][HW] operation failed: decrypt=%d err=%d\n",
               decrypt, result);
        (void)chacha_rtl_recover_locked(1);
        result = CHACHA_RTL_ERROR_OPERATION;
      }
    } else {
      printf("[CHACHA][HW] key init failed: err=%d\n", result);
      /*
       * No data operation was submitted, so caller buffers are not owned by
       * DMA. Recovery is still useful but fallback remains safe after a
       * successful deinit even if re-init fails.
       */
      (void)chacha_rtl_recover_locked(0);
      result = CHACHA_RTL_ERROR_OPERATION;
    }
  }
  chacha_rtl_clear_key_material();
  carbox_crypto_chacha_device_unlock(RT_DEV_LOCK_CRYPTO);
  return result;
}

int chacha_rtl8195b_encrypt(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *plaintext, size_t plaintext_len,
  void *ciphertext, uint8_t tag[16]
) {
  return chacha_rtl_run(
    0, key, nonce, aad, aad_len,
    plaintext, plaintext_len, ciphertext, tag
  );
}

int chacha_rtl8195b_encrypt_partial_padded(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *plaintext, size_t plaintext_len,
  void *ciphertext, uint8_t tag[16]
) {
  int result;
  uint32_t profile_start_us;

  if ((plaintext_len == 0u) || (plaintext_len > CHACHA_RTL_MAX_MESSAGE) ||
      ((plaintext_len & 15u) == 0u)) {
    return CHACHA_RTL_SKIP_LENGTH;
  }
  if (aad_len > CHACHA_RTL_MAX_AAD) return CHACHA_RTL_SKIP_AAD_LENGTH;
  if (rtw_in_interrupt()) return CHACHA_RTL_SKIP_INTERRUPT;

  carbox_crypto_chacha_device_lock(RT_DEV_LOCK_CRYPTO);
  result = chacha_rtl_prepare_locked();
  if (result == CHACHA_RTL_OK) {
    chacha_rtl_stage_parameters(key, nonce, aad, aad_len);
    result = rtl_crypto_chacha_poly1305_init(g_chacha_hw_key);
    if (result == 0) {
      /*
       * The public SDK wrapper rejects msg_len % 16 before entering ROM.
       * Board reverse-engineering proved that the underlying encrypt stub
       * produces the correct logical ciphertext/tag, while DMA writes up to
       * 15 extra destination bytes. Only the AirPlay TX direct path calls
       * this entry, after its object-local malloc hook supplied that padding.
       * Partial decrypt remains unsupported and never uses this bypass.
       */
      /*
       * This direct function-table call intentionally bypasses the public
       * SDK alignment check and therefore also bypasses the linker's normal
       * crypto HAL wrapper.  Mark the exact ROM interval explicitly so the
       * 10-second report attributes this operation to CHACHA_COMBINED rather
       * than producing a false outside_us/SLOW payload=0 transaction.
       */
      profile_start_us =
        crypto_engine_profiler_chacha_combined_begin(
          (uint32_t)plaintext_len);
      result = hal_crypto_stubs_s.rtl_crypto_chacha_poly1305_encrypt(
        (const uint8_t *)plaintext, (uint32_t)plaintext_len,
        g_chacha_hw_nonce, g_chacha_hw_aad, (uint32_t)aad_len,
        (uint8_t *)ciphertext, g_chacha_hw_tag
      );
      crypto_engine_profiler_chacha_combined_end(
        profile_start_us, (uint32_t)plaintext_len, (uint32_t)aad_len, result);
      if (result == 0) {
        memcpy(tag, g_chacha_hw_tag, 16u);
        result = CHACHA_RTL_OK;
      } else {
        printf("[CHACHA][HW] partial combined encrypt failed: err=%d\n",
               result);
        (void)chacha_rtl_recover_locked(1);
        result = CHACHA_RTL_ERROR_OPERATION;
      }
    } else {
      printf("[CHACHA][HW] partial combined key init failed: err=%d\n",
             result);
      (void)chacha_rtl_recover_locked(0);
      result = CHACHA_RTL_ERROR_OPERATION;
    }
  }
  chacha_rtl_clear_key_material();
  carbox_crypto_chacha_device_unlock(RT_DEV_LOCK_CRYPTO);
  return result;
}

int chacha_rtl8195b_decrypt(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *ciphertext, size_t ciphertext_len,
  void *plaintext, uint8_t calculated_tag[16]
) {
  return chacha_rtl_run(
    1, key, nonce, aad, aad_len,
    ciphertext, ciphertext_len, plaintext, calculated_tag
  );
}

int chacha_rtl8195b_transaction_begin(void)
{
  if (rtw_in_interrupt()) return CHACHA_RTL_SKIP_INTERRUPT;
  carbox_crypto_chacha_device_lock(RT_DEV_LOCK_CRYPTO);
  return CHACHA_RTL_OK;
}

void chacha_rtl8195b_transaction_end(void)
{
  carbox_crypto_chacha_device_unlock(RT_DEV_LOCK_CRYPTO);
}

int chacha_rtl8195b_chacha_xor_locked(
  const uint8_t key[32], const uint8_t nonce[8], uint32_t counter,
  const void *input, size_t input_len, void *output
) {
  int result = chacha_rtl_validate_raw(input_len, 1);

  if (result != CHACHA_RTL_OK) return result;

  result = chacha_rtl_prepare_locked();
  if (result == CHACHA_RTL_OK) {
    chacha_rtl_stage_parameters(key, nonce, NULL, 0u);
    result = rtl_crypto_chacha_init(g_chacha_hw_key);
    if (result == 0) {
      result = rtl_crypto_chacha_encrypt(
        (const uint8_t *)input, (uint32_t)input_len,
        g_chacha_hw_nonce, counter, (uint8_t *)output
      );
      if (result == 0) {
        result = CHACHA_RTL_OK;
      } else {
        printf("[CHACHA][HW] standalone ChaCha failed: err=%d\n", result);
        (void)chacha_rtl_recover_locked(1);
        result = CHACHA_RTL_ERROR_OPERATION;
      }
    } else {
      printf("[CHACHA][HW] standalone ChaCha key init failed: err=%d\n",
             result);
      (void)chacha_rtl_recover_locked(0);
      result = CHACHA_RTL_ERROR_OPERATION;
    }
  }
  chacha_rtl_clear_key_material();
  return result;
}

int chacha_rtl8195b_chacha_xor(
  const uint8_t key[32], const uint8_t nonce[8], uint32_t counter,
  const void *input, size_t input_len, void *output
) {
  int result = chacha_rtl8195b_transaction_begin();

  if (result == CHACHA_RTL_OK) {
    result = chacha_rtl8195b_chacha_xor_locked(
      key, nonce, counter, input, input_len, output
    );
    chacha_rtl8195b_transaction_end();
  }
  return result;
}

int chacha_rtl8195b_poly1305_locked(
  const uint8_t poly_key[32],
  const void *message, size_t message_len,
  uint8_t digest[16]
) {
  int result = chacha_rtl_validate_raw(message_len, 0);

  if (result != CHACHA_RTL_OK) return result;

  result = chacha_rtl_prepare_locked();
  if (result == CHACHA_RTL_OK) {
    memcpy(g_chacha_hw_key, poly_key, 32);
    result = rtl_crypto_poly1305(
      (const uint8_t *)message, (uint32_t)message_len,
      g_chacha_hw_key, g_chacha_hw_tag
    );
    if (result == 0) {
      memcpy(digest, g_chacha_hw_tag, 16);
      result = CHACHA_RTL_OK;
    } else {
      printf("[CHACHA][HW] standalone Poly1305 failed: err=%d\n", result);
      (void)chacha_rtl_recover_locked(1);
      result = CHACHA_RTL_ERROR_OPERATION;
    }
  }
  chacha_rtl_clear_key_material();
  return result;
}

int chacha_rtl8195b_poly1305(
  const uint8_t poly_key[32],
  const void *message, size_t message_len,
  uint8_t digest[16]
) {
  int result = chacha_rtl8195b_transaction_begin();

  if (result == CHACHA_RTL_OK) {
    result = chacha_rtl8195b_poly1305_locked(
      poly_key, message, message_len, digest
    );
    chacha_rtl8195b_transaction_end();
  }
  return result;
}

#else

int chacha_rtl8195b_encrypt(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *plaintext, size_t plaintext_len,
  void *ciphertext, uint8_t tag[16]
) {
  (void)key; (void)nonce; (void)aad; (void)aad_len;
  (void)plaintext; (void)plaintext_len; (void)ciphertext; (void)tag;
  return CHACHA_RTL_SKIP_DISABLED;
}

int chacha_rtl8195b_transaction_begin(void) {
  return CHACHA_RTL_SKIP_DISABLED;
}

void chacha_rtl8195b_transaction_end(void) {
}

int chacha_rtl8195b_chacha_xor_locked(
  const uint8_t key[32], const uint8_t nonce[8], uint32_t counter,
  const void *input, size_t input_len, void *output
) {
  (void)key; (void)nonce; (void)counter;
  (void)input; (void)input_len; (void)output;
  return CHACHA_RTL_SKIP_DISABLED;
}

int chacha_rtl8195b_encrypt_partial_padded(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *plaintext, size_t plaintext_len,
  void *ciphertext, uint8_t tag[16]
) {
  (void)key; (void)nonce; (void)aad; (void)aad_len;
  (void)plaintext; (void)plaintext_len; (void)ciphertext; (void)tag;
  return CHACHA_RTL_SKIP_DISABLED;
}

int chacha_rtl8195b_decrypt(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *ciphertext, size_t ciphertext_len,
  void *plaintext, uint8_t calculated_tag[16]
) {
  (void)key; (void)nonce; (void)aad; (void)aad_len;
  (void)ciphertext; (void)ciphertext_len; (void)plaintext;
  (void)calculated_tag;
  return CHACHA_RTL_SKIP_DISABLED;
}

int chacha_rtl8195b_chacha_xor(
  const uint8_t key[32], const uint8_t nonce[8], uint32_t counter,
  const void *input, size_t input_len, void *output
) {
  (void)key; (void)nonce; (void)counter;
  (void)input; (void)input_len; (void)output;
  return CHACHA_RTL_SKIP_DISABLED;
}

int chacha_rtl8195b_poly1305(
  const uint8_t poly_key[32],
  const void *message, size_t message_len,
  uint8_t digest[16]
) {
  (void)poly_key; (void)message; (void)message_len; (void)digest;
  return CHACHA_RTL_SKIP_DISABLED;
}

int chacha_rtl8195b_poly1305_locked(
  const uint8_t poly_key[32],
  const void *message, size_t message_len,
  uint8_t digest[16]
) {
  (void)poly_key; (void)message; (void)message_len; (void)digest;
  return CHACHA_RTL_SKIP_DISABLED;
}

int chacha_rtl8195b_precheck_context(void) {
  return CHACHA_RTL_OK;
}

#endif

const char *chacha_rtl8195b_status_string(int status) {
  switch (status) {
    case CHACHA_RTL_OK: return "ok";
    case CHACHA_RTL_SKIP_DISABLED: return "disabled";
    case CHACHA_RTL_SKIP_INTERRUPT: return "interrupt-context";
    case CHACHA_RTL_SKIP_LENGTH: return "unsupported-length";
    case CHACHA_RTL_SKIP_AAD_LENGTH: return "unsupported-aad-length";
    case CHACHA_RTL_SKIP_LAYOUT: return "unsupported-buffer-layout";
    case CHACHA_RTL_SKIP_MEMORY: return "scratch-allocation-failed";
    case CHACHA_RTL_SKIP_THRESHOLD: return "below-hardware-threshold";
    case CHACHA_RTL_SKIP_POLY_LENGTH: return "poly1305-input-too-large";
    case CHACHA_RTL_ERROR_INIT: return "hardware-init-failed";
    case CHACHA_RTL_ERROR_OPERATION: return "hardware-operation-failed";
    default: return "unknown";
  }
}
