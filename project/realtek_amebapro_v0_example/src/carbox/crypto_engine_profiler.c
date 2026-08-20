#include "crypto_engine_profiler.h"
#include "crypto_priority_lock.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "device_lock.h"
#include "diag.h"
#include "hal_timer.h"

#ifndef CONFIG_CRYPTO_ENGINE_PROFILE
#define CONFIG_CRYPTO_ENGINE_PROFILE 0
#endif

#if CONFIG_CRYPTO_ENGINE_PROFILE

/*
 * AES, ChaCha and Poly1305 share RT_DEV_LOCK_CRYPTO and one global hardware
 * adapter. Wrapping the common device lock observes every crypto client
 * without replacing the vendor algorithm objects.
 *
 * The hold interval includes parameter setup, DMA/cache work and completion
 * wait. It is therefore a conservative upper bound on hardware occupancy and
 * the serialized interval relevant to crypto-engine contention.
 */
typedef struct crypto_prof_stats_s {
	uint32_t lock_calls;
	uint32_t completed_calls;
	uint64_t wait_us;
	uint32_t wait_us_max;
	uint32_t wait_ge_10us;
	uint32_t wait_ge_100us;
	uint32_t wait_ge_1ms;
	uint32_t wait_ge_10ms;
	uint64_t hold_us;
	uint32_t hold_us_max;
	uint32_t hold_ge_1ms;
	uint32_t hold_ge_5ms;
	uint32_t hold_ge_10ms;
	uint32_t hold_ge_50ms;
	uintptr_t wait_max_caller;
	uintptr_t hold_max_caller;
} crypto_prof_stats_t;

enum {
	CRYPTO_PROF_TOTAL = 0,
	CRYPTO_PROF_AES,
	CRYPTO_PROF_CHACHA,
	CRYPTO_PROF_OTHER,
	CRYPTO_PROF_BUCKET_COUNT
};

enum {
	CRYPTO_SIZE_AES = 0,
	CRYPTO_SIZE_CHACHA_COMBINED,
	CRYPTO_SIZE_CHACHA_XOR,
	CRYPTO_SIZE_POLY1305,
	CRYPTO_SIZE_KIND_COUNT
};

enum {
	CRYPTO_SIZE_LT_1K = 0,
	CRYPTO_SIZE_1K_TO_4K,
	CRYPTO_SIZE_4K_TO_16K,
	CRYPTO_SIZE_16K_TO_64K,
	CRYPTO_SIZE_GE_64K,
	CRYPTO_SIZE_BIN_COUNT
};

typedef struct crypto_size_bin_s {
	uint32_t calls;
	uint64_t bytes;
	uint64_t api_us;
	uint32_t api_us_max;
	uint32_t errors;
	uint32_t transactions;
	uint64_t hold_us;
	uint32_t hold_us_max;
} crypto_size_bin_t;

typedef struct crypto_size_stats_s {
	uint32_t calls;
	uint64_t bytes;
	uint64_t aad_bytes;
	uint32_t len_min;
	uint32_t len_max;
	uint64_t api_us;
	uint32_t api_us_max;
	uint32_t errors;
	crypto_size_bin_t bin[CRYPTO_SIZE_BIN_COUNT];
} crypto_size_stats_t;

typedef struct crypto_phase_stats_s {
	uint32_t transactions;
	uint32_t api_calls;
	uint32_t waits;
	uint32_t irq_seen;
	uint32_t irq_missing;
	uint64_t hold_us;
	uint64_t api_us;
	uint64_t outside_api_us;
	uint64_t pre_wait_us;
	uint64_t hw_to_irq_us;
	uint64_t irq_handler_us;
	uint64_t irq_to_wake_us;
	uint64_t post_wait_us;
	uint32_t hold_us_max;
	uint32_t api_us_max;
	uint32_t outside_api_us_max;
	uint32_t pre_wait_us_max;
	uint32_t hw_to_irq_us_max;
	uint32_t irq_handler_us_max;
	uint32_t irq_to_wake_us_max;
	uint32_t post_wait_us_max;
} crypto_phase_stats_t;

typedef struct crypto_phase_slow_s {
	uint32_t valid;
	uint32_t bucket;
	uint32_t payload_bytes;
	uint32_t payload_max;
	uint32_t api_calls;
	uint32_t hold_us;
	uint32_t api_us;
	uint32_t outside_api_us;
	uint32_t pre_wait_us;
	uint32_t hw_to_irq_us;
	uint32_t irq_handler_us;
	uint32_t irq_to_wake_us;
	uint32_t irq_to_wake_us_max;
	uint32_t post_wait_us;
	uint32_t waits;
	uint32_t irq_missing;
	uint32_t owner_priority;
	uint32_t irq_task_priority;
	char owner_task[configMAX_TASK_NAME_LEN];
	char irq_task[configMAX_TASK_NAME_LEN];
} crypto_phase_slow_t;

#define CRYPTO_PHASE_SLOW_COUNT 4U

/*
 * These are linker symbols for public functions in the same object as the
 * internal lock callers. Declaring byte aliases lets the profiler classify a
 * return PC without changing or rebuilding the vendor-facing API.
 */
extern const uint8_t crypto_prof_aes_anchor[] __asm__("AES_CTR_Init");
extern const uint8_t crypto_prof_chacha_begin[]
	__asm__("chacha_rtl8195b_precheck_context");
extern const uint8_t crypto_prof_chacha_end[]
	__asm__("chacha_rtl8195b_status_string");
extern const uint8_t crypto_prof_aes_lock[]
	__asm__("carbox_crypto_aes_device_lock");
extern const uint8_t crypto_prof_chacha_lock[]
	__asm__("carbox_crypto_chacha_device_lock");

static const char *const crypto_prof_bucket_name[CRYPTO_PROF_BUCKET_COUNT] = {
	"TOTAL", "AES", "CHACHA", "OTHER"
};

static const char *const crypto_size_kind_name[CRYPTO_SIZE_KIND_COUNT] = {
	"AES", "CHACHA_COMBINED", "CHACHA_XOR", "POLY1305"
};

static crypto_prof_stats_t crypto_prof_live[CRYPTO_PROF_BUCKET_COUNT];
static crypto_prof_stats_t crypto_prof_snapshot[CRYPTO_PROF_BUCKET_COUNT];
static crypto_size_stats_t crypto_size_live[CRYPTO_SIZE_KIND_COUNT];
static crypto_size_stats_t crypto_size_snapshot[CRYPTO_SIZE_KIND_COUNT];
static crypto_phase_stats_t crypto_phase_live[CRYPTO_PROF_BUCKET_COUNT];
static crypto_phase_stats_t crypto_phase_snapshot[CRYPTO_PROF_BUCKET_COUNT];
static crypto_phase_slow_t crypto_phase_slow_live[CRYPTO_PHASE_SLOW_COUNT];
static crypto_phase_slow_t crypto_phase_slow_snapshot[CRYPTO_PHASE_SLOW_COUNT];
static uint32_t crypto_prof_owner_start_us;
static uintptr_t crypto_prof_owner_caller;
static uint32_t crypto_prof_owner_bucket;
static uint32_t crypto_prof_owner_valid;
static uint32_t crypto_prof_owner_size_kind;
static uint32_t crypto_prof_owner_size_bin;
static uint32_t crypto_prof_owner_size_valid;
static uint32_t crypto_prof_owner_payload_bytes;
static uint32_t crypto_prof_owner_payload_max;
static uint32_t crypto_prof_owner_api_calls;
static uint32_t crypto_prof_owner_api_total_us;
static uint32_t crypto_prof_owner_pre_wait_us;
static uint32_t crypto_prof_owner_hw_to_irq_us;
static uint32_t crypto_prof_owner_irq_handler_us;
static uint32_t crypto_prof_owner_irq_to_wake_us;
static uint32_t crypto_prof_owner_post_wait_us;
static uint32_t crypto_prof_owner_waits;
static uint32_t crypto_prof_owner_irq_missing;
static uint32_t crypto_prof_api_start_us;
static uint32_t crypto_prof_wait_start_us;
static uint32_t crypto_prof_wait_end_us;
static volatile uint32_t crypto_prof_irq_us;
static volatile uint32_t crypto_prof_api_active;
static volatile uint32_t crypto_prof_wait_active;
static volatile uint32_t crypto_prof_irq_valid;
static TaskHandle_t crypto_prof_owner_task;
static uint32_t crypto_prof_owner_priority;
static volatile TaskHandle_t crypto_prof_irq_task;
static volatile uint32_t crypto_prof_irq_task_priority;
static TaskHandle_t crypto_prof_owner_worst_irq_task;
static uint32_t crypto_prof_owner_worst_irq_priority;
static uint32_t crypto_prof_owner_worst_irq_delay_us;
static volatile uint32_t crypto_prof_irq_signal_us;
static volatile uint32_t crypto_prof_irq_handler_ready;

extern void __real_device_mutex_lock(RT_DEV_LOCK_E device);
extern void __real_device_mutex_unlock(RT_DEV_LOCK_E device);

static uint32_t crypto_prof_classify(uintptr_t caller)
{
	uintptr_t pc = caller & ~(uintptr_t)1U;
	uintptr_t aes_anchor = (uintptr_t)crypto_prof_aes_anchor;
	uintptr_t chacha_begin = (uintptr_t)crypto_prof_chacha_begin;
	uintptr_t chacha_end = (uintptr_t)crypto_prof_chacha_end;
	uintptr_t aes_lock = (uintptr_t)crypto_prof_aes_lock;
	uintptr_t chacha_lock = (uintptr_t)crypto_prof_chacha_lock;

	/* Priority-aware API entries are the immediate callers of the real lock. */
	if ((pc >= aes_lock) && (pc < (aes_lock + 0x40U))) {
		return CRYPTO_PROF_AES;
	}
	if ((pc >= chacha_lock) && (pc < (chacha_lock + 0x40U))) {
		return CRYPTO_PROF_CHACHA;
	}

	/* _AES_RTL_Begin precedes the exported AES_CTR_Init in AESUtils.o. */
	if ((pc < (aes_anchor + 0x100U)) &&
	    ((pc + 0x1000U) >= aes_anchor)) {
		return CRYPTO_PROF_AES;
	}

	/* The shared ChaCha runner precedes the first exported backend API. */
	if ((pc + 0x200U >= chacha_begin) &&
	    (pc < (chacha_end + 0x100U))) {
		return CRYPTO_PROF_CHACHA;
	}

	return CRYPTO_PROF_OTHER;
}

static void crypto_prof_record_wait(crypto_prof_stats_t *stats,
				    uint32_t wait_us, uintptr_t caller)
{
	stats->lock_calls++;
	stats->wait_us += wait_us;
	if (wait_us >= 10U) stats->wait_ge_10us++;
	if (wait_us >= 100U) stats->wait_ge_100us++;
	if (wait_us >= 1000U) stats->wait_ge_1ms++;
	if (wait_us >= 10000U) stats->wait_ge_10ms++;
	if (wait_us > stats->wait_us_max) {
		stats->wait_us_max = wait_us;
		stats->wait_max_caller = caller;
	}
}

static void crypto_prof_record_hold(crypto_prof_stats_t *stats,
				    uint32_t hold_us, uintptr_t caller)
{
	stats->completed_calls++;
	stats->hold_us += hold_us;
	if (hold_us >= 1000U) stats->hold_ge_1ms++;
	if (hold_us >= 5000U) stats->hold_ge_5ms++;
	if (hold_us >= 10000U) stats->hold_ge_10ms++;
	if (hold_us >= 50000U) stats->hold_ge_50ms++;
	if (hold_us > stats->hold_us_max) {
		stats->hold_us_max = hold_us;
		stats->hold_max_caller = caller;
	}
}

static uint32_t crypto_size_bin_for_len(uint32_t len)
{
	if (len < 1024U) return CRYPTO_SIZE_LT_1K;
	if (len < 4096U) return CRYPTO_SIZE_1K_TO_4K;
	if (len < 16384U) return CRYPTO_SIZE_4K_TO_16K;
	if (len < 65536U) return CRYPTO_SIZE_16K_TO_64K;
	return CRYPTO_SIZE_GE_64K;
}

/*
 * Called by wrappers around the HAL data APIs while RT_DEV_LOCK_CRYPTO is
 * held.  Measuring here reports the actual hardware transaction size, not an
 * outer CarPlay frame size that may have been split into <=64 KiB chunks.
 */
static void crypto_size_record(uint32_t kind, uint32_t len, uint32_t aad_len,
			       uint32_t api_us, int result)
{
	crypto_size_stats_t *stats;
	crypto_size_bin_t *bin;
	uint32_t bin_index;

	if (kind >= CRYPTO_SIZE_KIND_COUNT) return;
	stats = &crypto_size_live[kind];
	bin_index = crypto_size_bin_for_len(len);
	bin = &stats->bin[bin_index];

	stats->calls++;
	stats->bytes += len;
	stats->aad_bytes += aad_len;
	stats->api_us += api_us;
	if ((stats->calls == 1U) || (len < stats->len_min)) stats->len_min = len;
	if (len > stats->len_max) stats->len_max = len;
	if (api_us > stats->api_us_max) stats->api_us_max = api_us;
	if (result != 0) stats->errors++;

	bin->calls++;
	bin->bytes += len;
	bin->api_us += api_us;
	if (api_us > bin->api_us_max) bin->api_us_max = api_us;
	if (result != 0) bin->errors++;

	/* The last data operation identifies the transaction at unlock. */
	if (crypto_prof_owner_valid) {
		crypto_prof_owner_size_kind = kind;
		crypto_prof_owner_size_bin = bin_index;
		crypto_prof_owner_size_valid = 1U;
	}
}

static uint32_t crypto_api_begin(uint32_t len)
{
	uint32_t now = hal_read_curtime_us();

	if (crypto_prof_owner_valid) {
		crypto_prof_api_start_us = now;
		crypto_prof_owner_api_calls++;
		crypto_prof_owner_payload_bytes += len;
		if (len > crypto_prof_owner_payload_max) {
			crypto_prof_owner_payload_max = len;
		}
		crypto_prof_wait_active = 0U;
		crypto_prof_irq_valid = 0U;
		crypto_prof_irq_handler_ready = 0U;
		crypto_prof_irq_signal_us = 0U;
		crypto_prof_wait_start_us = 0U;
		crypto_prof_wait_end_us = 0U;
		crypto_prof_api_active = 1U;
	}
	return now;
}

static uint32_t crypto_api_end(uint32_t start_us)
{
	uint32_t end_us = hal_read_curtime_us();
	uint32_t elapsed_us = end_us - start_us;

	if (crypto_prof_owner_valid && crypto_prof_api_active) {
		crypto_prof_owner_api_total_us += elapsed_us;
		if (crypto_prof_wait_end_us != 0U) {
			crypto_prof_owner_post_wait_us +=
				end_us - crypto_prof_wait_end_us;
		}
		crypto_prof_api_active = 0U;
		crypto_prof_wait_active = 0U;
		crypto_prof_irq_valid = 0U;
	}
	return elapsed_us;
}

uint32_t crypto_engine_profiler_chacha_combined_begin(uint32_t message_len)
{
	return crypto_api_begin(message_len);
}

void crypto_engine_profiler_chacha_combined_end(uint32_t start_us,
						 uint32_t message_len,
						 uint32_t aad_len, int result)
{
	crypto_size_record(CRYPTO_SIZE_CHACHA_COMBINED, message_len, aad_len,
		crypto_api_end(start_us), result);
}

static void crypto_phase_insert_slow(const crypto_phase_slow_t *sample)
{
	uint32_t position;
	uint32_t i;

	for (position = 0U; position < CRYPTO_PHASE_SLOW_COUNT; ++position) {
		if (!crypto_phase_slow_live[position].valid ||
		    (sample->hold_us > crypto_phase_slow_live[position].hold_us)) {
			for (i = CRYPTO_PHASE_SLOW_COUNT - 1U; i > position; --i) {
				crypto_phase_slow_live[i] = crypto_phase_slow_live[i - 1U];
			}
			crypto_phase_slow_live[position] = *sample;
			return;
		}
	}
}

static void crypto_phase_record_transaction(uint32_t hold_us)
{
	crypto_phase_stats_t *stats;
	crypto_phase_slow_t sample;
	uint32_t outside_api_us;

	if ((crypto_prof_owner_bucket != CRYPTO_PROF_AES) &&
	    (crypto_prof_owner_bucket != CRYPTO_PROF_CHACHA)) return;
	stats = &crypto_phase_live[crypto_prof_owner_bucket];
	outside_api_us = hold_us >= crypto_prof_owner_api_total_us ?
		hold_us - crypto_prof_owner_api_total_us : 0U;

	stats->transactions++;
	stats->api_calls += crypto_prof_owner_api_calls;
	stats->waits += crypto_prof_owner_waits;
	stats->irq_seen += crypto_prof_owner_waits - crypto_prof_owner_irq_missing;
	stats->irq_missing += crypto_prof_owner_irq_missing;
	stats->hold_us += hold_us;
	stats->api_us += crypto_prof_owner_api_total_us;
	stats->outside_api_us += outside_api_us;
	stats->pre_wait_us += crypto_prof_owner_pre_wait_us;
	stats->hw_to_irq_us += crypto_prof_owner_hw_to_irq_us;
	stats->irq_handler_us += crypto_prof_owner_irq_handler_us;
	stats->irq_to_wake_us += crypto_prof_owner_irq_to_wake_us;
	stats->post_wait_us += crypto_prof_owner_post_wait_us;
	if (hold_us > stats->hold_us_max) stats->hold_us_max = hold_us;
	if (crypto_prof_owner_api_total_us > stats->api_us_max)
		stats->api_us_max = crypto_prof_owner_api_total_us;
	if (outside_api_us > stats->outside_api_us_max)
		stats->outside_api_us_max = outside_api_us;
	if (crypto_prof_owner_pre_wait_us > stats->pre_wait_us_max)
		stats->pre_wait_us_max = crypto_prof_owner_pre_wait_us;
	if (crypto_prof_owner_hw_to_irq_us > stats->hw_to_irq_us_max)
		stats->hw_to_irq_us_max = crypto_prof_owner_hw_to_irq_us;
	if (crypto_prof_owner_irq_handler_us > stats->irq_handler_us_max)
		stats->irq_handler_us_max = crypto_prof_owner_irq_handler_us;
	if (crypto_prof_owner_irq_to_wake_us > stats->irq_to_wake_us_max)
		stats->irq_to_wake_us_max = crypto_prof_owner_irq_to_wake_us;
	if (crypto_prof_owner_post_wait_us > stats->post_wait_us_max)
		stats->post_wait_us_max = crypto_prof_owner_post_wait_us;

	memset(&sample, 0, sizeof(sample));
	sample.valid = 1U;
	sample.bucket = crypto_prof_owner_bucket;
	sample.payload_bytes = crypto_prof_owner_payload_bytes;
	sample.payload_max = crypto_prof_owner_payload_max;
	sample.api_calls = crypto_prof_owner_api_calls;
	sample.hold_us = hold_us;
	sample.api_us = crypto_prof_owner_api_total_us;
	sample.outside_api_us = outside_api_us;
	sample.pre_wait_us = crypto_prof_owner_pre_wait_us;
	sample.hw_to_irq_us = crypto_prof_owner_hw_to_irq_us;
	sample.irq_handler_us = crypto_prof_owner_irq_handler_us;
	sample.irq_to_wake_us = crypto_prof_owner_irq_to_wake_us;
	sample.irq_to_wake_us_max = crypto_prof_owner_worst_irq_delay_us;
	sample.post_wait_us = crypto_prof_owner_post_wait_us;
	sample.waits = crypto_prof_owner_waits;
	sample.irq_missing = crypto_prof_owner_irq_missing;
	sample.owner_priority = crypto_prof_owner_priority;
	sample.irq_task_priority = crypto_prof_owner_worst_irq_priority;
	if (crypto_prof_owner_task != NULL) {
		strncpy(sample.owner_task, pcTaskGetName(crypto_prof_owner_task),
			sizeof(sample.owner_task) - 1U);
	}
	if (crypto_prof_owner_worst_irq_task != NULL) {
		strncpy(sample.irq_task,
			pcTaskGetName(crypto_prof_owner_worst_irq_task),
			sizeof(sample.irq_task) - 1U);
	}
	crypto_phase_insert_slow(&sample);
}

void __wrap_device_mutex_lock(RT_DEV_LOCK_E device)
{
	uint32_t start_us;
	uint32_t acquired_us;
	uint32_t wait_us;
	uint32_t bucket;
	uintptr_t caller;

	if (device != RT_DEV_LOCK_CRYPTO) {
		__real_device_mutex_lock(device);
		return;
	}

	caller = (uintptr_t)__builtin_return_address(0);
	start_us = hal_read_curtime_us();
	__real_device_mutex_lock(device);
	acquired_us = hal_read_curtime_us();
	wait_us = acquired_us - start_us;
	switch (carbox_crypto_priority_current_kind()) {
	case CARBOX_CRYPTO_KIND_AES:
		bucket = CRYPTO_PROF_AES;
		break;
	case CARBOX_CRYPTO_KIND_CHACHA:
		bucket = CRYPTO_PROF_CHACHA;
		break;
	default:
		bucket = crypto_prof_classify(caller);
		break;
	}

	/* The crypto mutex serializes writers to these counters. */
	crypto_prof_record_wait(&crypto_prof_live[CRYPTO_PROF_TOTAL],
		wait_us, caller);
	crypto_prof_record_wait(&crypto_prof_live[bucket], wait_us, caller);

	crypto_prof_owner_start_us = acquired_us;
	crypto_prof_owner_caller = caller;
	crypto_prof_owner_bucket = bucket;
	crypto_prof_owner_valid = 1U;
	crypto_prof_owner_size_valid = 0U;
	crypto_prof_owner_task = xTaskGetCurrentTaskHandle();
	crypto_prof_owner_priority = uxTaskPriorityGet(crypto_prof_owner_task);
	crypto_prof_owner_payload_bytes = 0U;
	crypto_prof_owner_payload_max = 0U;
	crypto_prof_owner_api_calls = 0U;
	crypto_prof_owner_api_total_us = 0U;
	crypto_prof_owner_pre_wait_us = 0U;
	crypto_prof_owner_hw_to_irq_us = 0U;
	crypto_prof_owner_irq_handler_us = 0U;
	crypto_prof_owner_irq_to_wake_us = 0U;
	crypto_prof_owner_post_wait_us = 0U;
	crypto_prof_owner_waits = 0U;
	crypto_prof_owner_irq_missing = 0U;
	crypto_prof_api_start_us = 0U;
	crypto_prof_wait_start_us = 0U;
	crypto_prof_wait_end_us = 0U;
	crypto_prof_api_active = 0U;
	crypto_prof_wait_active = 0U;
	crypto_prof_irq_valid = 0U;
	crypto_prof_irq_signal_us = 0U;
	crypto_prof_irq_handler_ready = 0U;
	crypto_prof_irq_task = NULL;
	crypto_prof_irq_task_priority = 0U;
	crypto_prof_owner_worst_irq_task = NULL;
	crypto_prof_owner_worst_irq_priority = 0U;
	crypto_prof_owner_worst_irq_delay_us = 0U;
}

void __wrap_device_mutex_unlock(RT_DEV_LOCK_E device)
{
	if (device == RT_DEV_LOCK_CRYPTO && crypto_prof_owner_valid) {
		uint32_t hold_us =
			hal_read_curtime_us() - crypto_prof_owner_start_us;

		crypto_prof_record_hold(&crypto_prof_live[CRYPTO_PROF_TOTAL],
			hold_us, crypto_prof_owner_caller);
		crypto_prof_record_hold(
			&crypto_prof_live[crypto_prof_owner_bucket],
			hold_us, crypto_prof_owner_caller);
		crypto_phase_record_transaction(hold_us);
		if (crypto_prof_owner_size_valid &&
		    (crypto_prof_owner_size_kind < CRYPTO_SIZE_KIND_COUNT) &&
		    (crypto_prof_owner_size_bin < CRYPTO_SIZE_BIN_COUNT)) {
			crypto_size_bin_t *bin =
				&crypto_size_live[crypto_prof_owner_size_kind]
					.bin[crypto_prof_owner_size_bin];

			bin->transactions++;
			bin->hold_us += hold_us;
			if (hold_us > bin->hold_us_max) bin->hold_us_max = hold_us;
		}
		crypto_prof_owner_size_valid = 0U;
		crypto_prof_owner_valid = 0U;
	}

	__real_device_mutex_unlock(device);
}

/*
 * The RTL completion bridge blocks in rtw_down_timeout_sema() after the DMA
 * request has been submitted.  g_crypto_handler() runs at crypto IRQ entry.
 * Wrapping both points separates hardware/IRQ latency from scheduler wakeup
 * latency without changing the customer's AES or ChaCha archive.
 */
extern uint32_t __real_rtw_down_timeout_sema(void *sema, uint32_t timeout);
extern void __real_rtw_up_sema_from_isr(void *sema);
extern void __real_g_crypto_handler(int crypto_done, int crc_done);

uint32_t __wrap_rtw_down_timeout_sema(void *sema, uint32_t timeout)
{
	uint32_t start_us;
	uint32_t end_us;
	uint32_t result;
	uint32_t irq_us;

	if (!crypto_prof_owner_valid || !crypto_prof_api_active ||
	    (timeout == 0U)) {
		return __real_rtw_down_timeout_sema(sema, timeout);
	}

	start_us = hal_read_curtime_us();
	crypto_prof_wait_start_us = start_us;
	crypto_prof_wait_end_us = 0U;
	crypto_prof_irq_valid = 0U;
	crypto_prof_irq_handler_ready = 0U;
	crypto_prof_irq_signal_us = 0U;
	crypto_prof_wait_active = 1U;
	crypto_prof_owner_pre_wait_us += start_us - crypto_prof_api_start_us;
	result = __real_rtw_down_timeout_sema(sema, timeout);
	end_us = hal_read_curtime_us();
	crypto_prof_wait_end_us = end_us;
	crypto_prof_owner_waits++;

	irq_us = crypto_prof_irq_us;
	if (crypto_prof_irq_valid &&
	    ((uint32_t)(irq_us - start_us) <= (uint32_t)(end_us - start_us))) {
		uint32_t signal_us = crypto_prof_irq_signal_us;
		uint32_t wake_us = end_us - signal_us;

		crypto_prof_owner_hw_to_irq_us += irq_us - start_us;
		crypto_prof_owner_irq_to_wake_us += wake_us;
		if (wake_us > crypto_prof_owner_worst_irq_delay_us) {
			crypto_prof_owner_worst_irq_delay_us = wake_us;
			crypto_prof_owner_worst_irq_task = crypto_prof_irq_task;
			crypto_prof_owner_worst_irq_priority =
				crypto_prof_irq_task_priority;
		}
	} else {
		crypto_prof_owner_irq_missing++;
	}
	crypto_prof_wait_active = 0U;
	return result;
}

void __wrap_g_crypto_handler(int crypto_done, int crc_done)
{
	uint32_t start_us = 0U;
	uint32_t end_us;

	if (crypto_prof_owner_valid && crypto_prof_api_active &&
	    crypto_prof_wait_active && (crypto_done > 0)) {
		start_us = hal_read_curtime_us();
		crypto_prof_irq_us = start_us;
		crypto_prof_irq_task = xTaskGetCurrentTaskHandle();
		crypto_prof_irq_task_priority =
			uxTaskPriorityGetFromISR(crypto_prof_irq_task);
	}
	__real_g_crypto_handler(crypto_done, crc_done);
	if (start_us != 0U) {
		end_us = hal_read_curtime_us();
		crypto_prof_owner_irq_handler_us += end_us - start_us;
		crypto_prof_irq_handler_ready = 1U;
	}
}

void __wrap_rtw_up_sema_from_isr(void *sema)
{
	if (crypto_prof_owner_valid && crypto_prof_api_active &&
	    crypto_prof_wait_active && crypto_prof_irq_handler_ready) {
		crypto_prof_irq_signal_us = hal_read_curtime_us();
		crypto_prof_irq_valid = 1U;
		crypto_prof_irq_handler_ready = 0U;
	}
	__real_rtw_up_sema_from_isr(sema);
}

/* AES HAL data APIs used by AESUtils.o. */
#define CRYPTO_WRAP_AES_MODE(mode) \
extern int __real_crypto_aes_##mode##_encrypt(const uint8_t *, uint32_t, \
	const uint8_t *, uint32_t, uint8_t *); \
extern int __real_crypto_aes_##mode##_decrypt(const uint8_t *, uint32_t, \
	const uint8_t *, uint32_t, uint8_t *); \
int __wrap_crypto_aes_##mode##_encrypt(const uint8_t *message, uint32_t msglen, \
	const uint8_t *iv, uint32_t ivlen, uint8_t *result) \
{ \
	uint32_t start_us = crypto_api_begin(msglen); \
	int err = __real_crypto_aes_##mode##_encrypt(message, msglen, iv, ivlen, result); \
	crypto_size_record(CRYPTO_SIZE_AES, msglen, 0U, \
		crypto_api_end(start_us), err); \
	return err; \
} \
int __wrap_crypto_aes_##mode##_decrypt(const uint8_t *message, uint32_t msglen, \
	const uint8_t *iv, uint32_t ivlen, uint8_t *result) \
{ \
	uint32_t start_us = crypto_api_begin(msglen); \
	int err = __real_crypto_aes_##mode##_decrypt(message, msglen, iv, ivlen, result); \
	crypto_size_record(CRYPTO_SIZE_AES, msglen, 0U, \
		crypto_api_end(start_us), err); \
	return err; \
}

CRYPTO_WRAP_AES_MODE(ecb)
CRYPTO_WRAP_AES_MODE(cbc)
CRYPTO_WRAP_AES_MODE(ctr)

extern int __real_crypto_aes_gcm_encrypt(const uint8_t *, uint32_t,
	const uint8_t *, const uint8_t *, uint32_t, uint8_t *, uint8_t *);
extern int __real_crypto_aes_gcm_decrypt(const uint8_t *, uint32_t,
	const uint8_t *, const uint8_t *, uint32_t, uint8_t *, uint8_t *);

#define CRYPTO_WRAP_AES_GCM(direction) \
int __wrap_crypto_aes_gcm_##direction(const uint8_t *message, uint32_t msglen, \
	const uint8_t *iv, const uint8_t *aad, uint32_t aadlen, \
	uint8_t *result, uint8_t *tag) \
{ \
	uint32_t start_us = crypto_api_begin(msglen); \
	int err = __real_crypto_aes_gcm_##direction(message, msglen, iv, aad, \
		aadlen, result, tag); \
	crypto_size_record(CRYPTO_SIZE_AES, msglen, aadlen, \
		crypto_api_end(start_us), err); \
	return err; \
}

CRYPTO_WRAP_AES_GCM(encrypt)
CRYPTO_WRAP_AES_GCM(decrypt)

/* RTL8195B ChaCha/Poly1305 data APIs used by the customer backend. */
extern int __real_rtl_crypto_chacha_encrypt(const uint8_t *, uint32_t,
	const uint8_t *, uint32_t, uint8_t *);
extern int __real_rtl_crypto_chacha_decrypt(const uint8_t *, uint32_t,
	const uint8_t *, uint32_t, uint8_t *);

#define CRYPTO_WRAP_CHACHA_XOR(direction) \
int __wrap_rtl_crypto_chacha_##direction(const uint8_t *message, uint32_t msglen, \
	const uint8_t *iv, uint32_t count, uint8_t *result) \
{ \
	uint32_t start_us = crypto_api_begin(msglen); \
	int err = __real_rtl_crypto_chacha_##direction(message, msglen, iv, count, result); \
	crypto_size_record(CRYPTO_SIZE_CHACHA_XOR, msglen, 0U, \
		crypto_api_end(start_us), err); \
	return err; \
}

CRYPTO_WRAP_CHACHA_XOR(encrypt)
CRYPTO_WRAP_CHACHA_XOR(decrypt)

extern int __real_rtl_crypto_poly1305(const uint8_t *, uint32_t,
	const uint8_t *, uint8_t *);
extern int __real_rtl_crypto_poly1305_process(const uint8_t *, uint32_t,
	uint8_t *);

int __wrap_rtl_crypto_poly1305(const uint8_t *message, uint32_t msglen,
	const uint8_t *key, uint8_t *digest)
{
	uint32_t start_us = crypto_api_begin(msglen);
	int err = __real_rtl_crypto_poly1305(message, msglen, key, digest);
	crypto_size_record(CRYPTO_SIZE_POLY1305, msglen, 0U,
		crypto_api_end(start_us), err);
	return err;
}

int __wrap_rtl_crypto_poly1305_process(const uint8_t *message, uint32_t msglen,
	uint8_t *digest)
{
	uint32_t start_us = crypto_api_begin(msglen);
	int err = __real_rtl_crypto_poly1305_process(message, msglen, digest);
	crypto_size_record(CRYPTO_SIZE_POLY1305, msglen, 0U,
		crypto_api_end(start_us), err);
	return err;
}

extern int __real_rtl_crypto_chacha_poly1305_encrypt(const uint8_t *, uint32_t,
	const uint8_t *, const uint8_t *, uint32_t, uint8_t *, uint8_t *);
extern int __real_rtl_crypto_chacha_poly1305_decrypt(const uint8_t *, uint32_t,
	const uint8_t *, const uint8_t *, uint32_t, uint8_t *, uint8_t *);

#define CRYPTO_WRAP_CHACHA_COMBINED(direction) \
int __wrap_rtl_crypto_chacha_poly1305_##direction(const uint8_t *message, \
	uint32_t msglen, const uint8_t *nonce, const uint8_t *aad, \
	uint32_t aadlen, uint8_t *result, uint8_t *tag) \
{ \
	uint32_t start_us = crypto_api_begin(msglen); \
	int err = __real_rtl_crypto_chacha_poly1305_##direction(message, msglen, \
		nonce, aad, aadlen, result, tag); \
	crypto_size_record(CRYPTO_SIZE_CHACHA_COMBINED, msglen, aadlen, \
		crypto_api_end(start_us), err); \
	return err; \
}

CRYPTO_WRAP_CHACHA_COMBINED(encrypt)
CRYPTO_WRAP_CHACHA_COMBINED(decrypt)

void crypto_engine_profiler_report(uint32_t sequence)
{
	crypto_prof_stats_t *total;
	uint32_t wait_avg = 0U;
	uint32_t hold_avg = 0U;
	uint32_t busy_x10;
	uint32_t i;

	/* Snapshot on the existing PC profiler's 10-second report cadence. */
	taskENTER_CRITICAL();
	memcpy(crypto_prof_snapshot, crypto_prof_live,
		sizeof(crypto_prof_snapshot));
	memset(&crypto_prof_live, 0, sizeof(crypto_prof_live));
	memcpy(crypto_size_snapshot, crypto_size_live,
		sizeof(crypto_size_snapshot));
	memset(&crypto_size_live, 0, sizeof(crypto_size_live));
	memcpy(crypto_phase_snapshot, crypto_phase_live,
		sizeof(crypto_phase_snapshot));
	memset(&crypto_phase_live, 0, sizeof(crypto_phase_live));
	memcpy(crypto_phase_slow_snapshot, crypto_phase_slow_live,
		sizeof(crypto_phase_slow_snapshot));
	memset(&crypto_phase_slow_live, 0, sizeof(crypto_phase_slow_live));
	taskEXIT_CRITICAL();
	total = &crypto_prof_snapshot[CRYPTO_PROF_TOTAL];

	if (total->lock_calls != 0U) {
		wait_avg = (uint32_t)(total->wait_us / total->lock_calls);
	}
	if (total->completed_calls != 0U) {
		hold_avg = (uint32_t)(total->hold_us / total->completed_calls);
	}

	busy_x10 = (uint32_t)((total->hold_us * 1000ULL) /
		10000000ULL);
	if (busy_x10 > 1000U) busy_x10 = 1000U;

	rt_printf("[CRYPTOPROF][%lu] window_ms=10000 lock/completed=%lu/%lu "
		  "wait_us total/avg/max=%llu/%lu/%lu "
		  "ge10/100/1000/10000us=%lu/%lu/%lu/%lu "
		  "max_caller=0x%08lx\r\n",
		  (unsigned long)sequence,
		  (unsigned long)total->lock_calls,
		  (unsigned long)total->completed_calls,
		  (unsigned long long)total->wait_us,
		  (unsigned long)wait_avg,
		  (unsigned long)total->wait_us_max,
		  (unsigned long)total->wait_ge_10us,
		  (unsigned long)total->wait_ge_100us,
		  (unsigned long)total->wait_ge_1ms,
		  (unsigned long)total->wait_ge_10ms,
		  (unsigned long)total->wait_max_caller);
	rt_printf("[CRYPTOPROF][%lu] hold_us total/avg/max=%llu/%lu/%lu "
		  "ge1/5/10/50ms=%lu/%lu/%lu/%lu busy=%lu.%lu%% "
		  "max_caller=0x%08lx active=%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long long)total->hold_us,
		  (unsigned long)hold_avg,
		  (unsigned long)total->hold_us_max,
		  (unsigned long)total->hold_ge_1ms,
		  (unsigned long)total->hold_ge_5ms,
		  (unsigned long)total->hold_ge_10ms,
		  (unsigned long)total->hold_ge_50ms,
		  (unsigned long)(busy_x10 / 10U),
		  (unsigned long)(busy_x10 % 10U),
		  (unsigned long)total->hold_max_caller,
		  (unsigned long)crypto_prof_owner_valid);

	for (i = CRYPTO_PROF_AES; i < CRYPTO_PROF_BUCKET_COUNT; ++i) {
		crypto_prof_stats_t *stats = &crypto_prof_snapshot[i];
		uint32_t bucket_wait_avg = stats->lock_calls != 0U ?
			(uint32_t)(stats->wait_us / stats->lock_calls) : 0U;
		uint32_t bucket_hold_avg = stats->completed_calls != 0U ?
			(uint32_t)(stats->hold_us / stats->completed_calls) : 0U;
		uint32_t bucket_busy_x10 =
			(uint32_t)((stats->hold_us * 1000ULL) / 10000000ULL);

		if (bucket_busy_x10 > 1000U) bucket_busy_x10 = 1000U;
		rt_printf("[CRYPTOPROF][%lu][%s] ops=%lu/%lu "
			  "wait_us total/avg/max=%llu/%lu/%lu ge100/1000=%lu/%lu "
			  "hold_us total/avg/max=%llu/%lu/%lu ge1/10ms=%lu/%lu "
			  "busy=%lu.%lu%% caller=0x%08lx\r\n",
			  (unsigned long)sequence,
			  crypto_prof_bucket_name[i],
			  (unsigned long)stats->lock_calls,
			  (unsigned long)stats->completed_calls,
			  (unsigned long long)stats->wait_us,
			  (unsigned long)bucket_wait_avg,
			  (unsigned long)stats->wait_us_max,
			  (unsigned long)stats->wait_ge_100us,
			  (unsigned long)stats->wait_ge_1ms,
			  (unsigned long long)stats->hold_us,
			  (unsigned long)bucket_hold_avg,
			  (unsigned long)stats->hold_us_max,
			  (unsigned long)stats->hold_ge_1ms,
			  (unsigned long)stats->hold_ge_10ms,
			  (unsigned long)(bucket_busy_x10 / 10U),
			  (unsigned long)(bucket_busy_x10 % 10U),
			  (unsigned long)stats->hold_max_caller);
	}

	for (i = 0U; i < CRYPTO_SIZE_KIND_COUNT; ++i) {
		crypto_size_stats_t *stats = &crypto_size_snapshot[i];
		uint32_t api_avg = stats->calls != 0U ?
			(uint32_t)(stats->api_us / stats->calls) : 0U;
		uint32_t len_avg = stats->calls != 0U ?
			(uint32_t)(stats->bytes / stats->calls) : 0U;
		uint32_t b;

		rt_printf("[CRYPTOSIZE][%lu][%s] calls=%lu bytes=%llu aad=%llu "
			  "len_avg/min/max=%lu/%lu/%lu api_us total/avg/max=%llu/%lu/%lu "
			  "errors=%lu\r\n",
			  (unsigned long)sequence, crypto_size_kind_name[i],
			  (unsigned long)stats->calls,
			  (unsigned long long)stats->bytes,
			  (unsigned long long)stats->aad_bytes,
			  (unsigned long)len_avg,
			  (unsigned long)(stats->calls ? stats->len_min : 0U),
			  (unsigned long)stats->len_max,
			  (unsigned long long)stats->api_us,
			  (unsigned long)api_avg,
			  (unsigned long)stats->api_us_max,
			  (unsigned long)stats->errors);
		rt_printf("[CRYPTOSIZE][%lu][%s] bins=<1K/1-4K/4-16K/16-64K/>=64K "
			  "calls=", (unsigned long)sequence,
			  crypto_size_kind_name[i]);
		for (b = 0U; b < CRYPTO_SIZE_BIN_COUNT; ++b) {
			rt_printf("%s%lu", b ? "/" : "",
				(unsigned long)stats->bin[b].calls);
		}
		rt_printf(" bytes=");
		for (b = 0U; b < CRYPTO_SIZE_BIN_COUNT; ++b) {
			rt_printf("%s%llu", b ? "/" : "",
				(unsigned long long)stats->bin[b].bytes);
		}
		rt_printf("\r\n");
		if (stats->calls == 0U) continue;

		rt_printf("[CRYPTOSIZE][%lu][%s] bins api_avg:max_us=",
			  (unsigned long)sequence, crypto_size_kind_name[i]);
		for (b = 0U; b < CRYPTO_SIZE_BIN_COUNT; ++b) {
			uint32_t avg = stats->bin[b].calls ?
				(uint32_t)(stats->bin[b].api_us /
					stats->bin[b].calls) : 0U;
			rt_printf("%s%lu:%lu", b ? "/" : "",
				(unsigned long)avg,
				(unsigned long)stats->bin[b].api_us_max);
		}
		rt_printf(" hold_txn:avg:max_us=");
		for (b = 0U; b < CRYPTO_SIZE_BIN_COUNT; ++b) {
			uint32_t hold_avg = stats->bin[b].transactions ?
				(uint32_t)(stats->bin[b].hold_us /
					stats->bin[b].transactions) : 0U;
			rt_printf("%s%lu:%lu:%lu", b ? "/" : "",
				(unsigned long)stats->bin[b].transactions,
				(unsigned long)hold_avg,
				(unsigned long)stats->bin[b].hold_us_max);
		}
		rt_printf("\r\n");
	}

	for (i = CRYPTO_PROF_AES; i <= CRYPTO_PROF_CHACHA; ++i) {
		crypto_phase_stats_t *phase = &crypto_phase_snapshot[i];
		uint32_t txn = phase->transactions;
		uint32_t waits = phase->waits;

		rt_printf("[CRYPTOPHASE][%lu][%s] txn=%lu api_calls=%lu waits=%lu "
			  "irq_seen/missing=%lu/%lu hold_us total/avg/max=%llu/%lu/%lu "
			  "api_us total/avg/max=%llu/%lu/%lu outside_us total/avg/max=%llu/%lu/%lu\r\n",
			  (unsigned long)sequence, crypto_prof_bucket_name[i],
			  (unsigned long)txn, (unsigned long)phase->api_calls,
			  (unsigned long)waits, (unsigned long)phase->irq_seen,
			  (unsigned long)phase->irq_missing,
			  (unsigned long long)phase->hold_us,
			  (unsigned long)(txn ? phase->hold_us / txn : 0U),
			  (unsigned long)phase->hold_us_max,
			  (unsigned long long)phase->api_us,
			  (unsigned long)(txn ? phase->api_us / txn : 0U),
			  (unsigned long)phase->api_us_max,
			  (unsigned long long)phase->outside_api_us,
			  (unsigned long)(txn ? phase->outside_api_us / txn : 0U),
			  (unsigned long)phase->outside_api_us_max);
		rt_printf("[CRYPTOPHASE][%lu][%s] phase_us total/avg/max "
			  "pre_wait=%llu/%lu/%lu hw_to_irq=%llu/%lu/%lu "
			  "irq_handler=%llu/%lu/%lu irq_to_wake=%llu/%lu/%lu "
			  "post_wait=%llu/%lu/%lu\r\n",
			  (unsigned long)sequence, crypto_prof_bucket_name[i],
			  (unsigned long long)phase->pre_wait_us,
			  (unsigned long)(waits ? phase->pre_wait_us / waits : 0U),
			  (unsigned long)phase->pre_wait_us_max,
			  (unsigned long long)phase->hw_to_irq_us,
			  (unsigned long)(phase->irq_seen ?
				phase->hw_to_irq_us / phase->irq_seen : 0U),
			  (unsigned long)phase->hw_to_irq_us_max,
			  (unsigned long long)phase->irq_handler_us,
			  (unsigned long)(phase->irq_seen ?
				phase->irq_handler_us / phase->irq_seen : 0U),
			  (unsigned long)phase->irq_handler_us_max,
			  (unsigned long long)phase->irq_to_wake_us,
			  (unsigned long)(phase->irq_seen ?
				phase->irq_to_wake_us / phase->irq_seen : 0U),
			  (unsigned long)phase->irq_to_wake_us_max,
			  (unsigned long long)phase->post_wait_us,
			  (unsigned long)(waits ? phase->post_wait_us / waits : 0U),
			  (unsigned long)phase->post_wait_us_max);
	}

	for (i = 0U; i < CRYPTO_PHASE_SLOW_COUNT; ++i) {
		crypto_phase_slow_t *slow = &crypto_phase_slow_snapshot[i];

		if (!slow->valid) continue;
		rt_printf("[CRYPTOPHASE][%lu][SLOW%lu] kind=%s owner=%s/p%lu "
			  "irq_task=%s/p%lu payload=%luB max_chunk=%luB "
			  "calls=%lu hold/api/outside_us=%lu/%lu/%lu "
			  "pre/hw_irq/handler/irq_wake(max)/post_us=%lu/%lu/%lu/%lu(%lu)/%lu "
			  "waits=%lu irq_missing=%lu\r\n",
			  (unsigned long)sequence, (unsigned long)(i + 1U),
			  crypto_prof_bucket_name[slow->bucket],
			  slow->owner_task[0] ? slow->owner_task : "?",
			  (unsigned long)slow->owner_priority,
			  slow->irq_task[0] ? slow->irq_task : "?",
			  (unsigned long)slow->irq_task_priority,
			  (unsigned long)slow->payload_bytes,
			  (unsigned long)slow->payload_max,
			  (unsigned long)slow->api_calls,
			  (unsigned long)slow->hold_us,
			  (unsigned long)slow->api_us,
			  (unsigned long)slow->outside_api_us,
			  (unsigned long)slow->pre_wait_us,
			  (unsigned long)slow->hw_to_irq_us,
			  (unsigned long)slow->irq_handler_us,
			  (unsigned long)slow->irq_to_wake_us,
			  (unsigned long)slow->irq_to_wake_us_max,
			  (unsigned long)slow->post_wait_us,
			  (unsigned long)slow->waits,
			  (unsigned long)slow->irq_missing);
	}
}

#else

uint32_t crypto_engine_profiler_chacha_combined_begin(uint32_t message_len)
{
	(void)message_len;
	return 0U;
}

void crypto_engine_profiler_chacha_combined_end(uint32_t start_us,
						 uint32_t message_len,
						 uint32_t aad_len, int result)
{
	(void)start_us;
	(void)message_len;
	(void)aad_len;
	(void)result;
}

void crypto_engine_profiler_report(uint32_t sequence)
{
	(void)sequence;
}

#endif /* CONFIG_CRYPTO_ENGINE_PROFILE */
