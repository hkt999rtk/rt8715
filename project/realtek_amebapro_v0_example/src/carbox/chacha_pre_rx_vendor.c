#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

/*
 * Preserve the customer's complete ChaCha path before the screen RX task
 * starts, then leave every later transaction on the normal implementation.
 * Routing is fixed at init time and retained through final/verify.
 */
#define PRE_RX_STATE_SLOTS 64u
#define PRE_RX_ROUTE_NORMAL 1u
#define PRE_RX_ROUTE_VENDOR 2u

typedef struct {
	void *state;
	uint8_t route;
} pre_rx_state_slot;

static pre_rx_state_slot pre_rx_states[PRE_RX_STATE_SLOTS];
static volatile uint32_t pre_rx_latched;
static volatile uint32_t pre_rx_vendor_count;
static volatile uint32_t pre_rx_nettransport_copy_seen;

static int pre_rx_name_equal(const char *left, const char *right)
{
	if (left == NULL || right == NULL) {
		return 0;
	}
	while (*left != '\0' && *right != '\0' && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

static int pre_rx_is_receiver_task(void)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	const char *name = task != NULL ? pcTaskGetName(task) : NULL;

	if (pre_rx_name_equal(name, "AirPlayScreenReceiver")) {
		if (__sync_bool_compare_and_swap(&pre_rx_latched, 0u, 1u)) {
			printf("[CHACHA][PRE_RX] RX boundary reached; normal ChaCha only\n");
		}
		return 1;
	}
	return 0;
}

static uint8_t pre_rx_select_route(void)
{
	if (pre_rx_is_receiver_task()) {
		return PRE_RX_ROUTE_NORMAL;
	}
	return __atomic_load_n(&pre_rx_latched, __ATOMIC_ACQUIRE) != 0u ?
		PRE_RX_ROUTE_NORMAL : PRE_RX_ROUTE_VENDOR;
}

static pre_rx_state_slot *pre_rx_find(void *state, int create, uint8_t route)
{
	uintptr_t start = ((uintptr_t)state >> 3) % PRE_RX_STATE_SLOTS;
	uintptr_t i;

	for (i = 0; i < PRE_RX_STATE_SLOTS; ++i) {
		pre_rx_state_slot *slot =
			&pre_rx_states[(start + i) % PRE_RX_STATE_SLOTS];
		void *owner = __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);

		if (owner == state) {
			if (create) {
				slot->route = route;
			}
			return slot;
		}
		if (create && owner == NULL &&
		    __sync_bool_compare_and_swap(&slot->state, NULL, state)) {
			slot->route = route;
			return slot;
		}
	}
	return NULL;
}

static uint8_t pre_rx_route(void *state)
{
	pre_rx_state_slot *slot;

	(void)pre_rx_is_receiver_task();
	slot = pre_rx_find(state, 0, 0u);
	return slot != NULL ? slot->route : PRE_RX_ROUTE_NORMAL;
}

static void pre_rx_release(void *state)
{
	pre_rx_state_slot *slot = pre_rx_find(state, 0, 0u);

	if (slot != NULL) {
		slot->route = 0u;
		__atomic_store_n(&slot->state, NULL, __ATOMIC_RELEASE);
	}
}

/* These symbols are referenced only by the namespaced customer objects. */
void *carbox_vendor_chacha_memcpy(void *dst, const void *src, size_t len)
{
	volatile uint8_t *out = (volatile uint8_t *)dst;
	const volatile uint8_t *in = (const volatile uint8_t *)src;
	size_t i;

	for (i = 0; i < len; ++i) {
		out[i] = in[i];
	}
	return dst;
}

void *carbox_vendor_chacha_memset(void *dst, int value, size_t len)
{
	volatile uint8_t *out = (volatile uint8_t *)dst;
	size_t i;

	for (i = 0; i < len; ++i) {
		out[i] = (uint8_t)value;
	}
	return dst;
}

extern void *__wrap_memcpy(void *dst, const void *src, size_t len);
extern void *__wrap_memset(void *dst, int value, size_t len);

/* Called only from NetTransportChaCha20Poly1305.o after symbol patching. */
void *carbox_chacha_pre_rx_memcpy(void *dst, const void *src, size_t len)
{
	if (pre_rx_is_receiver_task() ||
	    __atomic_load_n(&pre_rx_latched, __ATOMIC_ACQUIRE) != 0u) {
		return __wrap_memcpy(dst, src, len);
	}
	if (__sync_bool_compare_and_swap(
		    &pre_rx_nettransport_copy_seen, 0u, 1u)) {
		printf("[CHACHA][PRE_RX] NetTransport private memcpy/memset\n");
	}
	return carbox_vendor_chacha_memcpy(dst, src, len);
}

void *carbox_chacha_pre_rx_memset(void *dst, int value, size_t len)
{
	if (pre_rx_is_receiver_task() ||
	    __atomic_load_n(&pre_rx_latched, __ATOMIC_ACQUIRE) != 0u) {
		return __wrap_memset(dst, value, len);
	}
	return carbox_vendor_chacha_memset(dst, value, len);
}

extern void __real_chacha20_poly1305_init_64x64(
	void *state, const uint8_t key[32], const uint8_t nonce[8]);
extern void __real_chacha20_poly1305_add_aad(
	void *state, const void *src, size_t len);
extern size_t __real_chacha20_poly1305_encrypt(
	void *state, const void *src, size_t len, void *dst);
extern size_t __real_chacha20_poly1305_decrypt(
	void *state, const void *src, size_t len, void *dst);
extern size_t __real_chacha20_poly1305_final(
	void *state, void *dst, uint8_t tag[16]);
extern size_t __real_chacha20_poly1305_verify(
	void *state, void *dst, const uint8_t tag[16], int32_t *out_error);
extern void __real_chacha20_poly1305_encrypt_all_64x64(
	const uint8_t key[32], const uint8_t nonce[8], const void *aad,
	size_t aad_len, const void *src, size_t len, void *dst, uint8_t tag[16]);
extern int32_t __real_chacha20_poly1305_decrypt_all_64x64(
	const uint8_t key[32], const uint8_t nonce[8], const void *aad,
	size_t aad_len, const void *src, size_t len, void *dst,
	const uint8_t tag[16]);

extern void carbox_vendor_chacha20_poly1305_init_64x64(
	void *state, const uint8_t key[32], const uint8_t nonce[8]);
extern void carbox_vendor_chacha20_poly1305_add_aad(
	void *state, const void *src, size_t len);
extern size_t carbox_vendor_chacha20_poly1305_encrypt(
	void *state, const void *src, size_t len, void *dst);
extern size_t carbox_vendor_chacha20_poly1305_decrypt(
	void *state, const void *src, size_t len, void *dst);
extern size_t carbox_vendor_chacha20_poly1305_final(
	void *state, void *dst, uint8_t tag[16]);
extern size_t carbox_vendor_chacha20_poly1305_verify(
	void *state, void *dst, const uint8_t tag[16], int32_t *out_error);
extern void carbox_vendor_chacha20_poly1305_encrypt_all_64x64(
	const uint8_t key[32], const uint8_t nonce[8], const void *aad,
	size_t aad_len, const void *src, size_t len, void *dst, uint8_t tag[16]);
extern int32_t carbox_vendor_chacha20_poly1305_decrypt_all_64x64(
	const uint8_t key[32], const uint8_t nonce[8], const void *aad,
	size_t aad_len, const void *src, size_t len, void *dst,
	const uint8_t tag[16]);

void __wrap_chacha20_poly1305_init_64x64(
	void *state, const uint8_t key[32], const uint8_t nonce[8])
{
	uint8_t route = pre_rx_select_route();
	pre_rx_state_slot *slot = pre_rx_find(state, 1, route);

	if (slot == NULL) {
		route = PRE_RX_ROUTE_NORMAL;
	}
	if (route == PRE_RX_ROUTE_VENDOR) {
		uint32_t count = __sync_add_and_fetch(&pre_rx_vendor_count, 1u);
		if (count == 1u) {
			printf("[CHACHA][PRE_RX] customer ChaCha + private memcpy/memset\n");
		}
		carbox_vendor_chacha20_poly1305_init_64x64(state, key, nonce);
	} else {
		__real_chacha20_poly1305_init_64x64(state, key, nonce);
	}
}

void __wrap_chacha20_poly1305_add_aad(
	void *state, const void *src, size_t len)
{
	if (pre_rx_route(state) == PRE_RX_ROUTE_VENDOR) {
		carbox_vendor_chacha20_poly1305_add_aad(state, src, len);
	} else {
		__real_chacha20_poly1305_add_aad(state, src, len);
	}
}

size_t __wrap_chacha20_poly1305_encrypt(
	void *state, const void *src, size_t len, void *dst)
{
	if (pre_rx_route(state) == PRE_RX_ROUTE_VENDOR) {
		return carbox_vendor_chacha20_poly1305_encrypt(state, src, len, dst);
	}
	return __real_chacha20_poly1305_encrypt(state, src, len, dst);
}

size_t __wrap_chacha20_poly1305_decrypt(
	void *state, const void *src, size_t len, void *dst)
{
	if (pre_rx_route(state) == PRE_RX_ROUTE_VENDOR) {
		return carbox_vendor_chacha20_poly1305_decrypt(state, src, len, dst);
	}
	return __real_chacha20_poly1305_decrypt(state, src, len, dst);
}

size_t __wrap_chacha20_poly1305_final(
	void *state, void *dst, uint8_t tag[16])
{
	uint8_t route = pre_rx_route(state);
	size_t written = route == PRE_RX_ROUTE_VENDOR ?
		carbox_vendor_chacha20_poly1305_final(state, dst, tag) :
		__real_chacha20_poly1305_final(state, dst, tag);

	pre_rx_release(state);
	return written;
}

size_t __wrap_chacha20_poly1305_verify(
	void *state, void *dst, const uint8_t tag[16], int32_t *out_error)
{
	uint8_t route = pre_rx_route(state);
	size_t written = route == PRE_RX_ROUTE_VENDOR ?
		carbox_vendor_chacha20_poly1305_verify(state, dst, tag, out_error) :
		__real_chacha20_poly1305_verify(state, dst, tag, out_error);

	pre_rx_release(state);
	return written;
}

void __wrap_chacha20_poly1305_encrypt_all_64x64(
	const uint8_t key[32], const uint8_t nonce[8], const void *aad,
	size_t aad_len, const void *src, size_t len, void *dst, uint8_t tag[16])
{
	if (pre_rx_select_route() == PRE_RX_ROUTE_VENDOR) {
		carbox_vendor_chacha20_poly1305_encrypt_all_64x64(
			key, nonce, aad, aad_len, src, len, dst, tag);
	} else {
		__real_chacha20_poly1305_encrypt_all_64x64(
			key, nonce, aad, aad_len, src, len, dst, tag);
	}
}

int32_t __wrap_chacha20_poly1305_decrypt_all_64x64(
	const uint8_t key[32], const uint8_t nonce[8], const void *aad,
	size_t aad_len, const void *src, size_t len, void *dst,
	const uint8_t tag[16])
{
	if (pre_rx_select_route() == PRE_RX_ROUTE_VENDOR) {
		return carbox_vendor_chacha20_poly1305_decrypt_all_64x64(
			key, nonce, aad, aad_len, src, len, dst, tag);
	}
	return __real_chacha20_poly1305_decrypt_all_64x64(
		key, nonce, aad, aad_len, src, len, dst, tag);
}
