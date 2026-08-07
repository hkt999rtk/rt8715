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

#ifndef CARBOX_CHACHA_HW_IRQ_TIMEOUT_MS
#define CARBOX_CHACHA_HW_IRQ_TIMEOUT_MS 1000u
#endif

#define CHACHA_RTL_MAX_MESSAGE 65536u
#define CHACHA_RTL_MAX_AAD       496u

extern hal_crypto_adapter_t g_rtl_cryptoEngine_s;
extern void g_crypto_handler(int crypto_done, int crc_done);
extern int g_crypto_pre_exec(void *adapter);
extern int g_crypto_wait_done(void *adapter);
extern void rtl_crypto_irq_enable(
  hal_crypto_adapter_t *adapter, void (*handler)(int, int)
);
extern int rtw_in_interrupt(void);

static _sema g_chacha_completion_sema;
static int g_chacha_completion_ready;
static int g_chacha_hw_state;
static uint8_t g_chacha_hw_key[32] __attribute__((aligned(32)));
static uint8_t g_chacha_hw_nonce[32] __attribute__((aligned(32)));
static uint8_t g_chacha_hw_aad[CHACHA_RTL_MAX_AAD] __attribute__((aligned(32)));
static uint8_t g_chacha_hw_tag[32] __attribute__((aligned(32)));

#if CARBOX_CHACHA_HW_SELFTEST
static int g_chacha_hw_selftest_started;
static int chacha_rtl_selftest_locked(void);
#endif

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

static int chacha_rtl_irq_pre_exec(void *adapter) {
  g_crypto_pre_exec(adapter);
  while (rtw_down_timeout_sema(&g_chacha_completion_sema, 0) == _TRUE) {}
  return 0;
}

static int chacha_rtl_irq_wait_done(void *adapter) {
  hal_crypto_adapter_t *rtl_adapter = (hal_crypto_adapter_t *)adapter;
  int result;

  if (!rtl_adapter->isIntMode) return 0;
  if (rtw_down_timeout_sema(
        &g_chacha_completion_sema, CARBOX_CHACHA_HW_IRQ_TIMEOUT_MS
      ) == _TRUE) {
    return 0;
  }

  printf(
    "[CHACHA][HW] IRQ timeout after %lu ms; checking DMA completion\n",
    (unsigned long)CARBOX_CHACHA_HW_IRQ_TIMEOUT_MS
  );
  result = g_crypto_wait_done(adapter);
  return result;
}

static void chacha_rtl_irq_handler(int crypto_done, int crc_done) {
  g_crypto_handler(crypto_done, crc_done);
  if (crypto_done > 0) {
    rtw_up_sema_from_isr(&g_chacha_completion_sema);
  }
}

/*
 * RT_DEV_LOCK_CRYPTO must be held. AES and ChaCha may install different
 * semaphore instances, but both callback sets are generic for every RTL crypto
 * operation. Reinstall this set before each ChaCha transaction.
 */
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

  if (!g_chacha_completion_ready) {
    rtw_init_sema(&g_chacha_completion_sema, 0);
    if (!g_chacha_completion_sema) {
      g_chacha_hw_state = -1;
      printf("[CHACHA][HW] completion semaphore init failed\n");
      return CHACHA_RTL_ERROR_INIT;
    }
    g_chacha_completion_ready = 1;
  }

  if ((g_rtl_cryptoEngine_s.pre_exec_func != chacha_rtl_irq_pre_exec) ||
      (g_rtl_cryptoEngine_s.wait_done_func != chacha_rtl_irq_wait_done) ||
      !g_rtl_cryptoEngine_s.isIntMode) {
    g_rtl_cryptoEngine_s.pre_exec_func = chacha_rtl_irq_pre_exec;
    g_rtl_cryptoEngine_s.wait_done_func = chacha_rtl_irq_wait_done;
    rtl_crypto_irq_enable(&g_rtl_cryptoEngine_s, chacha_rtl_irq_handler);
  }

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
      "[CHACHA][HW] RTL8195B IRQ + semaphore backend initialized "
      "(not board-validated, timeout=%lu ms)\n",
      (unsigned long)CARBOX_CHACHA_HW_IRQ_TIMEOUT_MS
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
    g_rtl_cryptoEngine_s.pre_exec_func = chacha_rtl_irq_pre_exec;
    g_rtl_cryptoEngine_s.wait_done_func = chacha_rtl_irq_wait_done;
    rtl_crypto_irq_enable(&g_rtl_cryptoEngine_s, chacha_rtl_irq_handler);
    printf("[CHACHA][HW] engine recovered after operation failure\n");
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

int chacha_rtl8195b_chacha_xor(
  const uint8_t key[32], const uint8_t nonce[8], uint32_t counter,
  const void *input, size_t input_len, void *output
) {
  int result = chacha_rtl_validate_raw(input_len, 1);

  if (result != CHACHA_RTL_OK) return result;

  carbox_crypto_chacha_device_lock(RT_DEV_LOCK_CRYPTO);
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
  carbox_crypto_chacha_device_unlock(RT_DEV_LOCK_CRYPTO);
  return result;
}

int chacha_rtl8195b_poly1305(
  const uint8_t poly_key[32],
  const void *message, size_t message_len,
  uint8_t digest[16]
) {
  int result = chacha_rtl_validate_raw(message_len, 0);

  if (result != CHACHA_RTL_OK) return result;

  carbox_crypto_chacha_device_lock(RT_DEV_LOCK_CRYPTO);
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
  carbox_crypto_chacha_device_unlock(RT_DEV_LOCK_CRYPTO);
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
