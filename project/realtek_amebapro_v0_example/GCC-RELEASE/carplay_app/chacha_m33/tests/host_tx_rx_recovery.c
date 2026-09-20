/* Fault injection against real streaming core, independent OpenSSL vectors.
 * Direct sender hooks check that no partial record is marked ready. */
#include "ChaCha20Poly1305.h"
#include "screen_tx_direct_crypto.h"
#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mock_rtl_reset_stats(void);
void mock_rtl_fail_chacha_on(unsigned);
void mock_rtl_fail_chacha_after_write_on(unsigned);
void mock_rtl_fail_poly1305_on(unsigned);
void mock_rtl_fail_combined_after_write(unsigned);
void mock_rtl_recovery_disabled(unsigned);
void mock_rtl_corrupt_on_error(void *);
unsigned mock_rtl_transaction_active(void);
unsigned mock_rtl_chacha_operations(void);
unsigned mock_rtl_combined_encrypts(void);
unsigned mock_rtl_decrypt_successes(void);

static int fail_alloc, direct_enabled, completed;
static const unsigned char *direct_src, *expected_cipher;
static unsigned char *direct_dst, *actual_tag;
static unsigned char expected_tag[16];
static size_t direct_len;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t len) {
  if (fail_alloc) { fail_alloc = 0; return NULL; }
  return __real_malloc(len);
}
int carbox_screen_tx_crypto_begin(void *src, size_t n, void *dst,
                                 uint32_t kind, const void **input) {
  *input = src;
  if (!direct_enabled) return 0;
  assert(src == direct_dst && dst == direct_dst && n == direct_len);
  assert(kind == CARBOX_SCREEN_TX_CRYPTO_CHACHA);
  *input = direct_src;
  return 1;
}
int carbox_screen_tx_crypto_active(void *dst, size_t n, const void **input) {
  *input = dst;
  if (!direct_enabled) return 0;
  assert(dst == direct_dst && n == direct_len);
  *input = direct_src;
  return 1;
}
void carbox_screen_tx_crypto_materialized(void *dst, size_t n) {
  assert(dst == direct_dst && n == direct_len);
}
void carbox_screen_tx_crypto_complete(void *dst, size_t n, uint32_t kind, int status) {
  assert(direct_enabled && dst == direct_dst && n == direct_len);
  assert(kind == CARBOX_SCREEN_TX_CRYPTO_CHACHA && status == 0);
  assert(!mock_rtl_transaction_active());
  assert(!memcmp(dst, expected_cipher, n));
  assert(!memcmp(actual_tag, expected_tag, 16));
  ++completed;
}
static void reference(const unsigned char *key, const unsigned char *nonce,
                      const unsigned char *aad, const unsigned char *plain,
                      size_t n, unsigned char *cipher, unsigned char *tag) {
  unsigned char iv[12] = {0};
  int wrote, tail;
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  assert(c);
  memcpy(iv + 4, nonce, 8);
  assert(EVP_EncryptInit_ex(c, EVP_chacha20_poly1305(), NULL, key, iv) == 1);
  assert(EVP_EncryptUpdate(c, NULL, &wrote, aad, 128) == 1);
  assert(EVP_EncryptUpdate(c, cipher, &wrote, plain, (int)n) == 1);
  assert(EVP_EncryptFinal_ex(c, cipher + wrote, &tail) == 1);
  assert((size_t)(wrote + tail) == n);
  assert(EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, tag) == 1);
  EVP_CIPHER_CTX_free(c);
}
/* layout 0: separate, 1: in-place streaming, 2: owned direct screen TX.
 * fault 1: partial raw write; 2: Poly failure after ciphertext generation;
 * 3: combined failure incl. padded tail; 4: engine reinit fails; 5: no memory. */
static void run(unsigned rx, size_t n, unsigned layout, unsigned fault, unsigned bad) {
  unsigned char key[32] = {1}, nonce[8] = {2}, aad[128];
  unsigned char *plain = malloc(n), *cipher = malloc(n), *out = malloc(n + 32);
  unsigned char tag[16];
  chacha20_poly1305_state s;
  size_t in = 0, written = 0;
  int32_t error = -1;
  assert(plain && cipher && out);
  for (size_t i = 0; i < n; ++i) plain[i] = (unsigned char)(i * 37 + 11);
  memset(aad, 0x59, sizeof(aad));
  reference(key, nonce, aad, plain, n, cipher, tag);
  memcpy(expected_tag, tag, 16);
  if (bad) tag[3] ^= 0x80;
  memset(out, 0xa5, n + 32);
  if (layout == 1) memcpy(out, rx ? cipher : plain, n);
  /* RX tag lives in the destination tail, as on actual wire buffers. */
  if (rx) memcpy(out + n, tag, 16);
  mock_rtl_reset_stats();
  direct_enabled = layout == 2;
  completed = 0; direct_src = plain; direct_dst = out; direct_len = n;
  expected_cipher = cipher; actual_tag = out + n;
  chacha20_poly1305_init_64x64(&s, key, nonce);
  chacha20_poly1305_add_aad(&s, aad, 17);
  chacha20_poly1305_add_aad(&s, aad + 17, 111);
  while (in < n) {
    size_t chunk = layout == 1 ? 71 : n;
    if (chunk > n - in) chunk = n - in;
    const void *src = layout ? out + in : (rx ? cipher : plain) + in;
    written += rx ? chacha20_poly1305_decrypt(&s, src, chunk, out + written)
                  : chacha20_poly1305_encrypt(&s, src, chunk, out + written);
    in += chunk;
  }
  if (fault == 1 || fault == 4) {
    /* RX standalone uses first call for the Poly key, then payload. */
    mock_rtl_fail_chacha_after_write_on(rx && n < 65536 ? 2 : 1);
    if (fault == 4) mock_rtl_recovery_disabled(1);
  }
  if (fault == 2) mock_rtl_fail_poly1305_on(1);
  if (fault == 3) mock_rtl_fail_combined_after_write(1);
  if (fault == 5) fail_alloc = 1;
  printf("CASE rx=%u len=%zu layout=%u fault=%u bad=%u\n", rx, n, layout, fault, bad);
  if (rx) {
    written += chacha20_poly1305_verify(&s, out + written, out + n, &error);
    assert((error == 0) == !bad);
    if (!bad) assert(!memcmp(out, plain, n));
    assert(!memcmp(out + n, tag, 16));
  } else {
    written += chacha20_poly1305_final(&s, out + written, out + n);
    assert(!memcmp(out, cipher, n));
    assert(!memcmp(out + n, expected_tag, 16));
    assert(completed == (layout == 2));
  }
  assert(written == n && !mock_rtl_transaction_active());
  for (size_t i = n + 16; i < n + 32; ++i) assert(out[i] == 0xa5);
  if (fault == 5) {
    assert(!fail_alloc);
    assert(!mock_rtl_chacha_operations() && !mock_rtl_combined_encrypts() &&
           !mock_rtl_decrypt_successes());
  }
  /* Source survives DMA failure for both retained direct and separate input. */
  for (size_t i = 0; i < n; ++i) assert(plain[i] == (unsigned char)(i * 37 + 11));
  direct_enabled = 0;
  free(plain); free(cipher); free(out);
}
/* Corrupt only the reused SW state during the simulated HW failure, after
 * key/nonce export. The original ciphertext and AAD remain valid. This is a
 * fault model, not a claim about the customer's actual timeout cause. */
static void state_recovery(size_t n, unsigned direct, unsigned damage, unsigned bad) {
  unsigned char key[32] = {1}, nonce[8] = {2}, aad[128], tag[16];
  unsigned char *plain = malloc(n), *cipher = malloc(n), *out = malloc(n + 32);
  chacha20_poly1305_state s;
  size_t written;
  int32_t error = -1;
  assert(plain && cipher && out);
  for (size_t i = 0; i < n; ++i) plain[i] = (unsigned char)(i * 37 + 11);
  memset(aad, 0x59, sizeof(aad));
  reference(key, nonce, aad, plain, n, cipher, tag);
  if (bad) tag[3] ^= 0x80;
  memset(out, 0xa5, n + 32);
  memcpy(out + n, tag, 16);
  mock_rtl_reset_stats();
  chacha20_poly1305_init_64x64(&s, key, nonce);
  chacha20_poly1305_add_aad(&s, aad, 17);
  chacha20_poly1305_add_aad(&s, aad + 17, 111);
  written = direct ? chacha20_poly1305_decrypt_rx(&s, cipher, n, out, 0, 1)
                   : chacha20_poly1305_decrypt(&s, cipher, n, out);
  switch (damage) {
    case 1: mock_rtl_corrupt_on_error(&s.poly_h[0]); break;
    case 2: mock_rtl_corrupt_on_error(&s.chacha_counter); break;
    case 3: mock_rtl_corrupt_on_error(&s.aad_len); break;
    case 4: mock_rtl_corrupt_on_error(&s.chacha_key[0]); break;
    case 5: mock_rtl_corrupt_on_error(&s.poly_leftover); break;
    default: break;
  }
  if (n % 16u == 0u) mock_rtl_fail_combined_after_write(1);
  else mock_rtl_fail_chacha_after_write_on(n > 65536u ? 1 : 2);
  printf("CASE rx=1 len=%zu layout=%u fault=6 damage=%u bad=%u\n",
         n, direct, damage, bad);
  written += chacha20_poly1305_verify(&s, out + written, out + n, &error);
  assert(written == n && (error == 0) == !bad);
  if (!bad) assert(!memcmp(out, plain, n));
  else for (size_t i = 0; i < n; ++i) assert(out[i] == 0);
  assert(!memcmp(out + n, tag, 16));
  for (size_t i = n + 16; i < n + 32; ++i) assert(out[i] == 0xa5);
  assert(!mock_rtl_transaction_active());
  free(plain); free(cipher); free(out);
}

int main(void) {
  for (unsigned rx = 0; rx < 2; ++rx) {
    for (unsigned layout = 0; layout < 2; ++layout) {
      for (unsigned fault = 0; fault <= 5; ++fault) {
        size_t n = fault == 3 ? 4096 : 5265;
        run(rx, n, layout, fault, 0);
        run(rx, n, layout, 0, 0); /* prove next HW record works */
        if (rx) run(rx, n, layout, fault, 1);
      }
      run(rx, 131073, layout, 1, 0);
      run(rx, 131073, layout, 0, 0);
      run(rx, 1023, layout, 0, 0);
      run(rx, 1024, layout, 3, 0);
    }
  }
  run(0, 4849, 2, 3, 0); /* reported TX shape, padded combined */
  run(0, 4849, 2, 0, 0);
  run(0, 131073, 2, 1, 0); /* direct chunked TX */
  run(0, 131073, 2, 0, 0);
  for (unsigned direct = 0; direct < 2; ++direct) {
    const size_t lengths[] = {2042, 6132, 8673, 1795, 8597, 9197,
                              2005, 2756, 5613, 4096, 65537};
    for (size_t i = 0; i < sizeof(lengths)/sizeof(lengths[0]); ++i) {
      for (unsigned damage = 0; damage <= 5; ++damage) {
        state_recovery(lengths[i], direct, damage, 0);
        state_recovery(lengths[i], direct, damage, 1);
      }
    }
  }
  puts("TX/RX recovery, partial DMA, bad tag, allocation, streaming, direct TX: PASS");
  return 0;
}
