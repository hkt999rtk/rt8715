#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "screen_rx_record_profiler.h"

/*
 * AirPlayScreen's closed-object ABI places the persistent 32-byte key and
 * 8-byte nonce inside the 0x118-byte region now used by the replacement
 * ChaCha state.  Snapshot them before init clears/uses that state, then put
 * them back only after final/verify has finished with the state.
 *
 * This wrapper is deliberately independent of the software/HW backend.  It
 * therefore protects the software fallback, HW verify and performance-first
 * HW modes without adding any payload copy to the fast path.
 */
#define CARBOX_CHACHA_STATE_ABI_SIZE 0x118u
#define CARBOX_CHACHA_ALIAS_SLOTS 64u

typedef struct {
  void *state;
  uint8_t key[32];
  uint8_t nonce[8];
  volatile uint8_t *key_target;
  volatile uint8_t *nonce_target;
  uint8_t restore_key;
  uint8_t restore_nonce;
} carbox_chacha_alias_slot;

static carbox_chacha_alias_slot g_alias_slots[CARBOX_CHACHA_ALIAS_SLOTS];
static uint32_t g_alias_banner;

/* The performance replacement archive keeps its NetTransport member pointed
 * at these dispatch names.  With pre-RX A/B routing disabled, forward them to
 * the normal measured memory wrappers. */
extern void *__wrap_memcpy(void *dst, const void *src, size_t len);
extern void *__wrap_memset(void *dst, int value, size_t len);

void *carbox_chacha_pre_rx_memcpy(void *dst, const void *src, size_t len) {
  return __wrap_memcpy(dst, src, len);
}

void *carbox_chacha_pre_rx_memset(void *dst, int value, size_t len) {
  return __wrap_memset(dst, value, len);
}

static int ranges_overlap(
  const void *a_arg, size_t a_len, const void *b_arg, size_t b_len
) {
  uintptr_t a = (uintptr_t)a_arg;
  uintptr_t b = (uintptr_t)b_arg;
  return (a < b + b_len) && (b < a + a_len);
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, size_t len) {
  size_t i;
  for (i = 0; i < len; ++i) dst[i] = src[i];
}

static carbox_chacha_alias_slot *find_slot(void *state) {
  unsigned int i;
  for (i = 0; i < CARBOX_CHACHA_ALIAS_SLOTS; ++i) {
    if (__atomic_load_n(&g_alias_slots[i].state, __ATOMIC_ACQUIRE) == state) {
      return &g_alias_slots[i];
    }
  }
  return NULL;
}

static void restore_slot(carbox_chacha_alias_slot *slot) {
  size_t i;
  if (!slot) return;
  if (slot->restore_key && slot->key_target) {
    for (i = 0; i < sizeof(slot->key); ++i) {
      slot->key_target[i] = slot->key[i];
    }
  }
  if (slot->restore_nonce && slot->nonce_target) {
    for (i = 0; i < sizeof(slot->nonce); ++i) {
      slot->nonce_target[i] = slot->nonce[i];
    }
  }
  __atomic_store_n(&slot->state, NULL, __ATOMIC_RELEASE);
}

static carbox_chacha_alias_slot *allocate_slot(void *state) {
  unsigned int i;
  carbox_chacha_alias_slot *old = find_slot(state);

  /* Recover persistent material if a caller reinitializes an unfinished
   * record in the same state object. */
  if (old) restore_slot(old);
  for (i = 0; i < CARBOX_CHACHA_ALIAS_SLOTS; ++i) {
    if (__sync_bool_compare_and_swap(&g_alias_slots[i].state, NULL, state)) {
      return &g_alias_slots[i];
    }
  }
  return NULL;
}

extern void __real_chacha20_poly1305_init_64x64(
  void *state, const uint8_t key[32], const uint8_t nonce[8]
);
extern void __real_chacha20_poly1305_add_aad(
  void *state, const void *src, size_t len
);
extern size_t __real_chacha20_poly1305_decrypt(
  void *state, const void *src, size_t len, void *dst
);
extern size_t __real_chacha20_poly1305_final(
  void *state, void *dst, uint8_t tag[16]
);
extern size_t __real_chacha20_poly1305_verify(
  void *state, void *dst, const uint8_t tag[16], int32_t *out_error
);

void __wrap_chacha20_poly1305_init_64x64(
  void *state, const uint8_t key[32], const uint8_t nonce[8]
) {
  uint8_t safe_key[32];
  uint8_t safe_nonce[8];
  int key_overlap = ranges_overlap(
    state, CARBOX_CHACHA_STATE_ABI_SIZE, key, sizeof(safe_key)
  );
  int nonce_overlap = ranges_overlap(
    state, CARBOX_CHACHA_STATE_ABI_SIZE, nonce, sizeof(safe_nonce)
  );
  carbox_chacha_alias_slot *slot = NULL;

  if (key_overlap || nonce_overlap) {
    slot = allocate_slot(state);
  }
  copy_bytes(safe_key, key, sizeof(safe_key));
  copy_bytes(safe_nonce, nonce, sizeof(safe_nonce));
  if (key_overlap || nonce_overlap) {
    if (!slot) {
      printf("[CHACHA][KEY_ALIAS_FIX][FAIL] no free slot\n");
    } else {
      copy_bytes(slot->key, safe_key, sizeof(slot->key));
      copy_bytes(slot->nonce, safe_nonce, sizeof(slot->nonce));
      slot->key_target = (volatile uint8_t *)(uintptr_t)key;
      slot->nonce_target = (volatile uint8_t *)(uintptr_t)nonce;
      slot->restore_key = (uint8_t)key_overlap;
      slot->restore_nonce = (uint8_t)nonce_overlap;
      if (__sync_bool_compare_and_swap(&g_alias_banner, 0u, 1u)) {
        printf(
          "[CHACHA][KEY_ALIAS_FIX] production enabled "
          "state_span=0x118 key_overlap=%d nonce_overlap=%d\n",
          key_overlap, nonce_overlap
        );
      }
    }
  }
  __real_chacha20_poly1305_init_64x64(state, safe_key, safe_nonce);
  carbox_screen_rx_crypto_init();
}

void __wrap_chacha20_poly1305_add_aad(
  void *state, const void *src, size_t len
) {
  __real_chacha20_poly1305_add_aad(state, src, len);
  carbox_screen_rx_crypto_aad(len);
}

size_t __wrap_chacha20_poly1305_decrypt(
  void *state, const void *src, size_t len, void *dst
) {
  size_t written = __real_chacha20_poly1305_decrypt(state, src, len, dst);
  carbox_screen_rx_crypto_decrypt(len, written);
  return written;
}

size_t __wrap_chacha20_poly1305_final(
  void *state, void *dst, uint8_t tag[16]
) {
  size_t written = __real_chacha20_poly1305_final(state, dst, tag);
  carbox_screen_rx_crypto_final(written);
  restore_slot(find_slot(state));
  return written;
}

size_t __wrap_chacha20_poly1305_verify(
  void *state, void *dst, const uint8_t tag[16], int32_t *out_error
) {
  size_t written =
    __real_chacha20_poly1305_verify(state, dst, tag, out_error);
  carbox_screen_rx_crypto_verify(written, out_error);
  restore_slot(find_slot(state));
  return written;
}
