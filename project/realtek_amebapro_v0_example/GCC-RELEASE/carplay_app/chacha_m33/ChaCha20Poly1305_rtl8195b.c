#include "ChaCha20Poly1305.h"
#include "ChaCha20Poly1305_rtl8195b.h"

#if CARBOX_CHACHA_MODE != CARBOX_CHACHA_MODE_SOFTWARE_ONLY

#include <stdio.h>
#include <string.h>

#include "basic_types.h"
#include "crypto_api.h"
#include "hal_crypto.h"
#include "rtl8195bhp_crypto_ctrl.h"
#include "device_lock.h"
#include "osdep_service.h"

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

  device_mutex_lock(RT_DEV_LOCK_CRYPTO);
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
  device_mutex_unlock(RT_DEV_LOCK_CRYPTO);
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

  device_mutex_lock(RT_DEV_LOCK_CRYPTO);
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
  device_mutex_unlock(RT_DEV_LOCK_CRYPTO);
  return result;
}

int chacha_rtl8195b_poly1305(
  const uint8_t poly_key[32],
  const void *message, size_t message_len,
  uint8_t digest[16]
) {
  int result = chacha_rtl_validate_raw(message_len, 0);

  if (result != CHACHA_RTL_OK) return result;

  device_mutex_lock(RT_DEV_LOCK_CRYPTO);
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
  device_mutex_unlock(RT_DEV_LOCK_CRYPTO);
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
