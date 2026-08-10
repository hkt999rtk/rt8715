#include "aes_backend_select.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "diag.h"
#include "screen_tx_direct_crypto.h"

#ifndef CONFIG_AES_MODE
#define CONFIG_AES_MODE 2
#endif

#define CARBOX_AES_MODE_SOFTWARE_ONLY       0
#define CARBOX_AES_MODE_SOFTWARE_HW_VERIFY  1
#define CARBOX_AES_MODE_HARDWARE_ONLY       2

#if (CONFIG_AES_MODE < CARBOX_AES_MODE_SOFTWARE_ONLY) || \
	(CONFIG_AES_MODE > CARBOX_AES_MODE_HARDWARE_ONLY)
#error "CONFIG_AES_MODE must be 0, 1, or 2"
#endif

/* Globalized from the vendor AESUtils.o by chacha_m33/Makefile. */
extern volatile int gAESRTLHWState;

#if CONFIG_AES_MODE == CARBOX_AES_MODE_SOFTWARE_HW_VERIFY

#define AES_VERIFY_SLOTS       32U
#define AES_VERIFY_CHUNK_SIZE  4096U

typedef struct aes_verify_slot_s {
	void *context;
	uint8_t key[16];
	uint8_t counter[16];
	uint8_t stream[16];
	size_t used;
	uint8_t active;
} aes_verify_slot_t;

/* Pure software helper globalized from AESUtils.o. */
extern void _AES_RTL_SW_ECB(const uint8_t key[16], int encrypt,
			     const uint8_t *source, size_t length,
			     uint8_t *destination);

static SemaphoreHandle_t aes_verify_mutex;
static aes_verify_slot_t aes_verify_slots[AES_VERIFY_SLOTS];
static uint8_t aes_verify_input[AES_VERIFY_CHUNK_SIZE]
	__attribute__((aligned(32)));
static uint8_t aes_verify_hardware[AES_VERIFY_CHUNK_SIZE]
	__attribute__((aligned(32)));
static uint32_t aes_verify_operations;
static uint32_t aes_verify_mismatches;
static uint32_t aes_verify_hw_errors;

static void aes_counter_increment(uint8_t counter[16])
{
	int i;

	for (i = 15; i >= 0; --i) {
		counter[i]++;
		if (counter[i] != 0U) break;
	}
}

static aes_verify_slot_t *aes_verify_find(void *context)
{
	unsigned int i;

	for (i = 0; i < AES_VERIFY_SLOTS; ++i) {
		if (aes_verify_slots[i].active &&
		    aes_verify_slots[i].context == context) {
			return &aes_verify_slots[i];
		}
	}
	return NULL;
}

static int aes_verify_register(void *context, const uint8_t key[16],
			       const uint8_t nonce[16])
{
	aes_verify_slot_t *slot;
	unsigned int i;

	if (aes_verify_mutex == NULL) return -1;
	xSemaphoreTake(aes_verify_mutex, portMAX_DELAY);
	slot = aes_verify_find(context);
	if (slot == NULL) {
		for (i = 0; i < AES_VERIFY_SLOTS; ++i) {
			if (!aes_verify_slots[i].active) {
				slot = &aes_verify_slots[i];
				break;
			}
		}
	}
	if (slot != NULL) {
		memset(slot, 0, sizeof(*slot));
		slot->context = context;
		memcpy(slot->key, key, sizeof(slot->key));
		memcpy(slot->counter, nonce, sizeof(slot->counter));
		slot->active = 1U;
	}
	xSemaphoreGive(aes_verify_mutex);
	return slot != NULL ? 0 : -1;
}

static void aes_verify_unregister(void *context)
{
	aes_verify_slot_t *slot;

	if (aes_verify_mutex == NULL) return;
	xSemaphoreTake(aes_verify_mutex, portMAX_DELAY);
	slot = aes_verify_find(context);
	if (slot != NULL) memset(slot, 0, sizeof(*slot));
	xSemaphoreGive(aes_verify_mutex);
}

static void aes_verify_software_update(aes_verify_slot_t *slot,
				       const uint8_t *source, size_t length,
				       uint8_t *destination)
{
	size_t count;

	while ((length > 0U) && (slot->used != 0U)) {
		*destination++ = *source++ ^ slot->stream[slot->used++];
		slot->used %= 16U;
		length--;
	}

	while (length >= 16U) {
		_AES_RTL_SW_ECB(slot->key, 1, slot->counter, 16U,
				 slot->stream);
		aes_counter_increment(slot->counter);
		for (count = 0; count < 16U; ++count) {
			destination[count] = source[count] ^ slot->stream[count];
		}
		source += 16U;
		destination += 16U;
		length -= 16U;
	}

	if (length > 0U) {
		_AES_RTL_SW_ECB(slot->key, 1, slot->counter, 16U,
				 slot->stream);
		aes_counter_increment(slot->counter);
		for (count = 0; count < length; ++count) {
			destination[count] = source[count] ^ slot->stream[count];
		}
		slot->used = length;
	}
}

#endif

extern int __real_AES_CTR_Init(void *context, const uint8_t key[16],
			       const uint8_t nonce[16]);
extern int __real_AES_CTR_Update(void *context, const void *source,
				 size_t length, void *destination);
extern void __real_AES_CTR_Final(void *context);

void carbox_aes_backend_select(void)
{
#if CONFIG_AES_MODE == CARBOX_AES_MODE_SOFTWARE_ONLY
	/* A negative state routes all vendor AES modes to their software fallback. */
	gAESRTLHWState = -1;
	rt_printf("[AES] mode=SOFTWARE_ONLY\r\n");
#elif CONFIG_AES_MODE == CARBOX_AES_MODE_SOFTWARE_HW_VERIFY
	aes_verify_mutex = xSemaphoreCreateMutex();
	if (aes_verify_mutex == NULL) {
		rt_printf("[AES] mode=SOFTWARE_HW_VERIFY init failed\r\n");
	} else {
		rt_printf("[AES] mode=SOFTWARE_HW_VERIFY software-authoritative\r\n");
	}
#else
	rt_printf("[AES] mode=HARDWARE_ONLY\r\n");
#endif
}

int __wrap_AES_CTR_Init(void *context, const uint8_t key[16],
			const uint8_t nonce[16])
{
	int status = __real_AES_CTR_Init(context, key, nonce);

#if CONFIG_AES_MODE == CARBOX_AES_MODE_SOFTWARE_HW_VERIFY
	if ((status == 0) && (aes_verify_register(context, key, nonce) != 0)) {
		__real_AES_CTR_Final(context);
		status = -1;
	}
#endif
	return status;
}

int __wrap_AES_CTR_Update(void *context, const void *source, size_t length,
			  void *destination)
{
	const void *direct_source = source;
	int direct = carbox_screen_tx_crypto_begin((void *)(uintptr_t)source,
		length, destination, CARBOX_SCREEN_TX_CRYPTO_AES,
		&direct_source);
	int status;

#if CONFIG_AES_MODE == CARBOX_AES_MODE_SOFTWARE_HW_VERIFY
	aes_verify_slot_t *slot;
	const uint8_t *input = (const uint8_t *)direct_source;
	uint8_t *output = (uint8_t *)destination;
	size_t remaining = length;
	size_t offset = 0U;

	if (aes_verify_mutex == NULL) {
		status = -1;
		goto exit;
	}
	xSemaphoreTake(aes_verify_mutex, portMAX_DELAY);
	slot = aes_verify_find(context);
	if (slot == NULL) {
		status = -1;
	} else {
		status = 0;
		while (remaining > 0U) {
			size_t chunk = remaining > AES_VERIFY_CHUNK_SIZE ?
				AES_VERIFY_CHUNK_SIZE : remaining;
			int hw_status;

			memcpy(aes_verify_input, input, chunk);
			aes_verify_software_update(slot, aes_verify_input, chunk,
						   output);
			hw_status = __real_AES_CTR_Update(context, aes_verify_input,
						 chunk, aes_verify_hardware);
			aes_verify_operations++;
			if ((hw_status != 0) || (gAESRTLHWState != 1)) {
				aes_verify_hw_errors++;
				rt_printf("[AES][VERIFY] HW ERROR op=%lu offset=%lu err=%d state=%d\r\n",
					  (unsigned long)aes_verify_operations,
					  (unsigned long)offset, hw_status,
					  gAESRTLHWState);
			} else if (memcmp(output, aes_verify_hardware, chunk) != 0) {
				size_t bad;

				for (bad = 0; bad < chunk; ++bad) {
					if (output[bad] != aes_verify_hardware[bad]) break;
				}
				aes_verify_mismatches++;
				rt_printf("[AES][VERIFY] MISMATCH op=%lu offset=%lu byte=%lu sw=%02x hw=%02x total=%lu\r\n",
					  (unsigned long)aes_verify_operations,
					  (unsigned long)offset,
					  (unsigned long)(offset + bad),
					  output[bad], aes_verify_hardware[bad],
					  (unsigned long)aes_verify_mismatches);
			}
			input += chunk;
			output += chunk;
			remaining -= chunk;
			offset += chunk;
		}
	}
	xSemaphoreGive(aes_verify_mutex);
#else
	status = __real_AES_CTR_Update(context, direct_source, length,
				       destination);
#endif

#if CONFIG_AES_MODE == CARBOX_AES_MODE_SOFTWARE_HW_VERIFY
exit:
#endif
	if (direct) {
		carbox_screen_tx_crypto_complete(destination, length,
			CARBOX_SCREEN_TX_CRYPTO_AES, status);
	}
	return status;
}

void __wrap_AES_CTR_Final(void *context)
{
#if CONFIG_AES_MODE == CARBOX_AES_MODE_SOFTWARE_HW_VERIFY
	aes_verify_unregister(context);
#endif
	__real_AES_CTR_Final(context);
}
