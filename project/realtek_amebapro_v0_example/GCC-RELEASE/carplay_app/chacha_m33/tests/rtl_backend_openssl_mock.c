#include "../ChaCha20Poly1305_rtl8195b.h"

#include <openssl/evp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static unsigned int g_mock_decrypt_successes;
static unsigned int g_mock_decrypt_failures;
static unsigned int g_mock_combined_encrypts;
static unsigned int g_mock_chacha_operations;
static unsigned int g_mock_poly1305_operations;
static unsigned int g_mock_fail_chacha_call;
static unsigned int g_mock_fail_poly1305_call;
static unsigned int g_mock_fail_aad_snapshot;
static unsigned int g_mock_interrupt_context;

void mock_rtl_reset_stats(void) {
  g_mock_decrypt_successes = 0;
  g_mock_decrypt_failures = 0;
  g_mock_combined_encrypts = 0;
  g_mock_chacha_operations = 0;
  g_mock_poly1305_operations = 0;
  g_mock_fail_chacha_call = 0;
  g_mock_fail_poly1305_call = 0;
  g_mock_fail_aad_snapshot = 0;
  g_mock_interrupt_context = 0;
}

unsigned int mock_rtl_decrypt_successes(void) {
  return g_mock_decrypt_successes;
}

unsigned int mock_rtl_decrypt_failures(void) {
  return g_mock_decrypt_failures;
}

unsigned int mock_rtl_combined_encrypts(void) {
  return g_mock_combined_encrypts;
}

unsigned int mock_rtl_chacha_operations(void) {
  return g_mock_chacha_operations;
}

unsigned int mock_rtl_poly1305_operations(void) {
  return g_mock_poly1305_operations;
}

void mock_rtl_fail_chacha_on(unsigned int call_index) {
  g_mock_fail_chacha_call = call_index;
}

void mock_rtl_fail_poly1305_on(unsigned int call_index) {
  g_mock_fail_poly1305_call = call_index;
}

void mock_rtl_fail_aad_snapshot_once(void) {
  g_mock_fail_aad_snapshot = 1u;
}

void mock_rtl_set_interrupt_context(unsigned int enabled) {
  g_mock_interrupt_context = enabled;
}

#if CARBOX_CHACHA_MODE != CARBOX_CHACHA_MODE_SOFTWARE_ONLY
void *carbox_chacha_aad_realloc(void *ptr, size_t len) {
  if (g_mock_fail_aad_snapshot) {
    g_mock_fail_aad_snapshot = 0u;
    return NULL;
  }
  return realloc(ptr, len);
}
#endif

static void make_nonce(uint8_t nonce12[12], const uint8_t nonce8[8]) {
  memset(nonce12, 0, 12);
  memcpy(nonce12 + 4, nonce8, 8);
}

static int mock_encrypt(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *plaintext, size_t plaintext_len,
  void *ciphertext, uint8_t tag[16]
) {
  EVP_CIPHER_CTX *ctx;
  uint8_t nonce12[12];
  int out_len;
  int tail_len;
  int ok = 0;

  make_nonce(nonce12, nonce);
  ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return CHACHA_RTL_ERROR_INIT;
  if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) != 1)
    goto exit;
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) != 1)
    goto exit;
  if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce12) != 1) goto exit;
  if (aad_len != 0u) {
    if (EVP_EncryptUpdate(
          ctx, NULL, &out_len, (const uint8_t *)aad, (int)aad_len
        ) != 1) goto exit;
  }
  if (EVP_EncryptUpdate(
        ctx, (uint8_t *)ciphertext, &out_len,
        (const uint8_t *)plaintext, (int)plaintext_len
      ) != 1) goto exit;
  if (EVP_EncryptFinal_ex(
        ctx, (uint8_t *)ciphertext + out_len, &tail_len
      ) != 1) goto exit;
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag) != 1)
    goto exit;
  ok = 1;
exit:
  EVP_CIPHER_CTX_free(ctx);
  return ok ? CHACHA_RTL_OK : CHACHA_RTL_ERROR_OPERATION;
}

int chacha_rtl8195b_encrypt(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *plaintext, size_t plaintext_len,
  void *ciphertext, uint8_t tag[16]
) {
  if ((plaintext_len == 0u) || (plaintext_len > 65536u) ||
      ((plaintext_len & 15u) != 0u)) return CHACHA_RTL_SKIP_LENGTH;
  if (aad_len > 496u) return CHACHA_RTL_SKIP_AAD_LENGTH;
  ++g_mock_combined_encrypts;
  return mock_encrypt(
    key, nonce, aad, aad_len, plaintext, plaintext_len, ciphertext, tag
  );
}

int chacha_rtl8195b_decrypt(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *ciphertext, size_t ciphertext_len,
  void *plaintext, uint8_t calculated_tag[16]
) {
  EVP_CIPHER_CTX *ctx;
  uint8_t iv[16];
  uint8_t *check_ciphertext;
  uint8_t check_tag[16];
  int out_len;
  int status;

  if ((ciphertext_len == 0u) || (ciphertext_len > 65536u) ||
      ((ciphertext_len & 15u) != 0u)) return CHACHA_RTL_SKIP_LENGTH;
  if (aad_len > 496u) return CHACHA_RTL_SKIP_AAD_LENGTH;

  memset(iv, 0, sizeof(iv));
  iv[0] = 1u;
  memcpy(iv + 8, nonce, 8);
  ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return CHACHA_RTL_ERROR_INIT;
  if (EVP_DecryptInit_ex(ctx, EVP_chacha20(), NULL, key, iv) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return CHACHA_RTL_ERROR_OPERATION;
  }
  if (EVP_DecryptUpdate(
        ctx, (uint8_t *)plaintext, &out_len,
        (const uint8_t *)ciphertext, (int)ciphertext_len
      ) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return CHACHA_RTL_ERROR_OPERATION;
  }
  EVP_CIPHER_CTX_free(ctx);

  check_ciphertext = (uint8_t *)malloc(ciphertext_len);
  if (!check_ciphertext) return CHACHA_RTL_SKIP_MEMORY;
  status = mock_encrypt(
    key, nonce, aad, aad_len, plaintext, ciphertext_len,
    check_ciphertext, check_tag
  );
  if ((status == CHACHA_RTL_OK) &&
      (memcmp(check_ciphertext, ciphertext, ciphertext_len) == 0)) {
    memcpy(calculated_tag, check_tag, 16);
    ++g_mock_decrypt_successes;
  } else if (status == CHACHA_RTL_OK) {
    ++g_mock_decrypt_failures;
    status = CHACHA_RTL_ERROR_OPERATION;
  }
  free(check_ciphertext);
  return status;
}

int chacha_rtl8195b_chacha_xor(
  const uint8_t key[32], const uint8_t nonce[8], uint32_t counter,
  const void *input, size_t input_len, void *output
) {
  EVP_CIPHER_CTX *ctx;
  uint8_t iv[16] = {0};
  int out_len;
  int ok;

  if ((input_len == 0u) || (input_len > 65536u) ||
      ((input_len & 15u) != 0u)) return CHACHA_RTL_SKIP_LENGTH;
  ++g_mock_chacha_operations;
  if (g_mock_fail_chacha_call == g_mock_chacha_operations) {
    return CHACHA_RTL_ERROR_OPERATION;
  }
  iv[0] = (uint8_t)counter;
  iv[1] = (uint8_t)(counter >> 8);
  iv[2] = (uint8_t)(counter >> 16);
  iv[3] = (uint8_t)(counter >> 24);
  memcpy(iv + 8, nonce, 8);

  ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return CHACHA_RTL_ERROR_INIT;
  ok = EVP_EncryptInit_ex(ctx, EVP_chacha20(), NULL, key, iv) == 1 &&
       EVP_EncryptUpdate(
         ctx, (uint8_t *)output, &out_len,
         (const uint8_t *)input, (int)input_len
       ) == 1 &&
       out_len == (int)input_len;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) return CHACHA_RTL_ERROR_OPERATION;
  return CHACHA_RTL_OK;
}

int chacha_rtl8195b_poly1305(
  const uint8_t poly_key[32],
  const void *message, size_t message_len,
  uint8_t digest[16]
) {
  EVP_PKEY *pkey;
  EVP_MD_CTX *ctx;
  size_t digest_len = 16u;
  int ok;

  if ((message_len == 0u) || (message_len > 65536u)) {
    return CHACHA_RTL_SKIP_LENGTH;
  }
  ++g_mock_poly1305_operations;
  if (g_mock_fail_poly1305_call == g_mock_poly1305_operations) {
    return CHACHA_RTL_ERROR_OPERATION;
  }
  pkey = EVP_PKEY_new_raw_private_key(
    EVP_PKEY_POLY1305, NULL, poly_key, 32u
  );
  ctx = EVP_MD_CTX_new();
  if (!pkey || !ctx) {
    if (ctx) EVP_MD_CTX_free(ctx);
    if (pkey) EVP_PKEY_free(pkey);
    return CHACHA_RTL_ERROR_INIT;
  }
  ok = EVP_DigestSignInit(ctx, NULL, NULL, NULL, pkey) == 1 &&
       EVP_DigestSignUpdate(ctx, message, message_len) == 1 &&
       EVP_DigestSignFinal(ctx, digest, &digest_len) == 1 &&
       digest_len == 16u;
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  if (!ok) return CHACHA_RTL_ERROR_OPERATION;
  return CHACHA_RTL_OK;
}

int chacha_rtl8195b_precheck_context(void) {
  return g_mock_interrupt_context ?
         CHACHA_RTL_SKIP_INTERRUPT : CHACHA_RTL_OK;
}

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
