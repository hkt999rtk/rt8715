#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Diagnostic wrappers for the customer's original ChaCha object.  The
 * linker redirects only undefined CarPlay calls through these functions;
 * __real_* remains the untouched implementation from lib_CarPlay.a.
 */
#define CHACHA_VENDOR_TRACE_SLOTS 64u

typedef struct {
  void *state;
  uint32_t sequence;
  uint64_t aad_len;
  size_t data_len;
  const uint8_t *input_base;
  uint8_t *output_base;
  uint32_t aad_hash;
  uint32_t input_hash;
  uint32_t key_hash;
  uint8_t key[32];
  uint8_t nonce[8];
  uint8_t *key_target;
  uint8_t *nonce_target;
  uint8_t restore_key;
  uint8_t restore_nonce;
  uint8_t aad_prefix[128];
  uint8_t aad_prefix_len;
  uint8_t input_prefix[16];
  uint8_t input_prefix_len;
  uint8_t direction;
} chacha_vendor_trace_slot;

static chacha_vendor_trace_slot g_chacha_vendor_trace[CHACHA_VENDOR_TRACE_SLOTS];
static uint32_t g_chacha_vendor_sequence;
static uint32_t g_chacha_vendor_fullkey_banner;
static uint32_t g_chacha_vendor_alias_banner;

static int chacha_vendor_ranges_overlap(
  const void *a_arg, size_t a_len, const void *b_arg, size_t b_len
) {
  uintptr_t a = (uintptr_t)a_arg;
  uintptr_t b = (uintptr_t)b_arg;

  return (a < b + b_len) && (b < a + a_len);
}

static uint32_t chacha_vendor_hash(const uint8_t *data, size_t len) {
  uint32_t hash = 2166136261u;
  size_t i;

  for (i = 0; i < len; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

static chacha_vendor_trace_slot *chacha_vendor_find(void *state, int create) {
  uintptr_t start = ((uintptr_t)state >> 3) % CHACHA_VENDOR_TRACE_SLOTS;
  uintptr_t i;

  for (i = 0; i < CHACHA_VENDOR_TRACE_SLOTS; ++i) {
    chacha_vendor_trace_slot *slot =
      &g_chacha_vendor_trace[(start + i) % CHACHA_VENDOR_TRACE_SLOTS];
    void *owner = __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);

    if (owner == state) return slot;
    if (create && owner == NULL &&
        __sync_bool_compare_and_swap(&slot->state, NULL, state)) {
      slot->sequence = __sync_add_and_fetch(&g_chacha_vendor_sequence, 1u);
      slot->aad_len = 0;
      slot->data_len = 0;
      slot->input_base = NULL;
      slot->output_base = NULL;
      slot->aad_hash = 2166136261u;
      slot->input_hash = 2166136261u;
      slot->key_hash = 0u;
      slot->key_target = NULL;
      slot->nonce_target = NULL;
      slot->restore_key = 0u;
      slot->restore_nonce = 0u;
      slot->aad_prefix_len = 0;
      slot->input_prefix_len = 0;
      slot->direction = 0;
      return slot;
    }
  }
  return NULL;
}

static void chacha_vendor_record_io(
  void *state, const void *src, size_t len, void *dst, uint8_t direction
) {
  chacha_vendor_trace_slot *slot = chacha_vendor_find(state, 0);

  if (!slot) return;
  {
    const uint8_t *bytes = (const uint8_t *)src;
    size_t i;

    for (i = 0; i < len; ++i) {
      slot->input_hash ^= bytes[i];
      slot->input_hash *= 16777619u;
      if (slot->input_prefix_len < sizeof(slot->input_prefix)) {
        slot->input_prefix[slot->input_prefix_len++] = bytes[i];
      }
    }
  }
  if (slot->data_len == 0u) {
    slot->input_base = (const uint8_t *)src;
    slot->output_base = (uint8_t *)dst;
    slot->direction = direction;
  }
  slot->data_len += len;
}

static void chacha_vendor_report(
  void *state, const uint8_t tag[16], size_t final_len, int have_error,
  int32_t error
) {
  chacha_vendor_trace_slot *slot = chacha_vendor_find(state, 0);

  if (!slot) return;
  if ((slot->sequence <= 16u) || (slot->data_len >= 512u)) {
    unsigned int i;
    printf(
      "[CHACHA][VENDOR_TRACE] seq=%lu dir=%s aad=%lu data=%lu final=%lu "
      "inplace=%u%s%ld key_hash=%08lx key=",
      (unsigned long)slot->sequence,
      slot->direction == 2u ? "DEC" : "ENC",
      (unsigned long)slot->aad_len, (unsigned long)slot->data_len,
      (unsigned long)final_len,
      slot->input_base == slot->output_base ? 1u : 0u,
      have_error ? " err=" : " status=", have_error ? (long)error : 0L,
      (unsigned long)slot->key_hash
    );
    for (i = 0; i < sizeof(slot->key); ++i) printf("%02X", slot->key[i]);
    printf(" nonce=");
    for (i = 0; i < sizeof(slot->nonce); ++i) printf("%02X", slot->nonce[i]);
    printf(
      " aad_hash=%08lx aad_prefix=",
      (unsigned long)slot->aad_hash
    );
    for (i = 0; i < slot->aad_prefix_len; ++i) {
      printf("%02X", slot->aad_prefix[i]);
    }
    printf(
      " in_hash=%08lx out_hash=%08lx tag_hash=%08lx tag=",
      (unsigned long)slot->input_hash,
      (unsigned long)chacha_vendor_hash(slot->output_base, slot->data_len),
      (unsigned long)chacha_vendor_hash(tag, 16u)
    );
    for (i = 0; i < 16u; ++i) printf("%02X", tag[i]);
    printf(" ct_prefix=");
    for (i = 0; i < 16u && i < slot->data_len; ++i) {
      printf("%02X", slot->output_base[i]);
    }
    printf(" pt_prefix=");
    for (i = 0; i < slot->input_prefix_len; ++i) {
      printf("%02X", slot->input_prefix[i]);
    }
    printf("\n");
  }
  if (slot->restore_key && slot->key_target) {
    volatile uint8_t *target = (volatile uint8_t *)slot->key_target;
    unsigned int i;
    for (i = 0; i < sizeof(slot->key); ++i) target[i] = slot->key[i];
  }
  if (slot->restore_nonce && slot->nonce_target) {
    volatile uint8_t *target = (volatile uint8_t *)slot->nonce_target;
    unsigned int i;
    for (i = 0; i < sizeof(slot->nonce); ++i) target[i] = slot->nonce[i];
  }
  __atomic_store_n(&slot->state, NULL, __ATOMIC_RELEASE);
}

extern void __real_chacha20_poly1305_init_64x64(
  void *state, const uint8_t key[32], const uint8_t nonce[8]
);
extern void __real_chacha20_poly1305_add_aad(
  void *state, const void *src, size_t len
);
extern size_t __real_chacha20_poly1305_encrypt(
  void *state, const void *src, size_t len, void *dst
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
  chacha_vendor_trace_slot *slot = chacha_vendor_find(state, 1);
  size_t i;

  if (__sync_bool_compare_and_swap(&g_chacha_vendor_fullkey_banner, 0u, 1u)) {
    printf(
      "[CHACHA][FULLKEY_TRACE] build=20260813-3 enabled "
      "fields=key32,nonce8,aad128,tag16,ct16,pt16\n"
    );
  }
  if (slot) {
    slot->key_hash = chacha_vendor_hash(key, 32u);
    for (i = 0; i < sizeof(slot->key); ++i) slot->key[i] = key[i];
    for (i = 0; i < sizeof(slot->nonce); ++i) slot->nonce[i] = nonce[i];
    slot->key_target = (uint8_t *)(uintptr_t)key;
    slot->nonce_target = (uint8_t *)(uintptr_t)nonce;
    slot->restore_key = (uint8_t)chacha_vendor_ranges_overlap(
      state, 0x118u, key, sizeof(slot->key)
    );
    slot->restore_nonce = (uint8_t)chacha_vendor_ranges_overlap(
      state, 0x118u, nonce, sizeof(slot->nonce)
    );
    if ((slot->restore_key || slot->restore_nonce) &&
        __sync_bool_compare_and_swap(&g_chacha_vendor_alias_banner, 0u, 1u)) {
      printf(
        "[CHACHA][KEY_ALIAS_FIX] enabled state_span=0x118 "
        "key_overlap=%u nonce_overlap=%u\n",
        (unsigned int)slot->restore_key,
        (unsigned int)slot->restore_nonce
      );
    }
  }
  __real_chacha20_poly1305_init_64x64(
    state, slot ? slot->key : key, slot ? slot->nonce : nonce
  );
}

void __wrap_chacha20_poly1305_add_aad(
  void *state, const void *src, size_t len
) {
  chacha_vendor_trace_slot *slot = chacha_vendor_find(state, 0);
  if (slot) {
    const uint8_t *bytes = (const uint8_t *)src;
    size_t i;

    slot->aad_len += len;
    for (i = 0; i < len; ++i) {
      slot->aad_hash ^= bytes[i];
      slot->aad_hash *= 16777619u;
      if (slot->aad_prefix_len < sizeof(slot->aad_prefix)) {
        slot->aad_prefix[slot->aad_prefix_len++] = bytes[i];
      }
    }
  }
  __real_chacha20_poly1305_add_aad(state, src, len);
}

size_t __wrap_chacha20_poly1305_encrypt(
  void *state, const void *src, size_t len, void *dst
) {
  size_t written;
  chacha_vendor_record_io(state, src, len, dst, 1u);
  written = __real_chacha20_poly1305_encrypt(state, src, len, dst);
  return written;
}

size_t __wrap_chacha20_poly1305_decrypt(
  void *state, const void *src, size_t len, void *dst
) {
  size_t written;
  chacha_vendor_record_io(state, src, len, dst, 2u);
  written = __real_chacha20_poly1305_decrypt(state, src, len, dst);
  return written;
}

size_t __wrap_chacha20_poly1305_final(
  void *state, void *dst, uint8_t tag[16]
) {
  size_t written = __real_chacha20_poly1305_final(state, dst, tag);
  chacha_vendor_report(state, tag, written, 0, 0);
  return written;
}

size_t __wrap_chacha20_poly1305_verify(
  void *state, void *dst, const uint8_t tag[16], int32_t *out_error
) {
  size_t written =
    __real_chacha20_poly1305_verify(state, dst, tag, out_error);
  chacha_vendor_report(state, tag, written, 1, *out_error);
  return written;
}
