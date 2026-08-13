#include <stddef.h>

int printf(const char *format, ...);
#include <stdint.h>

/*
 * Private byte primitives for the customer's ChaCha/NetTransport objects.
 * Volatile accesses prevent the compiler from folding these loops back into
 * the globally wrapped libc entry points.
 */
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

/* The replacement NetTransport diagnostic object uses the pre-RX symbol
 * names. In private-memory verification mode they are unconditional aliases;
 * no RX task is started during the first-frame test. */
void *carbox_chacha_pre_rx_memcpy(void *dst, const void *src, size_t len)
{
	return carbox_vendor_chacha_memcpy(dst, src, len);
}

void *carbox_chacha_pre_rx_memset(void *dst, int value, size_t len)
{
	return carbox_vendor_chacha_memset(dst, value, len);
}

/*
 * Diagnostic hook used only by the customer-private-software archive.  The
 * customer's HARDWARE_ONLY object already contains its complete software
 * implementation and selects it whenever the RTL precheck returns a skip
 * status.  Returning 7 (the customer's "below threshold / software" status)
 * keeps all ChaCha state handling and output inside that exact customer
 * object while preventing the RTL backend from touching the transaction.
 */
int carbox_vendor_chacha_force_software_precheck(const void *state)
{
	static int announced;

	(void)state;
	if (!announced) {
		announced = 1;
		printf("[CHACHA][CUSTOMER_SW] customer .a software path forced; private memcpy/memset\n");
	}
	return 7;
}
