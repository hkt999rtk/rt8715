#include "crypto_priority_lock.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "osdep_service.h"
#include "hal_crypto.h"
#include "rtl8195bhp_crypto_ctrl.h"

#ifndef CARBOX_CRYPTO_OWNER_BOOST_PRIORITY
#define CARBOX_CRYPTO_OWNER_BOOST_PRIORITY 11
#endif

#if CARBOX_CRYPTO_OWNER_BOOST_PRIORITY >= configMAX_PRIORITIES
#error "CARBOX_CRYPTO_OWNER_BOOST_PRIORITY must be below configMAX_PRIORITIES"
#endif

#define CARBOX_CRYPTO_PRIORITY_SLOTS 8u

#ifndef CARBOX_CRYPTO_IRQ_TIMEOUT_MS
#define CARBOX_CRYPTO_IRQ_TIMEOUT_MS 1000u
#endif

#define CARBOX_CRYPTO_IRQ_STATS \
	__attribute__((section(".lpddr.bss.crypto_irq_stats")))

extern hal_crypto_adapter_t g_rtl_cryptoEngine_s;
extern void g_crypto_handler(int crypto_done, int crc_done);
extern int g_crypto_pre_exec(void *adapter);
extern void rtl_crypto_irq_enable(
	hal_crypto_adapter_t *adapter, void (*handler)(int, int)
);

static _sema crypto_irq_completion;
static volatile unsigned crypto_irq_ready;
static volatile unsigned crypto_irq_active;
static volatile unsigned crypto_irq_installed CARBOX_CRYPTO_IRQ_STATS;
static volatile unsigned crypto_irq_last_timeout CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_generation CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_enable_calls CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_installs CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_rebinds CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_pre_execs CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_waits CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_completions CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_timeouts CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_spurious CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_drained CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_resets CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_vendor_restores CARBOX_CRYPTO_IRQ_STATS;

static int crypto_irq_pre_exec(void *adapter)
{
	uint32_t drained = 0;

	g_crypto_pre_exec(adapter);
	crypto_irq_active = 0u;
	while (rtw_down_timeout_sema(&crypto_irq_completion, 0) == _TRUE) {
		++drained;
	}
	if (drained) __sync_fetch_and_add(&crypto_irq_drained, drained);
	crypto_irq_last_timeout = 0u;
	__sync_fetch_and_add(&crypto_irq_generation, 1u);
	__sync_fetch_and_add(&crypto_irq_pre_execs, 1u);
	__sync_synchronize();
	crypto_irq_active = 1u;
	return 0;
}

static int crypto_irq_wait_done(void *adapter)
{
	hal_crypto_adapter_t *rtl_adapter = (hal_crypto_adapter_t *)adapter;

	if (!rtl_adapter->isIntMode) return -1;
	__sync_fetch_and_add(&crypto_irq_waits, 1u);
	if (rtw_down_timeout_sema(
		&crypto_irq_completion, CARBOX_CRYPTO_IRQ_TIMEOUT_MS
	) == _TRUE) {
		crypto_irq_active = 0u;
		return 0;
	}

	crypto_irq_active = 0u;
	crypto_irq_last_timeout = 1u;
	__sync_fetch_and_add(&crypto_irq_timeouts, 1u);
	printf(
		"[CRYPTOIRQ][TIMEOUT] generation=%lu waited_ms=%lu "
		"irq/completed/spurious=%lu/%lu/%lu\n",
		(unsigned long)crypto_irq_generation,
		(unsigned long)CARBOX_CRYPTO_IRQ_TIMEOUT_MS,
		(unsigned long)crypto_irq_completions,
		(unsigned long)(crypto_irq_waits - crypto_irq_timeouts),
		(unsigned long)crypto_irq_spurious
	);
	/* IRQ-only diagnostic build: do not enter the HAL polling waiter. */
	return -1;
}

static void crypto_irq_handler(int crypto_done, int crc_done)
{
	g_crypto_handler(crypto_done, crc_done);
	if ((crypto_done > 0) && crypto_irq_active) {
		__sync_fetch_and_add(&crypto_irq_completions, 1u);
		rtw_up_sema_from_isr(&crypto_irq_completion);
	} else {
		__sync_fetch_and_add(&crypto_irq_spurious, 1u);
	}
}

int carbox_crypto_irq_controller_enable(void)
{
	int needs_bind;

	__sync_fetch_and_add(&crypto_irq_enable_calls, 1u);
	if (!crypto_irq_ready) {
		rtw_init_sema(&crypto_irq_completion, 0);
		if (!crypto_irq_completion) return -1;
		crypto_irq_ready = 1u;
	}

	needs_bind =
		(g_rtl_cryptoEngine_s.pre_exec_func != crypto_irq_pre_exec) ||
		(g_rtl_cryptoEngine_s.wait_done_func != crypto_irq_wait_done) ||
		!g_rtl_cryptoEngine_s.isIntMode;
	if (needs_bind) {
		if (crypto_irq_installed) {
			__sync_fetch_and_add(&crypto_irq_rebinds, 1u);
			printf("[CRYPTOIRQ] controller ownership lost/reset; rebinding\n");
		}
		g_rtl_cryptoEngine_s.pre_exec_func = crypto_irq_pre_exec;
		g_rtl_cryptoEngine_s.wait_done_func = crypto_irq_wait_done;
		rtl_crypto_irq_enable(&g_rtl_cryptoEngine_s, crypto_irq_handler);
		crypto_irq_installed = 1u;
		__sync_fetch_and_add(&crypto_irq_installs, 1u);
		if (crypto_irq_installs == 1u) {
			printf(
				"[CRYPTOIRQ] unified AES/ChaCha controller installed "
				"(IRQ-only timeout=%lu ms)\n",
				(unsigned long)CARBOX_CRYPTO_IRQ_TIMEOUT_MS
			);
		}
	}
	return 0;
}

/*
 * AESUtils.o is retained from the customer archive because its proprietary
 * support headers are unavailable.  It still tries to install its historical
 * private handler.  Redirect that one symbol here: the request occurs while
 * RT_DEV_LOCK_CRYPTO is held and before DMA submission, so reclaim the global
 * adapter immediately and never expose the private handler to hardware.
 */
void carbox_crypto_irq_controller_vendor_enable(
	void *adapter, void (*ignored_handler)(int, int)
)
{
	(void)adapter;
	(void)ignored_handler;
	/* On the first engine user, install the unified hardware handler. */
	if (!crypto_irq_installed || !crypto_irq_ready ||
	    !g_rtl_cryptoEngine_s.isIntMode) {
		(void)carbox_crypto_irq_controller_enable();
		return;
	}

	/*
	 * AES has only assigned its historical function pointers; its redirected
	 * rtl_crypto_irq_enable() call has not touched the actual IRQ vector. Put
	 * the common pointers back without reprogramming the IRQ controller.
	 */
	g_rtl_cryptoEngine_s.pre_exec_func = crypto_irq_pre_exec;
	g_rtl_cryptoEngine_s.wait_done_func = crypto_irq_wait_done;
	__sync_fetch_and_add(&crypto_irq_vendor_restores, 1u);
}

void carbox_crypto_irq_controller_engine_reset(void)
{
	crypto_irq_active = 0u;
	crypto_irq_last_timeout = 0u;
	crypto_irq_installed = 0u;
	__sync_fetch_and_add(&crypto_irq_resets, 1u);
	if (crypto_irq_ready) {
		while (rtw_down_timeout_sema(&crypto_irq_completion, 0) == _TRUE) {
			__sync_fetch_and_add(&crypto_irq_drained, 1u);
		}
	}
}

int carbox_crypto_irq_controller_last_timed_out(void)
{
	return crypto_irq_last_timeout != 0u;
}

void carbox_crypto_irq_controller_report(unsigned window_index)
{
	printf(
		"[CRYPTOIRQ][%u] unified=1 irq_only=1 enable/install/rebind="
		"%lu/%lu/%lu pre/wait/complete/timeout=%lu/%lu/%lu/%lu "
		"spurious/drained/reset/vendor_restore=%lu/%lu/%lu/%lu "
		"active/gen=%u/%lu\n",
		window_index,
		(unsigned long)crypto_irq_enable_calls,
		(unsigned long)crypto_irq_installs,
		(unsigned long)crypto_irq_rebinds,
		(unsigned long)crypto_irq_pre_execs,
		(unsigned long)crypto_irq_waits,
		(unsigned long)crypto_irq_completions,
		(unsigned long)crypto_irq_timeouts,
		(unsigned long)crypto_irq_spurious,
		(unsigned long)crypto_irq_drained,
		(unsigned long)crypto_irq_resets,
		(unsigned long)crypto_irq_vendor_restores,
		crypto_irq_active,
		(unsigned long)crypto_irq_generation
	);
}

typedef struct {
	TaskHandle_t task;
	UBaseType_t original_priority;
	unsigned kind;
	unsigned depth;
} carbox_crypto_priority_slot_t;

static carbox_crypto_priority_slot_t crypto_priority_slots[
	CARBOX_CRYPTO_PRIORITY_SLOTS
] CARBOX_CRYPTO_IRQ_STATS;
static unsigned crypto_priority_slot_exhausted_reported
	CARBOX_CRYPTO_IRQ_STATS;

static carbox_crypto_priority_slot_t *crypto_priority_find_slot(
	TaskHandle_t task, int allocate
)
{
	carbox_crypto_priority_slot_t *free_slot = NULL;
	unsigned i;

	for (i = 0; i < CARBOX_CRYPTO_PRIORITY_SLOTS; ++i) {
		carbox_crypto_priority_slot_t *slot = &crypto_priority_slots[i];

		if (slot->task == task) return slot;
		if (!slot->task && !free_slot) free_slot = slot;
	}
	if (allocate && free_slot) free_slot->task = task;
	return allocate ? free_slot : NULL;
}

static void crypto_priority_enter(unsigned kind)
{
	TaskHandle_t task;
	carbox_crypto_priority_slot_t *slot;
	UBaseType_t priority;
	int report_exhaustion = 0;

	if (rtw_in_interrupt()) return;
	task = xTaskGetCurrentTaskHandle();
	if (!task) return;
	priority = uxTaskPriorityGet(task);

	taskENTER_CRITICAL();
	slot = crypto_priority_find_slot(task, 1);
	if (slot) {
		if (slot->depth++ == 0u) {
			slot->original_priority = priority;
			slot->kind = kind;
		}
	} else if (!crypto_priority_slot_exhausted_reported) {
		crypto_priority_slot_exhausted_reported = 1u;
		report_exhaustion = 1;
	}
	taskEXIT_CRITICAL();
	if (report_exhaustion) {
		printf("[CRYPTO][PRIO] slot table exhausted; boost skipped\n");
	}

	if (slot && (CARBOX_CRYPTO_OWNER_BOOST_PRIORITY > 0) &&
	    priority < CARBOX_CRYPTO_OWNER_BOOST_PRIORITY) {
		vTaskPrioritySet(task, CARBOX_CRYPTO_OWNER_BOOST_PRIORITY);
	}
}

static void crypto_priority_leave(void)
{
	TaskHandle_t task;
	carbox_crypto_priority_slot_t *slot;
	UBaseType_t original_priority = 0;
	int restore = 0;

	if (rtw_in_interrupt()) return;
	task = xTaskGetCurrentTaskHandle();
	if (!task) return;

	taskENTER_CRITICAL();
	slot = crypto_priority_find_slot(task, 0);
	if (slot && slot->depth) {
		if (--slot->depth == 0u) {
			original_priority = slot->original_priority;
			slot->task = NULL;
			restore = 1;
		}
	}
	taskEXIT_CRITICAL();

	if (restore && uxTaskPriorityGet(task) != original_priority) {
		vTaskPrioritySet(task, original_priority);
	}
}

static void crypto_priority_device_lock(RT_DEV_LOCK_E device, unsigned kind)
{
	if (device == RT_DEV_LOCK_CRYPTO) crypto_priority_enter(kind);
	device_mutex_lock(device);
}

static void crypto_priority_device_unlock(RT_DEV_LOCK_E device)
{
	/*
	 * Release first while still at the boosted priority.  Priority 11 is the
	 * platform maximum, so the current task can restore its slot immediately
	 * without leaving a lower-priority task holding the engine lock.
	 */
	device_mutex_unlock(device);
	if (device == RT_DEV_LOCK_CRYPTO) crypto_priority_leave();
}

/* Separate public entries preserve AES/ChaCha attribution in the profiler. */
void carbox_crypto_aes_device_lock(RT_DEV_LOCK_E device)
{
	crypto_priority_device_lock(device, CARBOX_CRYPTO_KIND_AES);
}

void carbox_crypto_aes_device_unlock(RT_DEV_LOCK_E device)
{
	crypto_priority_device_unlock(device);
}

void carbox_crypto_chacha_device_lock(RT_DEV_LOCK_E device)
{
	crypto_priority_device_lock(device, CARBOX_CRYPTO_KIND_CHACHA);
}

void carbox_crypto_chacha_device_unlock(RT_DEV_LOCK_E device)
{
	crypto_priority_device_unlock(device);
}

unsigned carbox_crypto_priority_current_kind(void)
{
	TaskHandle_t task;
	carbox_crypto_priority_slot_t *slot;
	unsigned kind = CARBOX_CRYPTO_KIND_NONE;

	if (rtw_in_interrupt()) return kind;
	task = xTaskGetCurrentTaskHandle();
	if (!task) return kind;
	taskENTER_CRITICAL();
	slot = crypto_priority_find_slot(task, 0);
	if (slot && slot->depth) kind = slot->kind;
	taskEXIT_CRITICAL();
	return kind;
}
