#ifndef CHACHA20_POLY1305_H
#define CHACHA20_POLY1305_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Core API: encryption/decryption are identical for ChaCha20 XOR stream. */
void chacha20_xor(
  uint8_t *out, const uint8_t *in, size_t len,
  const uint8_t key_bytes[32], const uint8_t nonce_bytes[12],
  uint32_t counter
);

#define chacha20_encode chacha20_xor
#define chacha20_decode chacha20_xor

#define CHACHA20_POLY1305_TAG_BYTES 16u

#define CARBOX_CHACHA_MODE_SOFTWARE_ONLY       0
#define CARBOX_CHACHA_MODE_SOFTWARE_HW_VERIFY  1
#define CARBOX_CHACHA_MODE_HARDWARE_ONLY       2
#define CARBOX_CHACHA_MODE_SPLIT_RX_SW_TX_HW   3

#define CARBOX_CHACHA_TX_USES_HARDWARE \
  ((CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_HARDWARE_ONLY) || \
   (CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_SPLIT_RX_SW_TX_HW))
#define CARBOX_CHACHA_RX_USES_HARDWARE \
  (CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_HARDWARE_ONLY)

#ifndef CARBOX_CHACHA_MODE
#define CARBOX_CHACHA_MODE CARBOX_CHACHA_MODE_SOFTWARE_ONLY
#endif

#if (CARBOX_CHACHA_MODE < CARBOX_CHACHA_MODE_SOFTWARE_ONLY) || \
    (CARBOX_CHACHA_MODE > CARBOX_CHACHA_MODE_SPLIT_RX_SW_TX_HW)
#error "Unsupported CARBOX_CHACHA_MODE"
#endif

#ifndef CARBOX_CHACHA_HW_MIN_LEN
#define CARBOX_CHACHA_HW_MIN_LEN 4096u
#endif

/*
 * Keep 0 for the existing standalone hardware Poly1305 baseline. Set to 1
 * to authenticate non-16-byte-aligned records with streaming software
 * Poly1305, avoiding construction of a payload-sized contiguous Poly input.
 */
#ifndef CARBOX_CHACHA_NONALIGNED_SW_POLY
#define CARBOX_CHACHA_NONALIGNED_SW_POLY 0
#endif
#if (CARBOX_CHACHA_NONALIGNED_SW_POLY != 0) && \
    (CARBOX_CHACHA_NONALIGNED_SW_POLY != 1)
#error "CARBOX_CHACHA_NONALIGNED_SW_POLY must be 0 or 1"
#endif

/* Set to 0 to disable periodic HW/SW traffic statistics. */
#ifndef CARBOX_CHACHA_STATS_INTERVAL_MS
#define CARBOX_CHACHA_STATS_INTERVAL_MS 5000u
#endif

/*
 * Diagnostic customer build only. Run the RTL8195B Poly1305 streaming and
 * ChaCha in-place capability tests once, immediately before the first real
 * hardware transaction. Keep disabled in production builds.
 */
#ifndef CARBOX_CHACHA_HW_SELFTEST
#define CARBOX_CHACHA_HW_SELFTEST 0
#endif
#if (CARBOX_CHACHA_HW_SELFTEST != 0) && (CARBOX_CHACHA_HW_SELFTEST != 1)
#error "CARBOX_CHACHA_HW_SELFTEST must be 0 or 1"
#endif

/*
 * Diagnostic-only ROM capability probe.  This bypasses the public RTL8195B
 * combined ChaCha20-Poly1305 wrapper's msglen%16 rejection and compares the
 * ROM-stub result against the portable implementation.  Production routing
 * remains unchanged regardless of the result.
 */
#ifndef CARBOX_CHACHA_COMBINED_PARTIAL_SELFTEST
#define CARBOX_CHACHA_COMBINED_PARTIAL_SELFTEST 0
#endif
#if (CARBOX_CHACHA_COMBINED_PARTIAL_SELFTEST != 0) && \
    (CARBOX_CHACHA_COMBINED_PARTIAL_SELFTEST != 1)
#error "CARBOX_CHACHA_COMBINED_PARTIAL_SELFTEST must be 0 or 1"
#endif

typedef struct {
  uint32_t chacha_key[8];
  uint32_t chacha_nonce[2];
  uint64_t chacha_counter;
  uint8_t chacha_buffer[64];
  size_t chacha_leftover;

  uint32_t poly_r[5];
  uint32_t poly_s[4];
  uint32_t poly_h[5];
  uint8_t poly_buffer[16];
  size_t poly_leftover;
  uint8_t poly_pad[16];

  uint64_t aad_len;
  uint64_t data_len;
  uint8_t aad_padded;

  /*
   * Private backend bookkeeping. The closed CarPlay objects allocate 0x118
   * bytes for this state. Keep this structure at or below that ABI limit.
   */
  uint8_t *rtl_aad;
  const uint8_t *rtl_input_base;
  const uint8_t *rtl_input_next;
  uint8_t *rtl_input_snapshot;
  uint8_t *rtl_output_base;
  uint8_t *rtl_output_next;
  size_t rtl_aad_len;
  size_t rtl_input_snapshot_len;
  uint8_t rtl_direction;
  uint8_t rtl_eligible;
  uint8_t rtl_aad_seen;
  uint8_t rtl_reserved;
} chacha20_poly1305_state;

/*
 * Streaming ChaCha20-Poly1305 with a 64-bit nonce and 64-bit block counter.
 * Call add_aad before encrypt. final writes any buffered ciphertext to out
 * and writes the 16-byte authentication tag to tag.
 */
void chacha20_poly1305_init_64x64(
  chacha20_poly1305_state *state,
  const uint8_t key[32], const uint8_t nonce[8]
);
void chacha20_poly1305_add_aad(
  chacha20_poly1305_state *state, const void *src, size_t len
);
size_t chacha20_poly1305_encrypt(
  chacha20_poly1305_state *state,
  const void *src, size_t len, void *dst
);
size_t chacha20_poly1305_decrypt(
  chacha20_poly1305_state *state,
  const void *src, size_t len, void *dst
);
size_t chacha20_poly1305_final(
  chacha20_poly1305_state *state,
  void *dst, uint8_t tag[CHACHA20_POLY1305_TAG_BYTES]
);
size_t chacha20_poly1305_verify(
  chacha20_poly1305_state *state, void *dst,
  const uint8_t tag[CHACHA20_POLY1305_TAG_BYTES], int32_t *out_error
);
void chacha20_poly1305_encrypt_all_64x64(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *plaintext, size_t plaintext_len,
  void *ciphertext, uint8_t tag[CHACHA20_POLY1305_TAG_BYTES]
);
int32_t chacha20_poly1305_decrypt_all_64x64(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *ciphertext, size_t ciphertext_len,
  void *plaintext, const uint8_t tag[CHACHA20_POLY1305_TAG_BYTES]
);

#if CARBOX_CHACHA_COMBINED_PARTIAL_SELFTEST
void chacha20_poly1305_reference_encrypt_all_64x64(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *plaintext, size_t plaintext_len,
  void *ciphertext, uint8_t tag[CHACHA20_POLY1305_TAG_BYTES]
);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CHACHA20_POLY1305_H */
