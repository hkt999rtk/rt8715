#include "ChaCha20Poly1305.h"
#include "screen_rx_poly_buffer.h"
#include "task.h"
#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TaskHandle_t rx_poly_test_task = (void *)(uintptr_t)1;
static unsigned fail_allocations;
void *__real_malloc(size_t length);
void *__wrap_malloc(size_t length)
{
    if (fail_allocations) {
        --fail_allocations;
        return NULL;
    }
    return __real_malloc(length);
}
void mock_rtl_reset_stats(void);
void mock_rtl_fail_chacha_on(unsigned call);
void mock_rtl_fail_chacha_after_write_on(unsigned call);
void mock_rtl_fail_poly1305_on(unsigned call);
unsigned mock_rtl_chacha_operations(void);
size_t mock_rtl_last_chacha_len(void);
uint32_t mock_rtl_last_chacha_counter(void);
const void *mock_rtl_last_chacha_input(void);
void *mock_rtl_last_chacha_output(void);
unsigned mock_rtl_transaction_active(void);
const void *mock_rtl_last_poly_input(void);

static void run_case(size_t n, size_t aad_len, int bad_tag, int fail, int other_task)
{
    uint8_t key[32] = {1}, nonce[8] = {2}, nonce12[12] = {0};
    uint8_t aad[129], tag[16];
    uint8_t *plain = malloc(n ? n : 1);
    uint8_t *payload = carbox_screen_rx_poly_alloc(n + 16);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    chacha20_poly1305_state state;
    size_t i, written;
    int out, tail;
    int32_t error;
    size_t poly_len = 128 + ((n + 15) & ~(size_t)15) + 16;
    int direct = CONFIG_SCREEN_RX_POLY_INPLACE && !other_task &&
                 aad_len == 128 && n >= 4096 && (n & 15) && poly_len <= 65536;
    assert(plain && payload && ctx);
    for (i = 0; i < n; ++i) plain[i] = (uint8_t)(i * 37 + 11);
    memset(aad, 0x5a, sizeof(aad));
    memcpy(nonce12 + 4, nonce, 8);
    assert(EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, key, nonce12) == 1);
    assert(EVP_EncryptUpdate(ctx, NULL, &out, aad, (int)aad_len) == 1);
    assert(EVP_EncryptUpdate(ctx, payload, &out, plain, (int)n) == 1);
    assert(EVP_EncryptFinal_ex(ctx, payload + out, &tail) == 1);
    assert((size_t)(out + tail) == n);
    assert(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag) == 1);
    EVP_CIPHER_CTX_free(ctx);
    if (bad_tag) tag[3] ^= 0x80;
    memcpy(payload + n, tag, 16);
    /* The old allocation's extra readable tail remains part of the contract. */
    memset(payload + n + 16, 0xa5, 15);
    if (other_task) rx_poly_test_task = (void *)(uintptr_t)2;
    mock_rtl_reset_stats();
    if (fail == 1) mock_rtl_fail_poly1305_on(1);
    /* First raw ChaCha call derives the Poly1305 key; second decrypts data. */
    if (fail == 2) mock_rtl_fail_chacha_on(2);
    if (fail == 3) mock_rtl_fail_chacha_on(1);
    if (fail == 4) mock_rtl_fail_chacha_after_write_on(2);
    chacha20_poly1305_init_64x64(&state, key, nonce);
    chacha20_poly1305_add_aad(&state, aad, aad_len);
    /* Exercise streaming updates including a buffered partial ChaCha block. */
    i = n > 37 ? 37 : n;
    written = chacha20_poly1305_decrypt(&state, payload, i, payload);
    written += chacha20_poly1305_decrypt(&state, payload + i, n - i, payload + written);
    written += chacha20_poly1305_verify(&state, payload + written, payload + n, &error);
    assert(written == n);
    assert((error != 0) == (bad_tag || fail));
    assert(memcmp(payload + n, tag, 16) == 0);
    if (!fail && !bad_tag) assert(memcmp(payload, plain, n) == 0);
    if (direct && fail != 3) {
        assert(mock_rtl_last_poly_input() == payload - 128);
        assert(memcmp(payload - 128, aad, 128) == 0);
    } else {
        assert((uintptr_t)mock_rtl_last_poly_input() != (uintptr_t)payload - 128);
    }
    if (direct) {
        if (fail == 1 || fail == 3) {
            assert(mock_rtl_chacha_operations() == 1);
        } else if (CONFIG_SCREEN_RX_CHACHA_SINGLE_PASS) {
            /* One key-generation call plus ONE in-place payload call,
             * including on a failure after the mock has written the tail. */
            assert(mock_rtl_chacha_operations() == 2);
            assert(mock_rtl_last_chacha_len() == ((n + 15) & ~(size_t)15));
            assert(mock_rtl_last_chacha_counter() == 1);
            assert(mock_rtl_last_chacha_input() == payload);
            assert(mock_rtl_last_chacha_output() == payload);
        } else {
            assert(mock_rtl_chacha_operations() == (fail ? 2 : 3));
        }
    }
    assert(!mock_rtl_transaction_active());
    carbox_screen_rx_poly_free(payload); /* Final release may be another task. */
    free(plain);
    rx_poly_test_task = (void *)(uintptr_t)1;
}

static void allocation_cases(void)
{
    void *p[65];
    unsigned i;
    size_t n = 4097, length = 128 + 4112 + 16;
    assert(carbox_screen_rx_poly_alloc(SIZE_MAX) == NULL);
    fail_allocations = 1;
    p[0] = carbox_screen_rx_poly_alloc(n + 16);
    assert((p[0] != NULL) == CONFIG_SCREEN_RX_POLY_INPLACE);
    assert(!carbox_screen_rx_poly_input(p[0], n, 128, length));
    carbox_screen_rx_poly_free(p[0]);
    fail_allocations = 2;
    assert(carbox_screen_rx_poly_alloc(n + 16) == NULL);
    fail_allocations = 0;
    carbox_screen_rx_poly_free(NULL);
    carbox_screen_rx_poly_free(malloc(100)); /* Untracked destination/context. */
    for (i = 0; i < 65; ++i) {
        p[i] = carbox_screen_rx_poly_alloc(n + 16);
        assert(p[i]);
        assert((carbox_screen_rx_poly_input(p[i], n, 128, length) != NULL) ==
               (CONFIG_SCREEN_RX_POLY_INPLACE && i < 64));
        assert(!carbox_screen_rx_poly_input(p[i], n - 1, 128, length));
        assert(!carbox_screen_rx_poly_input(p[i], n, 127, length));
        assert(!carbox_screen_rx_poly_input(p[i], n, 128, length - 1));
        assert(!carbox_screen_rx_poly_input((uint8_t *)p[i] + 1, n, 128, length));
    }
    for (i = 0; i < 65; ++i) carbox_screen_rx_poly_free(p[i]);
    /* All slots are reusable after release, including after table exhaustion. */
    p[0] = carbox_screen_rx_poly_alloc(n + 16);
    assert((carbox_screen_rx_poly_input(p[0], n, 128, length) != NULL) ==
           CONFIG_SCREEN_RX_POLY_INPLACE);
    carbox_screen_rx_poly_free(p[0]);
}

int main(void)
{
    const size_t lengths[] = {0, 1, 15, 16, 4095, 4096, 32769, 65376,
                             65391, 65392, 65393, 65535, 65536, 65537};
    unsigned i;
    allocation_cases();
    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i)
        run_case(lengths[i], 128, 0, 0, 0);
    for (i = 1; i < 64; ++i) {
        if ((i & 15) == 0) continue;
        run_case(4096 + i, 128, 0, 0, 0);
        run_case(4096 + i, 128, 1, 0, 0);
        run_case(4096 + i, 128, 0, 1, 0);
        run_case(4096 + i, 128, 0, 2, 0);
        run_case(4096 + i, 128, 0, 3, 0);
        run_case(4096 + i, 128, 0, 4, 0);
    }
    run_case(4097, 128, 0, 0, 1);
    run_case(4097, 127, 0, 0, 0);
    run_case(4097, 129, 0, 0, 0);
    puts("screen RX Poly1305 ownership/layout/crypto tests passed");
    return 0;
}
