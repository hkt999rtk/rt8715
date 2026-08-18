#include "../ChaCha20Poly1305.h"
#include "../ChaCha20Poly1305_rtl8195b.h"

#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mock_rtl_reset_stats(void);
unsigned int mock_rtl_decrypt_successes(void);
unsigned int mock_rtl_decrypt_failures(void);
unsigned int mock_rtl_combined_inplace_decrypts(void);
unsigned int mock_rtl_combined_encrypts(void);
unsigned int mock_rtl_combined_inplace_encrypts(void);
unsigned int mock_rtl_chacha_operations(void);
unsigned int mock_rtl_chacha_inplace_operations(void);
unsigned int mock_rtl_poly1305_operations(void);
void mock_rtl_fail_chacha_on(unsigned int call_index);
void mock_rtl_fail_poly1305_on(unsigned int call_index);
void mock_rtl_fail_aad_snapshot_once(void);
void mock_rtl_set_interrupt_context(unsigned int enabled);

#if CARBOX_CHACHA_MODE != CARBOX_CHACHA_MODE_SOFTWARE_ONLY
static size_t round_up_16(size_t value) {
  return (value + 15u) & ~(size_t)15u;
}

static unsigned int expected_chacha_data_ops(size_t len) {
  unsigned int operations = 0u;

  while (len != 0u) {
    const size_t chunk_len = (len > 65536u) ? 65536u : len;
    const size_t prefix_len = chunk_len & ~(size_t)63u;
    const size_t tail_len = chunk_len - prefix_len;

    if ((chunk_len & 15u) == 0u) {
      ++operations;
    } else {
      if (prefix_len != 0u) ++operations;
      if (tail_len != 0u) ++operations;
    }
    len -= chunk_len;
  }
  return operations;
}
#endif

static int reference_encrypt(
  const uint8_t key[32], const uint8_t nonce8[8],
  const uint8_t *aad, size_t aad_len,
  const uint8_t *plaintext, size_t len,
  uint8_t *ciphertext, uint8_t tag[16]
) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  uint8_t nonce12[12] = {0};
  int out_len;
  int tail_len;
  int ok = 0;
  memcpy(nonce12 + 4, nonce8, 8);
  if (!ctx) return 0;
  if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) != 1)
    goto exit;
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) != 1)
    goto exit;
  if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce12) != 1) goto exit;
  if (aad_len && EVP_EncryptUpdate(
        ctx, NULL, &out_len, aad, (int)aad_len
      ) != 1) goto exit;
  if (EVP_EncryptUpdate(
        ctx, ciphertext, &out_len, plaintext, (int)len
      ) != 1) goto exit;
  if (EVP_EncryptFinal_ex(ctx, ciphertext + out_len, &tail_len) != 1)
    goto exit;
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag) != 1)
    goto exit;
  ok = 1;
exit:
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

static int run_case(size_t len, size_t aad_len) {
  static const size_t chunks[] = {1, 7, 31, 64, 3, 129};
  chacha20_poly1305_state state;
  uint8_t key[32];
  uint8_t nonce[8];
  uint8_t aad[512];
  uint8_t tag[16], reference_tag[16];
  uint8_t *plain = malloc(len ? len : 1);
  uint8_t *cipher = malloc(len ? len : 1);
  uint8_t *reference = malloc(len ? len : 1);
  uint8_t *decoded = malloc(len ? len : 1);
  uint8_t *inplace = malloc(len ? len : 1);
  size_t in_offset;
  size_t out_offset;
  size_t chunk_index;
  int32_t error;
  size_t i;

  if (!plain || !cipher || !reference || !decoded || !inplace) return 0;
  for (i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)(0x31u + i * 7u);
  for (i = 0; i < sizeof(nonce); ++i) nonce[i] = (uint8_t)(0x91u + i * 3u);
  for (i = 0; i < sizeof(aad); ++i) aad[i] = (uint8_t)(i * 11u + 5u);
  for (i = 0; i < len; ++i) plain[i] = (uint8_t)(i * 13u + len);

  if (!reference_encrypt(
        key, nonce, aad, aad_len, plain, len, reference, reference_tag
      )) return 0;

#if CARBOX_CHACHA_MODE != CARBOX_CHACHA_MODE_SOFTWARE_ONLY
  mock_rtl_reset_stats();
#endif
  chacha20_poly1305_init_64x64(&state, key, nonce);
  chacha20_poly1305_add_aad(&state, aad, aad_len);
  in_offset = 0;
  out_offset = 0;
  chunk_index = 0;
  while (in_offset < len) {
    size_t n = chunks[chunk_index++ % (sizeof(chunks) / sizeof(chunks[0]))];
    if (n > len - in_offset) n = len - in_offset;
    out_offset += chacha20_poly1305_encrypt(
      &state, plain + in_offset, n, cipher + out_offset
    );
    in_offset += n;
  }
  out_offset += chacha20_poly1305_final(
    &state, cipher + out_offset, tag
  );
  if ((out_offset != len) || memcmp(cipher, reference, len) ||
      memcmp(tag, reference_tag, 16)) {
    fprintf(stderr, "encrypt mismatch: mode=%d len=%zu aad=%zu\n",
            CARBOX_CHACHA_MODE, len, aad_len);
    return 0;
  }
#if CARBOX_CHACHA_MODE != CARBOX_CHACHA_MODE_SOFTWARE_ONLY
  if (len >= CARBOX_CHACHA_HW_MIN_LEN) {
    const int combined_backend =
      (len <= 65536u) && ((len & 15u) == 0u) && (aad_len <= 496u);
    if (combined_backend) {
      if (mock_rtl_combined_inplace_encrypts() != 1u) {
        fprintf(
          stderr,
          "combined hardware encrypt was not in-place: "
          "mode=%d len=%zu aad=%zu\n",
          CARBOX_CHACHA_MODE, len, aad_len
        );
        return 0;
      }
    } else if (mock_rtl_chacha_inplace_operations() == 0u) {
      fprintf(
        stderr,
        "raw hardware encrypt was not in-place: mode=%d len=%zu aad=%zu\n",
        CARBOX_CHACHA_MODE, len, aad_len
      );
      return 0;
    }
  }
#endif

  chacha20_poly1305_init_64x64(&state, key, nonce);
  chacha20_poly1305_add_aad(&state, aad, aad_len);
  in_offset = 0;
  out_offset = 0;
  chunk_index = 0;
  while (in_offset < len) {
    size_t n = chunks[chunk_index++ % (sizeof(chunks) / sizeof(chunks[0]))];
    if (n > len - in_offset) n = len - in_offset;
    out_offset += chacha20_poly1305_decrypt(
      &state, cipher + in_offset, n, decoded + out_offset
    );
    in_offset += n;
  }
  out_offset += chacha20_poly1305_verify(
    &state, decoded + out_offset, tag, &error
  );
  if (error || (out_offset != len) || memcmp(decoded, plain, len)) {
    fprintf(stderr, "decrypt mismatch: mode=%d len=%zu aad=%zu err=%d\n",
            CARBOX_CHACHA_MODE, len, aad_len, (int)error);
    return 0;
  }

  memcpy(inplace, cipher, len);
  mock_rtl_reset_stats();
  chacha20_poly1305_init_64x64(&state, key, nonce);
  chacha20_poly1305_add_aad(&state, aad, aad_len);
  in_offset = 0;
  out_offset = 0;
  chunk_index = 0;
  while (in_offset < len) {
    size_t n = chunks[chunk_index++ % (sizeof(chunks) / sizeof(chunks[0]))];
    if (n > len - in_offset) n = len - in_offset;
    out_offset += chacha20_poly1305_decrypt(
      &state, inplace + in_offset, n, inplace + out_offset
    );
    in_offset += n;
  }
  out_offset += chacha20_poly1305_verify(
    &state, inplace + out_offset, tag, &error
  );
  if (error || (out_offset != len) || memcmp(inplace, plain, len)) {
    fprintf(stderr,
            "in-place decrypt mismatch: mode=%d len=%zu aad=%zu err=%d\n",
            CARBOX_CHACHA_MODE, len, aad_len, (int)error);
    return 0;
  }
#if (CARBOX_CHACHA_MODE != CARBOX_CHACHA_MODE_SOFTWARE_ONLY) && \
    (CARBOX_CHACHA_MODE != CARBOX_CHACHA_MODE_SPLIT_RX_SW_TX_HW)
  {
    unsigned int combined = mock_rtl_decrypt_successes();
    unsigned int failures = mock_rtl_decrypt_failures();
    unsigned int chacha_ops = mock_rtl_chacha_operations();
    unsigned int poly_ops = mock_rtl_poly1305_operations();

    if (len < CARBOX_CHACHA_HW_MIN_LEN) {
      if (combined || failures || chacha_ops || poly_ops) {
        fprintf(stderr,
                "hardware used below threshold: mode=%d len=%zu aad=%zu\n",
                CARBOX_CHACHA_MODE, len, aad_len);
        return 0;
      }
    } else if ((len <= 65536u) && ((len & 15u) == 0u) &&
               (aad_len <= 496u)) {
      if ((combined != 1u) || failures || chacha_ops || poly_ops) {
        fprintf(stderr,
                "combined path coverage failed: mode=%d len=%zu aad=%zu "
                "combined=%u fail=%u chacha=%u poly=%u\n",
                CARBOX_CHACHA_MODE, len, aad_len, combined, failures,
                chacha_ops, poly_ops);
        return 0;
      }
#if CARBOX_CHACHA_NONALIGNED_SW_POLY
    } else if ((len <= 65536u) && ((len & 15u) != 0u)) {
      const unsigned int expected_chacha_ops =
        expected_chacha_data_ops(len);
      if (combined || failures || (chacha_ops != expected_chacha_ops) ||
          poly_ops) {
        fprintf(stderr,
                "standalone SW Poly path coverage failed: "
                "mode=%d len=%zu aad=%zu combined=%u fail=%u "
                "chacha=%u/%u poly=%u\n",
                CARBOX_CHACHA_MODE, len, aad_len, combined, failures,
                chacha_ops, expected_chacha_ops, poly_ops);
        return 0;
      }
#endif
    } else if ((len <= 65536u) &&
               (round_up_16(aad_len) + round_up_16(len) + 16u <=
                65536u)) {
      const unsigned int expected_chacha_ops =
        1u + expected_chacha_data_ops(len);
      if (combined || failures || (chacha_ops != expected_chacha_ops) ||
          (poly_ops != 1u)) {
        fprintf(stderr,
                "standalone path coverage failed: mode=%d len=%zu aad=%zu "
                "combined=%u fail=%u chacha=%u/%u poly=%u\n",
                CARBOX_CHACHA_MODE, len, aad_len, combined, failures,
                chacha_ops, expected_chacha_ops, poly_ops);
        return 0;
      }
    } else {
      const unsigned int expected_chacha_ops =
        expected_chacha_data_ops(len);
      if (combined || failures || (chacha_ops != expected_chacha_ops) ||
          poly_ops) {
        fprintf(stderr,
                "chunked path coverage failed: mode=%d len=%zu aad=%zu "
                "combined=%u fail=%u chacha=%u/%u poly=%u\n",
                CARBOX_CHACHA_MODE, len, aad_len, combined, failures,
                chacha_ops, expected_chacha_ops, poly_ops);
        return 0;
      }
    }
    if (len >= CARBOX_CHACHA_HW_MIN_LEN) {
      const int combined_backend =
        (len <= 65536u) && ((len & 15u) == 0u) && (aad_len <= 496u);
      if (combined_backend) {
        if (mock_rtl_combined_inplace_decrypts() != 1u) {
          fprintf(
            stderr,
            "combined hardware decrypt was not in-place: "
            "mode=%d len=%zu aad=%zu\n",
            CARBOX_CHACHA_MODE, len, aad_len
          );
          return 0;
        }
      } else if (mock_rtl_chacha_inplace_operations() == 0u) {
        fprintf(
          stderr,
          "raw hardware decrypt was not in-place: "
          "mode=%d len=%zu aad=%zu\n",
          CARBOX_CHACHA_MODE, len, aad_len
        );
        return 0;
      }
    }
  }
#endif

  tag[0] ^= 0x80u;
  error = chacha20_poly1305_decrypt_all_64x64(
    key, nonce, aad, aad_len, cipher, len, decoded, tag
  );
  if (error == 0) {
    fprintf(stderr, "bad tag accepted: mode=%d len=%zu aad=%zu\n",
            CARBOX_CHACHA_MODE, len, aad_len);
    return 0;
  }

  free(inplace);
  free(decoded);
  free(reference);
  free(cipher);
  free(plain);
  return 1;
}

#if CARBOX_CHACHA_MODE != CARBOX_CHACHA_MODE_SOFTWARE_ONLY
static int run_aad_snapshot_cases(void) {
  const size_t len = 4097u;
  chacha20_poly1305_state state;
  uint8_t key[32];
  uint8_t nonce[8];
  uint8_t aad_first[63];
  uint8_t aad_second[65];
  uint8_t aad_full[128];
  uint8_t expected_tag[16];
  uint8_t actual_tag[16];
  uint8_t *plain = malloc(len);
  uint8_t *expected = malloc(len);
  uint8_t *actual = malloc(len);
  size_t written;
  size_t i;
  int32_t error;

  if (!plain || !expected || !actual) return 0;
  for (i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)(i * 3u + 0x21u);
  for (i = 0; i < sizeof(nonce); ++i) nonce[i] = (uint8_t)(i * 7u + 0x81u);
  for (i = 0; i < sizeof(aad_full); ++i) aad_full[i] = (uint8_t)(i * 5u + 9u);
  memcpy(aad_first, aad_full, sizeof(aad_first));
  memcpy(aad_second, aad_full + sizeof(aad_first), sizeof(aad_second));
  for (i = 0; i < len; ++i) plain[i] = (uint8_t)(i * 11u + 1u);

  if (!reference_encrypt(
        key, nonce, aad_full, sizeof(aad_full), plain, len,
        expected, expected_tag
      )) return 0;

  /* Two non-contiguous AAD updates must be snapshotted before mutation. */
  chacha20_poly1305_init_64x64(&state, key, nonce);
  chacha20_poly1305_add_aad(&state, aad_first, sizeof(aad_first));
  chacha20_poly1305_add_aad(&state, aad_second, sizeof(aad_second));
  memset(aad_first, 0xa5, sizeof(aad_first));
  memset(aad_second, 0x5a, sizeof(aad_second));
  written = chacha20_poly1305_encrypt(&state, plain, len, actual);
  written += chacha20_poly1305_final(
    &state, actual + written, actual_tag
  );
  if ((written != len) || memcmp(actual, expected, len) ||
      memcmp(actual_tag, expected_tag, sizeof(actual_tag))) {
    fprintf(stderr, "AAD snapshot encrypt failed: mode=%d\n",
            CARBOX_CHACHA_MODE);
    return 0;
  }

  memcpy(aad_first, aad_full, sizeof(aad_first));
  memcpy(aad_second, aad_full + sizeof(aad_first), sizeof(aad_second));
  chacha20_poly1305_init_64x64(&state, key, nonce);
  chacha20_poly1305_add_aad(&state, aad_first, sizeof(aad_first));
  chacha20_poly1305_add_aad(&state, aad_second, sizeof(aad_second));
  memset(aad_first, 0x3c, sizeof(aad_first));
  memset(aad_second, 0xc3, sizeof(aad_second));
  written = chacha20_poly1305_decrypt(&state, expected, len, actual);
  written += chacha20_poly1305_verify(
    &state, actual + written, expected_tag, &error
  );
  if (error || (written != len) || memcmp(actual, plain, len)) {
    fprintf(stderr, "AAD snapshot decrypt failed: mode=%d err=%d\n",
            CARBOX_CHACHA_MODE, (int)error);
    return 0;
  }

  /* Snapshot OOM must skip HW but retain a correct software result. */
  mock_rtl_reset_stats();
  mock_rtl_fail_aad_snapshot_once();
  chacha20_poly1305_init_64x64(&state, key, nonce);
  chacha20_poly1305_add_aad(&state, aad_full, sizeof(aad_full));
  written = chacha20_poly1305_encrypt(&state, plain, len, actual);
  written += chacha20_poly1305_final(
    &state, actual + written, actual_tag
  );
  if ((written != len) || memcmp(actual, expected, len) ||
      memcmp(actual_tag, expected_tag, sizeof(actual_tag)) ||
      mock_rtl_combined_encrypts() || mock_rtl_decrypt_successes() ||
      mock_rtl_chacha_operations() ||
      mock_rtl_poly1305_operations()) {
    fprintf(stderr, "AAD snapshot OOM fallback failed: mode=%d\n",
            CARBOX_CHACHA_MODE);
    return 0;
  }

  /* Interrupt context is rejected before AAD allocation or HW setup. */
  mock_rtl_reset_stats();
  mock_rtl_set_interrupt_context(1u);
  chacha20_poly1305_init_64x64(&state, key, nonce);
  chacha20_poly1305_add_aad(&state, aad_full, sizeof(aad_full));
  mock_rtl_set_interrupt_context(0u);
  written = chacha20_poly1305_encrypt(&state, plain, len, actual);
  written += chacha20_poly1305_final(
    &state, actual + written, actual_tag
  );
  if ((written != len) || memcmp(actual, expected, len) ||
      memcmp(actual_tag, expected_tag, sizeof(actual_tag)) ||
      mock_rtl_combined_encrypts() || mock_rtl_decrypt_successes() ||
      mock_rtl_chacha_operations() ||
      mock_rtl_poly1305_operations()) {
    fprintf(stderr, "interrupt-context fallback failed: mode=%d\n",
            CARBOX_CHACHA_MODE);
    return 0;
  }

  free(actual);
  free(expected);
  free(plain);
  return 1;
}

static int run_failure_fallbacks(void) {
  const size_t large_len = 131073u;
  const size_t standalone_len = 4097u;
  uint8_t key[32];
  uint8_t nonce[8];
  uint8_t aad[128];
  uint8_t expected_tag[16];
  uint8_t actual_tag[16];
  uint8_t *plain = malloc(large_len);
  uint8_t *expected = malloc(large_len);
  uint8_t *actual = malloc(large_len);
  size_t i;
  int32_t error;
#if CARBOX_CHACHA_TX_USES_HARDWARE
  static const uint8_t zero_tag[16] = {0};
#endif

  if (!plain || !expected || !actual) return 0;
  for (i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)(0x53u + i * 5u);
  for (i = 0; i < sizeof(nonce); ++i) nonce[i] = (uint8_t)(0xa1u + i);
  for (i = 0; i < sizeof(aad); ++i) aad[i] = (uint8_t)(i * 9u + 7u);
  for (i = 0; i < large_len; ++i) plain[i] = (uint8_t)(i * 17u + 3u);

  if (!reference_encrypt(
        key, nonce, aad, sizeof(aad), plain, large_len,
        expected, expected_tag
      )) return 0;

  mock_rtl_reset_stats();
  mock_rtl_fail_chacha_on(2u);
  chacha20_poly1305_encrypt_all_64x64(
    key, nonce, aad, sizeof(aad), plain, large_len, actual, actual_tag
  );
#if CARBOX_CHACHA_TX_USES_HARDWARE
  if (memcmp(actual_tag, zero_tag, sizeof(actual_tag)) != 0) {
    fprintf(stderr, "chunked encrypt HW failure did not clear tag\n");
    return 0;
  }
#else
  if (memcmp(actual, expected, large_len) ||
      memcmp(actual_tag, expected_tag, sizeof(actual_tag))) {
    fprintf(stderr, "chunked encrypt fallback failed: mode=%d\n",
            CARBOX_CHACHA_MODE);
    return 0;
  }
#endif

  mock_rtl_reset_stats();
  mock_rtl_fail_chacha_on(2u);
  error = chacha20_poly1305_decrypt_all_64x64(
    key, nonce, aad, sizeof(aad), expected, large_len,
    actual, expected_tag
  );
#if CARBOX_CHACHA_RX_USES_HARDWARE
  if (error != CHACHA_RTL_ERROR_OPERATION) {
    fprintf(stderr,
            "chunked decrypt HW failure was not returned: err=%d\n",
            (int)error);
    return 0;
  }
#else
  if (error || memcmp(actual, plain, large_len)) {
    fprintf(stderr, "chunked decrypt fallback failed: mode=%d err=%d\n",
            CARBOX_CHACHA_MODE, (int)error);
    return 0;
  }
#endif

  if (!reference_encrypt(
        key, nonce, aad, sizeof(aad), plain, standalone_len,
        expected, expected_tag
      )) return 0;
  mock_rtl_reset_stats();
  mock_rtl_fail_poly1305_on(1u);
  chacha20_poly1305_encrypt_all_64x64(
    key, nonce, aad, sizeof(aad), plain, standalone_len,
    actual, actual_tag
  );
#if CARBOX_CHACHA_TX_USES_HARDWARE
#if CARBOX_CHACHA_NONALIGNED_SW_POLY
  if (memcmp(actual, expected, standalone_len) ||
      memcmp(actual_tag, expected_tag, sizeof(actual_tag))) {
    fprintf(stderr, "software Poly1305 policy result mismatch\n");
    return 0;
  }
#else
  if (memcmp(actual_tag, zero_tag, sizeof(actual_tag)) != 0) {
    fprintf(stderr, "Poly1305 HW failure did not clear tag\n");
    return 0;
  }
#endif
#else
  if (memcmp(actual, expected, standalone_len) ||
      memcmp(actual_tag, expected_tag, sizeof(actual_tag))) {
    fprintf(stderr, "standalone Poly1305 fallback failed: mode=%d\n",
            CARBOX_CHACHA_MODE);
    return 0;
  }
#endif

  free(actual);
  free(expected);
  free(plain);
  return 1;
}
#endif

int main(void) {
  static const size_t lengths[] = {
    0, 1, 15, 16, 17, 31, 32, 63, 64, 65, 255, 1024,
    4095, 4096, 4097, 4795, 16064,
    65007, 65008, 65009, 65391, 65392, 65393,
    65535, 65536, 65537, 131071, 131072, 131073
  };
  static const size_t aad_lengths[] = {0, 2, 16, 31, 496, 497};
  size_t i, j;
  for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
    for (j = 0; j < sizeof(aad_lengths) / sizeof(aad_lengths[0]); ++j) {
      if (!run_case(lengths[i], aad_lengths[j])) return 1;
    }
  }
#if CARBOX_CHACHA_MODE != CARBOX_CHACHA_MODE_SOFTWARE_ONLY
  if (!run_aad_snapshot_cases()) return 1;
  if (!run_failure_fallbacks()) return 1;
#endif
  printf("host ChaCha mode %d: PASS\n", CARBOX_CHACHA_MODE);
  return 0;
}
