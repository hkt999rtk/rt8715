#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "diag.h"
#include "chacha_key_alias_fix.h"
#include "screen_rx_record_profiler.h"

#ifndef CONFIG_SCREEN_FPS_PROFILE
#define CONFIG_SCREEN_FPS_PROFILE 0
#endif

#ifndef CONFIG_CHACHA_MODE
#define CONFIG_CHACHA_MODE 0
#endif

#if CONFIG_CHACHA_MODE == 0
#define CARBOX_CHACHA_RX_BACKEND "SW"
#elif CONFIG_CHACHA_MODE == 1
#define CARBOX_CHACHA_RX_BACKEND "SW_HW_VERIFY"
#elif CONFIG_CHACHA_MODE == 2
#define CARBOX_CHACHA_RX_BACKEND "HW"
#elif CONFIG_CHACHA_MODE == 3
#define CARBOX_CHACHA_RX_BACKEND "SW"
#else
#define CARBOX_CHACHA_RX_BACKEND "UNKNOWN"
#endif

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

#if CONFIG_SCREEN_FPS_PROFILE
#define CARBOX_CHACHA_RX_PROFILE_SLOTS 8u

typedef struct {
  void *state;
  uint32_t start_us;
  uint32_t bytes;
  uint32_t decrypt_calls;
} carbox_chacha_rx_profile_slot;

typedef struct {
  uint32_t records;
  uint32_t errors;
  uint32_t decrypt_calls;
  uint64_t bytes;
  uint64_t decrypt_us;
  uint32_t decrypt_max_us;
  uint32_t decrypt_over_8ms;
  uint32_t decrypt_over_16ms;
  uint64_t verify_us;
  uint32_t verify_max_us;
  uint64_t record_us;
  uint32_t record_max_us;
  uint32_t record_over_16ms;
  uint32_t record_over_33ms;
  uint32_t slot_miss;
} carbox_chacha_rx_latency_stats;

static carbox_chacha_rx_profile_slot g_rx_profile_slots[
  CARBOX_CHACHA_RX_PROFILE_SLOTS
];
static carbox_chacha_rx_latency_stats g_rx_latency;
static TaskHandle_t g_rx_task;

static int chacha_rx_is_screen_task(void) {
  TaskHandle_t task = xTaskGetCurrentTaskHandle();
  const char *name;

  if (task == g_rx_task) return task != NULL;
  name = pcTaskGetName(task);
  if (name && strcmp(name, "AirPlayScreenReceiver") == 0) {
    g_rx_task = task;
    return 1;
  }
  return 0;
}

static carbox_chacha_rx_profile_slot *chacha_rx_profile_find(
  void *state, int allocate
) {
  carbox_chacha_rx_profile_slot *free_slot = NULL;
  unsigned int i;

  for (i = 0; i < CARBOX_CHACHA_RX_PROFILE_SLOTS; ++i) {
    carbox_chacha_rx_profile_slot *slot = &g_rx_profile_slots[i];
    if (slot->state == state) return slot;
    if (!slot->state && !free_slot) free_slot = slot;
  }
  if (allocate && free_slot) {
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->state = state;
  }
  return allocate ? free_slot : NULL;
}

static uint32_t chacha_rx_decrypt_begin(void *state, size_t len) {
  carbox_chacha_rx_profile_slot *slot;
  uint32_t now_us;

  if (!chacha_rx_is_screen_task()) return 0u;
  now_us = hal_read_curtime_us();
  taskENTER_CRITICAL();
  slot = chacha_rx_profile_find(state, 1);
  if (slot) {
    if (slot->decrypt_calls == 0u) slot->start_us = now_us;
    slot->decrypt_calls++;
    slot->bytes += (uint32_t)len;
  } else {
    g_rx_latency.slot_miss++;
  }
  taskEXIT_CRITICAL();
  return slot ? now_us : 0u;
}

static void chacha_rx_decrypt_end(uint32_t start_us) {
  uint32_t elapsed_us;

  if (start_us == 0u) return;
  elapsed_us = hal_read_curtime_us() - start_us;
  taskENTER_CRITICAL();
  g_rx_latency.decrypt_calls++;
  g_rx_latency.decrypt_us += elapsed_us;
  if (elapsed_us > g_rx_latency.decrypt_max_us) {
    g_rx_latency.decrypt_max_us = elapsed_us;
  }
  g_rx_latency.decrypt_over_8ms += elapsed_us > 8000u;
  g_rx_latency.decrypt_over_16ms += elapsed_us > 16667u;
  taskEXIT_CRITICAL();
}

static uint32_t chacha_rx_verify_begin(void *state) {
  if (!chacha_rx_is_screen_task()) return 0u;
  taskENTER_CRITICAL();
  if (!chacha_rx_profile_find(state, 0)) {
    taskEXIT_CRITICAL();
    return 0u;
  }
  taskEXIT_CRITICAL();
  return hal_read_curtime_us();
}

static void chacha_rx_verify_end(void *state, uint32_t start_us,
                                 const int32_t *error) {
  carbox_chacha_rx_profile_slot *slot;
  uint32_t now_us;
  uint32_t verify_us;
  uint32_t record_us;

  if (start_us == 0u) return;
  now_us = hal_read_curtime_us();
  verify_us = now_us - start_us;
  taskENTER_CRITICAL();
  slot = chacha_rx_profile_find(state, 0);
  if (!slot) {
    g_rx_latency.slot_miss++;
    taskEXIT_CRITICAL();
    return;
  }
  record_us = now_us - slot->start_us;
  g_rx_latency.records++;
  g_rx_latency.errors += error == NULL || *error != 0;
  g_rx_latency.bytes += slot->bytes;
  g_rx_latency.verify_us += verify_us;
  if (verify_us > g_rx_latency.verify_max_us) {
    g_rx_latency.verify_max_us = verify_us;
  }
  g_rx_latency.record_us += record_us;
  if (record_us > g_rx_latency.record_max_us) {
    g_rx_latency.record_max_us = record_us;
  }
  g_rx_latency.record_over_16ms += record_us > 16667u;
  g_rx_latency.record_over_33ms += record_us > 33333u;
  memset(slot, 0, sizeof(*slot));
  taskEXIT_CRITICAL();
}
#endif

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
#if CONFIG_SCREEN_FPS_PROFILE
  uint32_t profile_start_us = chacha_rx_decrypt_begin(state, len);
#endif
  size_t written = __real_chacha20_poly1305_decrypt(state, src, len, dst);
#if CONFIG_SCREEN_FPS_PROFILE
  chacha_rx_decrypt_end(profile_start_us);
#endif
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
#if CONFIG_SCREEN_FPS_PROFILE
  uint32_t profile_start_us = chacha_rx_verify_begin(state);
#endif
  size_t written =
    __real_chacha20_poly1305_verify(state, dst, tag, out_error);
#if CONFIG_SCREEN_FPS_PROFILE
  chacha_rx_verify_end(state, profile_start_us, out_error);
#endif
  carbox_screen_rx_crypto_verify(written, out_error);
  restore_slot(find_slot(state));
  return written;
}

void carbox_chacha_rx_latency_report(uint32_t sequence) {
#if CONFIG_SCREEN_FPS_PROFILE
  carbox_chacha_rx_latency_stats stats;
  uint32_t mbps_x100;

  taskENTER_CRITICAL();
  stats = g_rx_latency;
  memset(&g_rx_latency, 0, sizeof(g_rx_latency));
  taskEXIT_CRITICAL();
  if (stats.decrypt_calls == 0u && stats.records == 0u) return;
  /* Use complete-record latency for throughput.  Hardware-only mode defers
   * the actual crypto work from update/decrypt to verify/finalize, so using
   * decrypt_us alone measured only wrapper/copy overhead. */
  mbps_x100 = stats.record_us != 0u ?
    (uint32_t)((stats.bytes * 800u) / stats.record_us) : 0u;
  rt_printf(
    "[CHACHARX][%lu] dir=RX_DEC backend=%s records/error=%lu/%lu "
    "calls/bytes=%lu/%llu update_us avg/max=%llu/%lu over8/16ms=%lu/%lu "
    "finalize_us avg/max=%llu/%lu record_us avg/max=%llu/%lu "
    "over16/33ms=%lu/%lu record_throughput=%lu.%02luMbps slot_miss=%lu\r\n",
    (unsigned long)sequence,
    CARBOX_CHACHA_RX_BACKEND,
    (unsigned long)stats.records, (unsigned long)stats.errors,
    (unsigned long)stats.decrypt_calls, (unsigned long long)stats.bytes,
    (unsigned long long)(stats.decrypt_calls ?
      stats.decrypt_us / stats.decrypt_calls : 0u),
    (unsigned long)stats.decrypt_max_us,
    (unsigned long)stats.decrypt_over_8ms,
    (unsigned long)stats.decrypt_over_16ms,
    (unsigned long long)(stats.records ?
      stats.verify_us / stats.records : 0u),
    (unsigned long)stats.verify_max_us,
    (unsigned long long)(stats.records ?
      stats.record_us / stats.records : 0u),
    (unsigned long)stats.record_max_us,
    (unsigned long)stats.record_over_16ms,
    (unsigned long)stats.record_over_33ms,
    (unsigned long)(mbps_x100 / 100u),
    (unsigned long)(mbps_x100 % 100u),
    (unsigned long)stats.slot_miss
  );
#else
  (void)sequence;
#endif
}
