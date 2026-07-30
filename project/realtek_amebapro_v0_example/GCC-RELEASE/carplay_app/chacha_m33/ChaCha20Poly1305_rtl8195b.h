#ifndef CHACHA20_POLY1305_RTL8195B_H
#define CHACHA20_POLY1305_RTL8195B_H

#include <stddef.h>
#include <stdint.h>

enum {
  CHACHA_RTL_OK = 0,
  CHACHA_RTL_SKIP_DISABLED = 1,
  CHACHA_RTL_SKIP_INTERRUPT = 2,
  CHACHA_RTL_SKIP_LENGTH = 3,
  CHACHA_RTL_SKIP_AAD_LENGTH = 4,
  CHACHA_RTL_SKIP_LAYOUT = 5,
  CHACHA_RTL_SKIP_MEMORY = 6,
  CHACHA_RTL_SKIP_THRESHOLD = 7,
  CHACHA_RTL_SKIP_POLY_LENGTH = 8,
  CHACHA_RTL_ERROR_INIT = -1,
  CHACHA_RTL_ERROR_OPERATION = -2
};

int chacha_rtl8195b_encrypt(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *plaintext, size_t plaintext_len,
  void *ciphertext, uint8_t tag[16]
);

int chacha_rtl8195b_decrypt(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *ciphertext, size_t ciphertext_len,
  void *plaintext, uint8_t calculated_tag[16]
);

int chacha_rtl8195b_chacha_xor(
  const uint8_t key[32], const uint8_t nonce[8], uint32_t counter,
  const void *input, size_t input_len, void *output
);

int chacha_rtl8195b_poly1305(
  const uint8_t poly_key[32],
  const void *message, size_t message_len,
  uint8_t digest[16]
);

/* Returns CHACHA_RTL_SKIP_INTERRUPT when task-blocking HW use is unsafe. */
int chacha_rtl8195b_precheck_context(void);

const char *chacha_rtl8195b_status_string(int status);

#endif
