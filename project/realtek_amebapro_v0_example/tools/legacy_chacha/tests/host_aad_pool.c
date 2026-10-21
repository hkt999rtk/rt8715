#include "ChaCha20Poly1305.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *carbox_chacha_aad_realloc_default(void *ptr, size_t length);
void *__real_realloc(void *ptr, size_t length);
static unsigned realloc_calls;
static unsigned fail_realloc;

void *__wrap_realloc(void *ptr, size_t length)
{
    __sync_fetch_and_add(&realloc_calls, 1);
    if (fail_realloc && length) {
        fail_realloc = 0;
        return NULL;
    }
    return __real_realloc(ptr, length);
}

static void init(chacha20_poly1305_state *state)
{
    const uint8_t key[32] = {1}, nonce[8] = {2};
    chacha20_poly1305_init_64x64(state, key, nonce);
}

static void finish(chacha20_poly1305_state *state, int decrypt)
{
    uint8_t byte = 0, tag[16] = {0};
    int32_t error;
    if (decrypt) {
        chacha20_poly1305_decrypt(state, &byte, 0, &byte);
        chacha20_poly1305_verify(state, &byte, tag, &error);
        assert(error != 0); /* Invalid tag must still release the snapshot. */
    } else {
        chacha20_poly1305_encrypt(state, &byte, 0, &byte);
        chacha20_poly1305_final(state, &byte, tag);
    }
}

static void snapshot_cases(void)
{
    chacha20_poly1305_state states[17];
    uint8_t aad[512], saved[128], *old;
    unsigned i, j;
    for (i = 0; i < sizeof(aad); ++i) aad[i] = (uint8_t)(i * 13);
    memcpy(saved, aad, 128);
    realloc_calls = 0;
    for (i = 0; i < 17; ++i) {
        init(&states[i]);
        chacha20_poly1305_add_aad(&states[i], aad, 128);
        assert(states[i].rtl_aad && states[i].rtl_aad_len == 128);
        assert(realloc_calls == (CONFIG_CHACHA_AAD_POOL ? (i >= 16 ? 1 : 0) : i + 1));
        for (j = 0; j < i; ++j) assert(states[i].rtl_aad != states[j].rtl_aad);
    }
    memset(aad, 0xa5, 128);
    for (i = 0; i < 17; ++i) {
        assert(!memcmp(states[i].rtl_aad, saved, 128));
        finish(&states[i], i & 1);
    }
    /* A new full set must use the pool again, without leaked final/verify slots. */
    realloc_calls = 0;
    for (i = 0; i < 16; ++i) {
        init(&states[i]);
        chacha20_poly1305_add_aad(&states[i], saved, 128);
    }
    assert(realloc_calls == (CONFIG_CHACHA_AAD_POOL ? 0 : 16));
    for (i = 0; i < 16; ++i) finish(&states[i], i & 1);

    init(&states[0]);
    realloc_calls = 0;
    chacha20_poly1305_add_aad(&states[0], saved, 63);
    old = states[0].rtl_aad;
    chacha20_poly1305_add_aad(&states[0], saved + 63, 65);
    if (CONFIG_CHACHA_AAD_POOL) assert(old == states[0].rtl_aad);
    assert(!memcmp(states[0].rtl_aad, saved, 128));
    assert(realloc_calls == (CONFIG_CHACHA_AAD_POOL ? 0 : 2));
    chacha20_poly1305_add_aad(&states[0], aad + 128, 1);
    assert(realloc_calls == (CONFIG_CHACHA_AAD_POOL ? 1 : 3));
    assert(!memcmp(states[0].rtl_aad, saved, 128));
    assert(states[0].rtl_aad[128] == aad[128]);
    chacha20_poly1305_add_aad(&states[0], aad + 129, sizeof(aad) - 129);
    assert(states[0].rtl_aad_len == sizeof(aad));
    assert(!memcmp(states[0].rtl_aad + 128, aad + 128, sizeof(aad) - 128));
    finish(&states[0], 0);

    /* Failed pool-to-heap growth retains the old data and its release record. */
    init(&states[0]);
    chacha20_poly1305_add_aad(&states[0], saved, 128);
    old = states[0].rtl_aad;
    fail_realloc = 1;
    chacha20_poly1305_add_aad(&states[0], aad, 1);
    assert(!states[0].rtl_eligible && states[0].rtl_aad == old);
    assert(!memcmp(old, saved, 128));
    finish(&states[0], 1);
    init(&states[0]);
    chacha20_poly1305_add_aad(&states[0], saved, 128);
    if (CONFIG_CHACHA_AAD_POOL) assert(states[0].rtl_aad == old);
    finish(&states[0], 0);

    init(&states[0]);
    realloc_calls = 0;
    chacha20_poly1305_add_aad(&states[0], aad, 0);
    assert(!states[0].rtl_aad && !realloc_calls);
    finish(&states[0], 0);
}

#define WORKERS 32
static pthread_barrier_t ready, release_slots;
static uint8_t *thread_slots[WORKERS];

static void *pool_worker(void *arg)
{
    uintptr_t index = (uintptr_t)arg;
    unsigned i;
    uint8_t *p = carbox_chacha_aad_realloc_default(NULL, 128);
    assert(p);
    memset(p, (int)index, 128);
    thread_slots[index] = p;
    pthread_barrier_wait(&ready);
    pthread_barrier_wait(&release_slots);
    for (i = 0; i < 128; ++i) assert(p[i] == index);
    assert(carbox_chacha_aad_realloc_default(p, 0) == NULL);
    return NULL;
}

static void concurrent_cases(void)
{
    pthread_t threads[WORKERS];
    uintptr_t i, j;
    realloc_calls = 0;
    assert(!pthread_barrier_init(&ready, NULL, WORKERS + 1));
    assert(!pthread_barrier_init(&release_slots, NULL, WORKERS + 1));
    for (i = 0; i < WORKERS; ++i)
        assert(!pthread_create(&threads[i], NULL, pool_worker, (void *)i));
    pthread_barrier_wait(&ready);
    assert(realloc_calls == (CONFIG_CHACHA_AAD_POOL ? WORKERS - 16 : WORKERS));
    for (i = 0; i < WORKERS; ++i)
        for (j = 0; j < i; ++j) assert(thread_slots[i] != thread_slots[j]);
    pthread_barrier_wait(&release_slots);
    for (i = 0; i < WORKERS; ++i) assert(!pthread_join(threads[i], NULL));
    pthread_barrier_destroy(&ready);
    pthread_barrier_destroy(&release_slots);
}

int main(void)
{
    snapshot_cases();
    concurrent_cases();
    snapshot_cases(); /* Confirm concurrent release returned every slot. */
    puts("AAD pool lifecycle/growth/fallback/concurrency tests passed");
    return 0;
}
