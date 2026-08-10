#include "aes_ctr_periodic_selftest.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"

#ifndef CONFIG_AES_CTR_PERIODIC_SELFTEST
#define CONFIG_AES_CTR_PERIODIC_SELFTEST 0
#endif

#ifndef CONFIG_AES_CTR_PERIODIC_SELFTEST_INTERVAL_MS
#define CONFIG_AES_CTR_PERIODIC_SELFTEST_INTERVAL_MS 10000U
#endif

#ifndef CONFIG_AES_CTR_PERIODIC_SELFTEST_TASK_PRIORITY
#define CONFIG_AES_CTR_PERIODIC_SELFTEST_TASK_PRIORITY 1U
#endif

#ifndef CONFIG_AES_CTR_PERIODIC_SELFTEST_TASK_STACK
#define CONFIG_AES_CTR_PERIODIC_SELFTEST_TASK_STACK 512U
#endif

#if CONFIG_AES_CTR_PERIODIC_SELFTEST

/*
 * AESUtils.o is supplied by the CarPlay archive.  Its public AES_CTR API only
 * receives a context pointer, so keep an intentionally oversized, aligned
 * opaque buffer here rather than depending on unavailable proprietary support
 * headers.  The public AESUtils.h context is currently smaller than 512 bytes.
 */
typedef union carbox_aes_ctr_context_u {
	uint32_t align;
	uint8_t opaque[512];
} carbox_aes_ctr_context_t;

extern int AES_CTR_Init(void *context, const uint8_t key[16],
			const uint8_t nonce[16]);
extern int AES_CTR_Update(void *context, const void *source, size_t length,
			  void *destination);
extern void AES_CTR_Final(void *context);

/* Globalized from the vendor AESUtils.o by chacha_m33/Makefile. */
extern volatile int gAESRTLHWState;

static const uint8_t aes_ctr_key[16] __attribute__((aligned(32))) = {
	0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
	0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};

static const uint8_t aes_ctr_iv[16] __attribute__((aligned(32))) = {
	0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
	0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
};

static const uint8_t aes_ctr_plaintext[64] __attribute__((aligned(32))) = {
	0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
	0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
	0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
	0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
	0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11,
	0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
	0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17,
	0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10
};

static const uint8_t aes_ctr_expected[64] __attribute__((aligned(32))) = {
	0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26,
	0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce,
	0x98, 0x06, 0xf6, 0x6b, 0x79, 0x70, 0xfd, 0xff,
	0x86, 0x17, 0x18, 0x7b, 0xb9, 0xff, 0xfd, 0xff,
	0x5a, 0xe4, 0xdf, 0x3e, 0xdb, 0xd5, 0xd3, 0x5e,
	0x5b, 0x4f, 0x09, 0x02, 0x0d, 0xb0, 0x3e, 0xab,
	0x1e, 0x03, 0x1d, 0xda, 0x2f, 0xbe, 0x03, 0xd1,
	0x79, 0x21, 0x70, 0xa0, 0xf3, 0x00, 0x9c, 0xee
};

static int carbox_aes_ctr_periodic_test(uint32_t run)
{
	carbox_aes_ctr_context_t context;
	uint8_t encrypted[64] __attribute__((aligned(32)));
	uint8_t recovered[64] __attribute__((aligned(32)));
	int initialized = 0;
	int error;
	const char *stage = "encrypt-init";

	memset(&context, 0, sizeof(context));
	error = AES_CTR_Init(&context, aes_ctr_key, aes_ctr_iv);
	if (error != 0) goto fail;
	initialized = 1;
	stage = "encrypt-update";
	error = AES_CTR_Update(&context, aes_ctr_plaintext,
			       sizeof(aes_ctr_plaintext), encrypted);
	AES_CTR_Final(&context);
	initialized = 0;
	if (error != 0) goto fail;
	if (gAESRTLHWState != 1) {
		stage = "encrypt-not-hardware";
		error = gAESRTLHWState;
		goto fail;
	}
	if (memcmp(encrypted, aes_ctr_expected, sizeof(encrypted)) != 0) {
		stage = "known-answer";
		error = -1;
		goto fail;
	}

	stage = "decrypt-init";
	error = AES_CTR_Init(&context, aes_ctr_key, aes_ctr_iv);
	if (error != 0) goto fail;
	initialized = 1;
	stage = "decrypt-update";
	error = AES_CTR_Update(&context, encrypted, sizeof(encrypted), recovered);
	AES_CTR_Final(&context);
	initialized = 0;
	if (error != 0) goto fail;
	if (gAESRTLHWState != 1) {
		stage = "decrypt-not-hardware";
		error = gAESRTLHWState;
		goto fail;
	}
	if (memcmp(recovered, aes_ctr_plaintext, sizeof(recovered)) != 0) {
		stage = "round-trip";
		error = -1;
		goto fail;
	}

	rt_printf("[AESCTR][PERIODIC] PASS run=%lu bytes=%lu hw_state=%d\r\n",
		  (unsigned long)run, (unsigned long)sizeof(encrypted),
		  gAESRTLHWState);
	return 0;

fail:
	if (initialized) AES_CTR_Final(&context);
	rt_printf("[AESCTR][PERIODIC] FAIL run=%lu stage=%s err=%d hw_state=%d\r\n",
		  (unsigned long)run, stage, error, gAESRTLHWState);
	return -1;
}

static void carbox_aes_ctr_periodic_selftest_task(void *argument)
{
	TickType_t last_wake = xTaskGetTickCount();
	uint32_t run = 0;

	(void)argument;
	for (;;) {
		vTaskDelayUntil(&last_wake,
			pdMS_TO_TICKS(CONFIG_AES_CTR_PERIODIC_SELFTEST_INTERVAL_MS));
		carbox_aes_ctr_periodic_test(++run);
	}
}

void carbox_aes_ctr_periodic_selftest_start(void)
{
	TaskHandle_t task = NULL;

	if (xTaskCreate(carbox_aes_ctr_periodic_selftest_task, "aesctrtest",
			CONFIG_AES_CTR_PERIODIC_SELFTEST_TASK_STACK, NULL,
			CONFIG_AES_CTR_PERIODIC_SELFTEST_TASK_PRIORITY,
			&task) != pdPASS) {
		rt_printf("[AESCTR][PERIODIC] task create failed\r\n");
	}
}

#else

void carbox_aes_ctr_periodic_selftest_start(void)
{
}

#endif
