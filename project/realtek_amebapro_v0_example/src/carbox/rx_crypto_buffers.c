/* Receiver-local adapters; see tools/patch_rx_crypto_archive.py for the exact
 * closed-object ABI and relocation guard. No vendor state structure is grown. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "video_handover_zero_copy.h"
#include "chacha_key_alias_fix.h"
#include "../../GCC-RELEASE/carplay_app/chacha_m33/ChaCha20Poly1305.h"

#define RX_SLOTS 8u
#ifndef CONFIG_CHACHA_HW_MIN_LEN
#define CONFIG_CHACHA_HW_MIN_LEN 1024u
#endif

typedef struct {
    TaskHandle_t task;
    const void *a;
    void *b;
    chacha20_poly1305_state *state;
    size_t len;
    int verified, recovered;
    unsigned record;
} screen_record;
static screen_record screens[RX_SLOTS];

static screen_record *screen_find(const void *a, const void *state)
{
    unsigned i;
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    for (i = 0; i < RX_SLOTS; ++i)
        if (screens[i].task == task && screens[i].a &&
            ((a && screens[i].a == a) || (state && screens[i].state == state)))
            return &screens[i];
    return NULL;
}

size_t carbox_screen_rx_decrypt(chacha20_poly1305_state *state,
                               const void *src, size_t len, void *dst)
{
    screen_record *r = NULL;
    void *plain = NULL;
    unsigned i;
    if (src == dst && len >= CONFIG_CHACHA_HW_MIN_LEN && len <= SIZE_MAX - 16u) {
        taskENTER_CRITICAL();
        for (i = 0; i < RX_SLOTS; ++i) {
            if (!screens[i].a) {
                r = &screens[i];
                r->task = xTaskGetCurrentTaskHandle();
                r->a = src;
                break;
            }
        }
        taskEXIT_CRITICAL();
        if (r) plain = carbox_video_handover_source_malloc(len + 16u);
        if (plain) {
            r->b = plain;
            r->state = state;
            r->len = len;
        } else if (r) {
            taskENTER_CRITICAL();
            memset(r, 0, sizeof(*r));
            taskEXIT_CRITICAL();
        }
    }
    return carbox_chacha_decrypt_rx(state, src, len, plain ? plain : dst,
                                      0u, plain != NULL);
}

size_t carbox_screen_rx_verify(chacha20_poly1305_state *state, void *dst,
                              const uint8_t tag[16], int32_t *error)
{
    screen_record *r = screen_find(NULL, state);
    size_t n;
    if (r && dst == (const uint8_t *)r->a + r->len)
        dst = (uint8_t *)r->b + r->len;
    n = chacha20_poly1305_verify(state, dst, tag, error); /* alias wrapper */
    if (r) {
        r->verified = *error == 0;
    }
    return n;
}

/* ARM AAPCS word forwarding: r0-r3 plus the five caller stack words. The
 * 64-bit timestamp starts at stack[0], callback/context at stack[3]/[4].
 * This preserves every ABI slot without guessing proprietary typedefs. */
extern int ScreenStreamProcessData(void *, const void *, size_t, uintptr_t,
                                  uintptr_t, uintptr_t, uintptr_t,
                                  uintptr_t, uintptr_t);
int carbox_screen_rx_process(void *stream, const void *input, size_t len,
                             uintptr_t r3, uintptr_t s0, uintptr_t s1,
                             uintptr_t s2, uintptr_t s3, uintptr_t s4)
{
    screen_record *r = screen_find(input, NULL);
    int result, recovered = 0;
    unsigned record = r ? r->record : 0;
    if (r) {
        if (!r->verified || r->len != len) return -1;
        input = r->b;
        recovered = r->recovered;
    }
    result = ScreenStreamProcessData(stream, input, len, r3,s0,s1,s2,s3,s4);
    if (recovered)
        printf("[CHACHARXREC][RX] id=%u kind=0 recovered_delivery=%d len=%lu\n",
               record, result, (unsigned long)len);
    return result;
}

void carbox_screen_rx_free(void *pointer)
{
    screen_record *r = screen_find(pointer, NULL);
    if (r) {
        void *b = r->b;
        taskENTER_CRITICAL();
        memset(r, 0, sizeof(*r));
        taskEXIT_CRITICAL();
        /* B may still have a queue consumer; release only its producer ref. */
        carbox_video_handover_producer_free(b);
    }
    carbox_video_handover_producer_free(pointer);
}

size_t carbox_audio_general_decrypt(chacha20_poly1305_state *state,
                                   const void *src, size_t len, void *dst)
{
    return carbox_chacha_decrypt_rx(state, src, len, dst, 1u, 1);
}

/* MainAlt keeps its existing plaintext node and jitter-buffer ownership.
 * Socket input goes to A; only RTP header + tag/nonce metadata is copied into
 * the original node. The payload is decrypted directly into that node. */
typedef struct {
    TaskHandle_t task;
    uint8_t *node, *wire;
    size_t received;
    int encrypted, recovered;
    unsigned record;
} audio_record;
static audio_record audios[RX_SLOTS];
static audio_record *audio_find(const void *node)
{
    unsigned i;
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    for (i = 0; i < RX_SLOTS; ++i)
        if (audios[i].task == task && audios[i].node == node) return &audios[i];
    return NULL;
}
static void audio_release(audio_record *r)
{
    uint8_t *wire;
    if (!r) return;
    wire = r->wire;
    taskENTER_CRITICAL();
    memset(r, 0, sizeof(*r));
    taskEXIT_CRITICAL();
    free(wire);
}
extern int RTPJitterBufferGetFreeNode(void *, void **);
extern void RTPJitterBufferPutFreeNode(void *, void *);
extern int RTPJitterBufferPutBusyNode(void *, void *);
extern int SocketRecvFrom(int, void *, size_t, size_t *, uintptr_t, uintptr_t,
                          uintptr_t, uintptr_t, uintptr_t, uintptr_t);
int carbox_audio_get_node(void *jitter, void **node)
{
    int result = RTPJitterBufferGetFreeNode(jitter, node);
    unsigned i;
    if (result || !*node) return result;
    taskENTER_CRITICAL();
    for (i = 0; i < RX_SLOTS; ++i) {
        if (!audios[i].node) {
            audios[i].node = *node;
            audios[i].task = xTaskGetCurrentTaskHandle();
            /* _MainAltAudioThread: jitter=context+48, encrypted=context+1432. */
            audios[i].encrypted = *((const uint8_t *)jitter + 1384u) != 0;
            break;
        }
    }
    taskEXIT_CRITICAL();
    return result;
}
int carbox_audio_recv(int fd, void *buffer, size_t capacity, size_t *received,
                      uintptr_t s0, uintptr_t s1, uintptr_t s2,
                      uintptr_t s3, uintptr_t s4, uintptr_t s5)
{
    audio_record *r = audio_find((uint8_t *)buffer - 8u);
    int result;
    if (r && r->encrypted && capacity == 1472u)
        r->wire = malloc(capacity);
    result = SocketRecvFrom(fd, r && r->wire ? r->wire : buffer, capacity,
                            received, s0,s1,s2,s3,s4,s5);
    if (r && r->encrypted && result == 0 && *received < 36u) return -1;
    if (r && r->wire && result == 0) {
        if (*received > capacity) return -1;
        r->received = *received;
        /* Reject encrypted underlength records before vendor subtracts 24. */
        if (*received < 36u) return -1;
        memcpy(buffer, r->wire, 12u);
        memcpy((uint8_t *)buffer + *received - 24u,
               r->wire + *received - 24u, 24u);
    }
    return result;
}
size_t carbox_audio_main_decrypt(chacha20_poly1305_state *state,
                                const void *src, size_t len, void *dst)
{
    audio_record *r = audio_find((const uint8_t *)src - 20u);
    int direct = r && r->wire && r->received >= 36u &&
                 len == r->received - 36u && src == dst;
    return carbox_chacha_decrypt_rx(state, direct ? r->wire + 12u : src,
                                       len, dst, 2u, direct);
}
size_t carbox_audio_main_verify(chacha20_poly1305_state *state, void *dst,
                               const uint8_t tag[16], int32_t *error)
{
    return chacha20_poly1305_verify(state, dst, tag, error);
}
void carbox_audio_put_free(void *jitter, void *node)
{
    audio_release(audio_find(node));
    RTPJitterBufferPutFreeNode(jitter, node);
}
int carbox_audio_put_busy(void *jitter, void *node)
{
    audio_record *r = audio_find(node);
    int recovered = r && r->recovered;
    unsigned record = r ? r->record : 0;
    int result;
    /* No code touches B after publication: a consumer may run immediately. */
    audio_release(r);
    result = RTPJitterBufferPutBusyNode(jitter, node);
    if (recovered) printf("[CHACHARXREC][RX] id=%u kind=2 recovered_queue=%d\n", record, result);
    return result;
}

size_t carbox_audio_general_verify(chacha20_poly1305_state *state, void *dst,
                                  const uint8_t tag[16], int32_t *error)
{
    return chacha20_poly1305_verify(state, dst, tag, error);
}

/* Called before the alias wrapper restores overlapping persistent key bytes.
 * All record metadata lives outside the closed 0x118-byte crypto ABI. */
void carbox_chacha_rx_result(const void *state, void *output, unsigned id,
                            unsigned kind, int retried, int32_t error)
{
    if (kind == 0u) {
        screen_record *r = screen_find(NULL, state);
        if (r) { r->recovered = retried && !error; r->record = id; }
    } else if (kind == 2u) {
        audio_record *r = audio_find((uint8_t *)output - 20u);
        if (r) { r->recovered = retried && !error; r->record = id; }
    } else if (retried && !error) {
        printf("[CHACHARXREC][RX] id=%u kind=1 recovered_verified=1\n", id);
    }
}
