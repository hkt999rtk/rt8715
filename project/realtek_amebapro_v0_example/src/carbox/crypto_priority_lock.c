#include "crypto_priority_lock.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "osdep_service.h"
#include "hal_crypto.h"
#include "hal_timer.h"
#include "rtl8195bhp_crypto_ctrl.h"
#include "rtl8195bhp_crypto.h"

#ifndef CARBOX_CRYPTO_OWNER_BOOST_PRIORITY
#define CARBOX_CRYPTO_OWNER_BOOST_PRIORITY 11
#endif

#if CARBOX_CRYPTO_OWNER_BOOST_PRIORITY >= configMAX_PRIORITIES
#error "CARBOX_CRYPTO_OWNER_BOOST_PRIORITY must be below configMAX_PRIORITIES"
#endif

#define CARBOX_CRYPTO_PRIORITY_SLOTS 8u

#ifndef CARBOX_CRYPTO_IRQ_TIMEOUT_MS
#define CARBOX_CRYPTO_IRQ_TIMEOUT_MS 20u
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
static volatile uint32_t crypto_irq_last_timeout_at_us CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_last_timeout_generation
	CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_last_timeout_kind CARBOX_CRYPTO_IRQ_STATS;
static volatile uintptr_t crypto_irq_last_timeout_task CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_last_timeout_priority
	CARBOX_CRYPTO_IRQ_STATS;

/* Callback metadata only: never retain keys, IVs or DMA payloads. */
static volatile uint32_t crypto_irq_last_callback_tick CARBOX_CRYPTO_IRQ_STATS;
static volatile uint32_t crypto_irq_last_callback_generation CARBOX_CRYPTO_IRQ_STATS;
static volatile int crypto_irq_last_callback_done CARBOX_CRYPTO_IRQ_STATS;
static volatile int crypto_irq_last_callback_crc CARBOX_CRYPTO_IRQ_STATS;

typedef struct {
	uint32_t at_us, at_tick, wait_us, generation, active;
	uint32_t completions, waits, timeouts, spurious;
	uint32_t csr, mask, errors, src_fifo, dst_fifo, debug;
	uint32_t irq_enabled, irq_pending, irq_active, irq_priority, irq_target_ns;
	uint32_t primask, basepri, faultmask, ipsr, icsr;
	uint32_t callback_tick, callback_generation;
	int callback_done, callback_crc;
	uint32_t initialized, int_mode, hooks_ok, cipher, auth;
} crypto_irq_timeout_detail_t;

/* Retained across engine recovery for debugger inspection. */
static volatile crypto_irq_timeout_detail_t crypto_irq_timeout_detail
	CARBOX_CRYPTO_IRQ_STATS;

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
	TaskHandle_t owner;
	unsigned owner_kind;
	UBaseType_t owner_priority;
	uint32_t wait_begin_us;
	crypto_irq_timeout_detail_t d;

	if (!rtl_adapter->isIntMode) return -1;
	__sync_fetch_and_add(&crypto_irq_waits, 1u);
	wait_begin_us = hal_read_curtime_us();
	if (rtw_down_timeout_sema(
		&crypto_irq_completion, CARBOX_CRYPTO_IRQ_TIMEOUT_MS
	) == _TRUE) {
		crypto_irq_active = 0u;
		return 0;
	}

	d.at_us = hal_read_curtime_us();
	d.wait_us = d.at_us - wait_begin_us;
	d.at_tick = (uint32_t)xTaskGetTickCount();
	/* Sample the caller's masks, not a mask installed by taskENTER_CRITICAL. */
	d.primask = __get_PRIMASK();
	d.basepri = __get_BASEPRI();
	d.faultmask = __get_FAULTMASK();
	d.ipsr = __get_IPSR();
	__disable_irq();
	__DSB();
	__ISB();
	/*
	 * rtl_crypto_irq_enable uses the secure engine and SCrypto_IRQn (29).
	 * Status reads do not acknowledge completion/errors. In particular, the
	 * error register must be read as 16 bits and must never be written here.
	 * Hardware can still progress during these sequential register reads.
	 * No RTOS calls or printing while IRQs are masked.
	 */
	d.csr = CRYPTO_S_MODULE->ipscsr_reset_isr_conf_reg;
	d.mask = CRYPTO_S_MODULE->ipscsr_int_mask_reg;
	d.errors = CRYPTO_S_MODULE->ipscsr_err_stats_reg;
	d.src_fifo = CRYPTO_S_MODULE->srcdesc_status_reg;
	d.dst_fifo = CRYPTO_S_MODULE->dstdesc_status_reg;
	d.debug = CRYPTO_S_MODULE->ipscsr_debug_reg;
	d.irq_enabled = NVIC_GetEnableIRQ(SCrypto_IRQn);
	d.irq_pending = NVIC_GetPendingIRQ(SCrypto_IRQn);
	d.irq_active = NVIC_GetActive(SCrypto_IRQn);
	d.irq_priority = NVIC_GetPriority(SCrypto_IRQn);
#if defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3U)
	d.irq_target_ns = NVIC_GetTargetState(SCrypto_IRQn);
#else
	d.irq_target_ns = 0xffffffffu;
#endif
	d.icsr = SCB->ICSR;
	d.generation = crypto_irq_generation;
	d.active = crypto_irq_active;
	d.completions = crypto_irq_completions;
	d.waits = crypto_irq_waits;
	d.spurious = crypto_irq_spurious;
	d.callback_tick = crypto_irq_last_callback_tick;
	d.callback_generation = crypto_irq_last_callback_generation;
	d.callback_done = crypto_irq_last_callback_done;
	d.callback_crc = crypto_irq_last_callback_crc;
	d.initialized = rtl_adapter->isInit;
	d.int_mode = rtl_adapter->isIntMode;
	d.hooks_ok = (rtl_adapter->pre_exec_func == crypto_irq_pre_exec) &&
		(rtl_adapter->wait_done_func == crypto_irq_wait_done);
	d.cipher = rtl_adapter->cipher_type;
	d.auth = rtl_adapter->auth_type;
	crypto_irq_active = 0u;
	crypto_irq_last_timeout = 1u;
	d.timeouts = ++crypto_irq_timeouts;
	__DMB();
	__set_PRIMASK(d.primask);
	__ISB();
	crypto_irq_timeout_detail = d;
	owner = xTaskGetCurrentTaskHandle();
	owner_kind = carbox_crypto_priority_current_kind();
	owner_priority = owner ? uxTaskPriorityGet(owner) : 0u;
	taskENTER_CRITICAL();
	crypto_irq_last_timeout_at_us = d.at_us;
	crypto_irq_last_timeout_generation = d.generation;
	crypto_irq_last_timeout_kind = owner_kind;
	crypto_irq_last_timeout_task = (uintptr_t)owner;
	crypto_irq_last_timeout_priority = (uint32_t)owner_priority;
	taskEXIT_CRITICAL();
	printf(
		"[CRYPTOIRQ][TIMEOUT] generation=%lu timeout_ms=%lu wait_us=%lu "
		"at_us=%lu at_tick=%lu tick_hz=%lu task=%s handle=%p kind=%u priority=%lu\n",
		(unsigned long)d.generation,
		(unsigned long)CARBOX_CRYPTO_IRQ_TIMEOUT_MS,
		(unsigned long)d.wait_us, (unsigned long)d.at_us,
		(unsigned long)d.at_tick, (unsigned long)configTICK_RATE_HZ,
		owner ? pcTaskGetName(owner) : "?", (void *)owner,
		owner_kind, (unsigned long)owner_priority
	);
	printf(
		"[CRYPTOIRQ][HW] generation=%lu pre_recovery=1 csr=%08lx "
		"dma_busy=%lu cmd_ok=%lu ok_count=%lu mask=%08lx "
		"cmd_ok_masked=%lu errors=%04lx src_fifo=%08lx dst_fifo=%08lx debug=%08lx\n",
		(unsigned long)d.generation, (unsigned long)d.csr,
		(unsigned long)((d.csr >> 3) & 1u),
		(unsigned long)((d.csr >> 4) & 1u),
		(unsigned long)((d.csr >> 8) & 0xffu),
		(unsigned long)d.mask, (unsigned long)(d.mask & 1u),
		(unsigned long)d.errors, (unsigned long)d.src_fifo,
		(unsigned long)d.dst_fifo, (unsigned long)d.debug
	);
	printf(
		"[CRYPTOIRQ][NVIC] generation=%lu irq=%u enabled/pending/active=%lu/%lu/%lu "
		"priority=%lu target_ns=%lu primask=%lu basepri=%02lx "
		"faultmask=%lu ipsr=%lu icsr=%08lx\n",
		(unsigned long)d.generation, (unsigned)SCrypto_IRQn,
		(unsigned long)d.irq_enabled, (unsigned long)d.irq_pending,
		(unsigned long)d.irq_active, (unsigned long)d.irq_priority,
		(unsigned long)d.irq_target_ns, (unsigned long)d.primask,
		(unsigned long)d.basepri, (unsigned long)d.faultmask,
		(unsigned long)d.ipsr, (unsigned long)d.icsr
	);
	printf(
		"[CRYPTOIRQ][SW] generation=%lu active=%lu accepted_cb/waits/timeouts/spurious="
		"%lu/%lu/%lu/%lu last_cb_gen=%lu last_cb_tick=%lu done/crc=%d/%d "
		"init/int_mode/hooks_ok=%lu/%lu/%lu cipher=%08lx auth=%08lx\n",
		(unsigned long)d.generation, (unsigned long)d.active,
		(unsigned long)d.completions, (unsigned long)d.waits,
		(unsigned long)d.timeouts, (unsigned long)d.spurious,
		(unsigned long)d.callback_generation, (unsigned long)d.callback_tick,
		d.callback_done, d.callback_crc,
		(unsigned long)d.initialized, (unsigned long)d.int_mode,
		(unsigned long)d.hooks_ok, (unsigned long)d.cipher, (unsigned long)d.auth
	);
	/* IRQ-only diagnostic build: do not enter the HAL polling waiter. */
	return -1;
}

static void crypto_irq_handler(int crypto_done, int crc_done)
{
	/* Avoid the HAL timer synchronization loop in interrupt context. */
	crypto_irq_last_callback_tick = (uint32_t)xTaskGetTickCountFromISR();
	crypto_irq_last_callback_generation = crypto_irq_generation;
	crypto_irq_last_callback_done = crypto_done;
	crypto_irq_last_callback_crc = crc_done;
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

/* Called under RT_DEV_LOCK_CRYPTO, with the engine clock still enabled.
 * SDK crypto_ipscsr_reset_isr_conf_reg_t documents bit31 as resetting BOTH
 * the crypto and DMA engines; bit3 is the independently observed DMA busy.
 * Do not trust HAL deinit's return code as a DMA completion indication. */
int carbox_crypto_irq_controller_quiesce(void)
{
    volatile uint32_t *csr = &CRYPTO_S_MODULE->ipscsr_reset_isr_conf_reg;
    uint32_t start;
    NVIC_DisableIRQ(SCrypto_IRQn);
    __DSB();
    __ISB();
    crypto_irq_active = 0u;
    *csr = 0x80000000u;
    __DSB();
    start = hal_read_curtime_us();
    while ((*csr & (1u << 3)) != 0u) {
        if ((uint32_t)(hal_read_curtime_us() - start) >= 1000u) return -1;
    }
    __DSB();
    NVIC_ClearPendingIRQ(SCrypto_IRQn);
    __DSB();
    return 0;
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

void carbox_crypto_irq_controller_snapshot(
	carbox_crypto_irq_snapshot_t *snapshot
)
{
	if (!snapshot) return;
	taskENTER_CRITICAL();
	snapshot->timeout_count = crypto_irq_timeouts;
	snapshot->reset_count = crypto_irq_resets;
	snapshot->generation = crypto_irq_generation;
	snapshot->last_timeout_at_us = crypto_irq_last_timeout_at_us;
	snapshot->last_timeout_generation = crypto_irq_last_timeout_generation;
	snapshot->last_timeout_kind = crypto_irq_last_timeout_kind;
	snapshot->last_timeout_task = crypto_irq_last_timeout_task;
	snapshot->last_timeout_priority = crypto_irq_last_timeout_priority;
	taskEXIT_CRITICAL();
}

void carbox_crypto_irq_controller_report(unsigned window_index)
{
	printf(
		"[CRYPTOIRQ][%u] unified=1 irq_only=1 timeout_ms=%lu "
		"enable/install/rebind="
		"%lu/%lu/%lu pre/wait/complete/timeout=%lu/%lu/%lu/%lu "
		"spurious/drained/reset/vendor_restore=%lu/%lu/%lu/%lu "
		"active/gen=%u/%lu\n",
		window_index,
		(unsigned long)CARBOX_CRYPTO_IRQ_TIMEOUT_MS,
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
