#include "large_memcpy_gdma.h"

#include "cmsis.h"
#include "diag.h"
#include "dma_api.h"
#include "hal_timer.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <stdint.h>
#include <string.h>

#ifndef CONFIG_LARGE_MEMCPY_GDMA
#define CONFIG_LARGE_MEMCPY_GDMA 0
#endif

#ifndef LARGE_MEMCPY_GDMA_THRESHOLD
#define LARGE_MEMCPY_GDMA_THRESHOLD 4096U
#endif

#ifndef LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY
#define LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY 11U
#endif

#ifndef LARGE_MEMCPY_GDMA_COPYV_VERIFY
#define LARGE_MEMCPY_GDMA_COPYV_VERIFY 0
#endif

#if LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY >= configMAX_PRIORITIES
#error "LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY must be below configMAX_PRIORITIES"
#endif

#define LARGE_MEMCPY_GDMA_CACHE_LINE 32U
#define LARGE_MEMCPY_GDMA_LINK_BURST_BYTES 64U
/* An ordinary contiguous memcpy uses four-byte transfers, so one transaction
 * can cover 4095 * 4 bytes per descriptor.  The scatter/gather helper below
 * deliberately uses the SDK multi-block byte-transfer convention instead. */
#define LARGE_MEMCPY_GDMA_TRANSACTION_MAX \
	(MAX_DMA_BLOCK_SIZE * MAX_MULTI_BLOCK_NUM * sizeof(uint32_t))
#define LARGE_MEMCPY_GDMA_TIMEOUT_MS 20U
/* RTL8195BHP has two GDMA controllers.  On each controller, channels 4 and 5
 * are the only channels that support multi-block/linked-list transfers.  Keep
 * one persistent context for every linked-list-capable hardware channel. */
#define LARGE_MEMCPY_GDMA_CONTEXT_COUNT \
	((MAX_GDMA_INDX + 1U) * (MAX_GDMA_CHNL - MIN_MULTI_CHNL_NUM))
#if LARGE_MEMCPY_GDMA_CONTEXT_COUNT != 4U
#error "RTL8195BHP linked GDMA callback table expects four hardware channels"
#endif
#define LARGE_MEMCPY_GDMA_LINK_BLOCK_BYTES \
	(MAX_DMA_BLOCK_SIZE & \
	 ~(LARGE_MEMCPY_GDMA_LINK_BURST_BYTES - 1U))

/* DW_axi_dmac channel CTL_LOW fields used by the linked memcpy path.  Keep
 * these definitions local: this wrapper owns channels 4/5 exclusively, while
 * the SDK header exposes the register fields but no masks for composing a
 * complete linked-list control word. */
#define LARGE_MEMCPY_GDMA_CTL_INT_EN       (1U << 0)
#define LARGE_MEMCPY_GDMA_CTL_LLP_DST_EN   (1U << 27)
#define LARGE_MEMCPY_GDMA_CTL_LLP_SRC_EN   (1U << 28)
#define LARGE_MEMCPY_GDMA_CTL_LLP_ENABLE \
	(LARGE_MEMCPY_GDMA_CTL_LLP_DST_EN | LARGE_MEMCPY_GDMA_CTL_LLP_SRC_EN)
#if CONFIG_LARGE_MEMCPY_GDMA

typedef struct large_memcpy_gdma_context_s {
	gdma_t dma;
	SemaphoreHandle_t done;
	const char *disabled_reason;
	uint32_t disabled_raw_error;
	uint32_t program_attempts;
	uint32_t program_successes;
	uint32_t completion_successes;
	uint32_t completion_failures;
	uint32_t acquisitions;
	uint32_t last_ch_en;
	uint32_t last_cfg_before_program;
	uint32_t last_cfg_after_program;
	uint32_t last_actual_sar;
	uint32_t last_actual_dar;
	uint32_t last_actual_llp;
	uint32_t last_actual_ctl_low;
	uint32_t last_actual_ctl_up;
	uint32_t last_actual_cfg_low;
	uint32_t last_expected_sar;
	uint32_t last_expected_dar;
	uint32_t last_expected_llp;
	uint32_t last_expected_ctl_low;
	uint32_t last_expected_ctl_up;
	volatile uint8_t busy;
	volatile uint8_t in_flight;
	volatile uint8_t irq_error;
	volatile uint8_t last_channel_valid;
	uint8_t available;
} large_memcpy_gdma_context_t;

static large_memcpy_gdma_context_t large_memcpy_gdma[LARGE_MEMCPY_GDMA_CONTEXT_COUNT];
static uint8_t large_memcpy_gdma_initialized;
static uint8_t large_memcpy_gdma_early_irq_reported;
static uint8_t large_memcpy_gdma_lli_reported;
static uint8_t large_memcpy_gdma_channel_reported;
static uint32_t large_memcpy_gdma_unavailable_fallbacks;
static uint32_t large_memcpy_gdma_busy_fallbacks;
static uint32_t large_memcpy_gdma_contiguous_attempts;
static uint32_t large_memcpy_gdma_contiguous_successes;
static uint32_t large_memcpy_gdma_contiguous_bytes;
static uint32_t large_memcpy_gdma_contiguous_cache_clean_us;
static uint32_t large_memcpy_gdma_contiguous_cache_clean_max_us;
static uint32_t large_memcpy_gdma_contiguous_dma_wait_us;
static uint32_t large_memcpy_gdma_contiguous_dma_wait_max_us;
static uint32_t large_memcpy_gdma_contiguous_cache_invalidate_us;
static uint32_t large_memcpy_gdma_contiguous_cache_invalidate_max_us;
static uint32_t large_memcpy_gdma_contiguous_cpu_edge_us;
static uint32_t large_memcpy_gdma_contiguous_cpu_edge_max_us;
/* Channel ownership has an atomic/critical-section fast path.  This semaphore
 * is only a contention event: ordinary uncontended DMA calls do not execute an
 * RTOS semaphore take/give at all. */
static SemaphoreHandle_t large_memcpy_gdma_channel_released;
static volatile uint32_t large_memcpy_gdma_channel_waiters;
static uint8_t large_memcpy_gdma_next_context;

static int large_memcpy_in_interrupt(void)
{
	uint32_t ipsr;

	__asm volatile("mrs %0, ipsr" : "=r" (ipsr));
	return ipsr != 0U;
}

static void large_memcpy_gdma_atomic_max(uint32_t *maximum, uint32_t value)
{
	uint32_t previous = __atomic_load_n(maximum, __ATOMIC_RELAXED);

	while (value > previous &&
	       !__atomic_compare_exchange_n(maximum, &previous, value, 0,
					    __ATOMIC_RELAXED,
					    __ATOMIC_RELAXED)) {
	}
}

static void large_memcpy_gdma_drain_completion(large_memcpy_gdma_context_t *context)
{
	while (xSemaphoreTake(context->done, 0) == pdTRUE) {
	}
}

static void large_memcpy_gdma_done_common(large_memcpy_gdma_context_t *context)
{
	phal_gdma_adaptor_t adaptor = &context->dma.hal_gdma_adaptor;
	BaseType_t task_woken = pdFALSE;
	uint32_t channel_mask;

	if (!context->in_flight || context->done == NULL ||
	    adaptor->gdma_dev == NULL) {
		return;
	}

	channel_mask = 1U << adaptor->ch_num;
	if ((adaptor->gdma_dev->raw_err & channel_mask) != 0U) {
		context->irq_error = 1U;
	}
	context->in_flight = 0U;
	xSemaphoreGiveFromISR(context->done, &task_woken);
	portYIELD_FROM_ISR(task_woken);
}

static void large_memcpy_gdma_done0(uint32_t id)
{
	(void)id;
	large_memcpy_gdma_done_common(&large_memcpy_gdma[0]);
}

static void large_memcpy_gdma_done1(uint32_t id)
{
	(void)id;
	large_memcpy_gdma_done_common(&large_memcpy_gdma[1]);
}

static void large_memcpy_gdma_done2(uint32_t id)
{
	(void)id;
	large_memcpy_gdma_done_common(&large_memcpy_gdma[2]);
}

static void large_memcpy_gdma_done3(uint32_t id)
{
	(void)id;
	large_memcpy_gdma_done_common(&large_memcpy_gdma[3]);
}

static const dma_irq_handler large_memcpy_gdma_callbacks[] = {
	large_memcpy_gdma_done0,
	large_memcpy_gdma_done1,
	large_memcpy_gdma_done2,
	large_memcpy_gdma_done3,
};

static void large_memcpy_gdma_configure_irq(large_memcpy_gdma_context_t *context)
{
	phal_gdma_adaptor_t adaptor = &context->dma.hal_gdma_adaptor;

	/* ErrorType and TransferType share one ROM callback.  Use completion-only
	 * IRQs and diagnose a transfer error from raw_err on timeout/completion. */
	hal_gdma_isr_dis(adaptor);
	hal_gdma_clean_pending_isr(adaptor);
	adaptor->gdma_isr_type = TransferType;
	hal_gdma_isr_en(adaptor);
}

static int large_memcpy_gdma_allocate(large_memcpy_gdma_context_t *context,
				      dma_irq_handler callback)
{
	phal_gdma_adaptor_t adaptor = &context->dma.hal_gdma_adaptor;

	dma_memcpy_init(&context->dma, callback, 0U);
	if (!adaptor->have_chnl) {
		context->disabled_reason = "initial-channel-allocation";
		return -1;
	}

	/* dma_memcpy_init() starts with an ordinary channel.  Convert it once at
	 * boot to channel 4/5 and retain it, so the first frame cannot trigger a
	 * hidden free/reallocate operation inside hal_gdma_memcpy(). */
	if (adaptor->ch_num < MIN_MULTI_CHNL_NUM) {
		if (hal_gdma_chnl_alloc(adaptor, MultiBlkEn) != HAL_OK) {
			context->disabled_reason = "linked-channel-allocation";
			return -1;
		}
		hal_gdma_chnl_init(adaptor);
	}
	if (adaptor->gdma_index > MAX_GDMA_INDX ||
	    adaptor->ch_num < MIN_MULTI_CHNL_NUM ||
	    adaptor->ch_num >= MAX_GDMA_CHNL || adaptor->pgdma_ch_lli == NULL) {
		context->disabled_reason = "invalid-linked-channel";
		if (adaptor->have_chnl) {
			dma_memcpy_deinit(&context->dma);
		}
		return -1;
	}
	/* Reinstall IRQ ownership after channel migration.  These calls route the
	 * group IRQ/callback; they do not compose the linked-list CTL/SAR/DAR words. */
	hal_gdma_irq_reg(adaptor,
		(irq_handler_t)hal_gdma_stubs.hal_gdma_memcpy_irq_handler,
		adaptor);
	hal_gdma_memcpy_irq_hook(adaptor, (gdma_callback_t)callback, adaptor);
	large_memcpy_gdma_configure_irq(context);
	context->disabled_reason = NULL;
	context->disabled_raw_error = 0U;
	context->available = 1U;
	return 0;
}

void carbox_large_memcpy_gdma_init(void)
{
	uint8_t allocated_mask[MAX_GDMA_INDX + 1U] = { 0U };
	unsigned int available = 0U;
	unsigned int i;

	if (large_memcpy_gdma_initialized) {
		return;
	}
	large_memcpy_gdma_initialized = 1U;
	large_memcpy_gdma_channel_released = xSemaphoreCreateCounting(
		LARGE_MEMCPY_GDMA_CONTEXT_COUNT, 0U);
	if (large_memcpy_gdma_channel_released == NULL) {
		rt_printf("[LARGE_MEMCPY_GDMA] channel semaphore allocation failed\r\n");
		return;
	}

	for (i = 0U; i < LARGE_MEMCPY_GDMA_CONTEXT_COUNT; ++i) {
		large_memcpy_gdma_context_t *context = &large_memcpy_gdma[i];
		phal_gdma_adaptor_t adaptor;

		context->done = xSemaphoreCreateBinary();
		if (context->done == NULL) {
			rt_printf("[LARGE_MEMCPY_GDMA] slot=%u semaphore allocation failed\r\n", i);
			continue;
		}
		if (large_memcpy_gdma_allocate(
			    context, large_memcpy_gdma_callbacks[i]) != 0) {
			rt_printf("[LARGE_MEMCPY_GDMA] slot=%u linked channel allocation failed\r\n", i);
			vSemaphoreDelete(context->done);
			context->done = NULL;
			continue;
		}

		adaptor = &context->dma.hal_gdma_adaptor;
		allocated_mask[adaptor->gdma_index] |=
			(uint8_t)(1U << adaptor->ch_num);
		++available;
		rt_printf("[LARGE_MEMCPY_GDMA] slot=%u ready gdma=%u channel=%u\r\n",
			  i, (unsigned int)adaptor->gdma_index,
			  (unsigned int)adaptor->ch_num);
	}

	rt_printf("[LARGE_MEMCPY_GDMA] available=%u threshold=>%u "
		  "requested=%u linked_blocks=%u transaction_max=%u "
		  "mask gdma0/gdma1=0x%02x/0x%02x "
		  "channel_programming=rx-validated-hal\r\n",
		  available,
		  (unsigned int)LARGE_MEMCPY_GDMA_THRESHOLD,
		  (unsigned int)LARGE_MEMCPY_GDMA_CONTEXT_COUNT,
		  (unsigned int)MAX_MULTI_BLOCK_NUM,
		  (unsigned int)LARGE_MEMCPY_GDMA_TRANSACTION_MAX,
		  (unsigned int)allocated_mask[0],
		  (unsigned int)allocated_mask[1]);
}

void carbox_large_memcpy_gdma_report(uint32_t sequence)
{
	uint32_t busy = __atomic_exchange_n(&large_memcpy_gdma_busy_fallbacks,
		0U, __ATOMIC_RELAXED);
	uint32_t unavailable = __atomic_exchange_n(
		&large_memcpy_gdma_unavailable_fallbacks, 0U, __ATOMIC_RELAXED);
	uint32_t available_count = 0U;
	uint32_t busy_count = 0U;
	uint32_t contiguous_attempts = __atomic_exchange_n(
		&large_memcpy_gdma_contiguous_attempts, 0U, __ATOMIC_RELAXED);
	uint32_t contiguous_successes = __atomic_exchange_n(
		&large_memcpy_gdma_contiguous_successes, 0U, __ATOMIC_RELAXED);
	uint32_t contiguous_bytes = __atomic_exchange_n(
		&large_memcpy_gdma_contiguous_bytes, 0U, __ATOMIC_RELAXED);
	uint32_t cache_clean_us = __atomic_exchange_n(
		&large_memcpy_gdma_contiguous_cache_clean_us, 0U,
		__ATOMIC_RELAXED);
	uint32_t cache_clean_max_us = __atomic_exchange_n(
		&large_memcpy_gdma_contiguous_cache_clean_max_us, 0U,
		__ATOMIC_RELAXED);
	uint32_t dma_wait_us = __atomic_exchange_n(
		&large_memcpy_gdma_contiguous_dma_wait_us, 0U,
		__ATOMIC_RELAXED);
	uint32_t dma_wait_max_us = __atomic_exchange_n(
		&large_memcpy_gdma_contiguous_dma_wait_max_us, 0U,
		__ATOMIC_RELAXED);
	uint32_t cache_invalidate_us = __atomic_exchange_n(
		&large_memcpy_gdma_contiguous_cache_invalidate_us, 0U,
		__ATOMIC_RELAXED);
	uint32_t cache_invalidate_max_us = __atomic_exchange_n(
		&large_memcpy_gdma_contiguous_cache_invalidate_max_us, 0U,
		__ATOMIC_RELAXED);
	uint32_t cpu_edge_us = __atomic_exchange_n(
		&large_memcpy_gdma_contiguous_cpu_edge_us, 0U,
		__ATOMIC_RELAXED);
	uint32_t cpu_edge_max_us = __atomic_exchange_n(
		&large_memcpy_gdma_contiguous_cpu_edge_max_us, 0U,
		__ATOMIC_RELAXED);
	uint32_t waiters;
	unsigned int i;

	taskENTER_CRITICAL();
	for (i = 0U; i < LARGE_MEMCPY_GDMA_CONTEXT_COUNT; ++i) {
		available_count += large_memcpy_gdma[i].available ? 1U : 0U;
		busy_count += large_memcpy_gdma[i].busy ? 1U : 0U;
	}
	waiters = large_memcpy_gdma_channel_waiters;
	taskEXIT_CRITICAL();

	rt_printf("[LARGE_MEMCPY_GDMA][%lu] fallback busy/unavailable=%lu/%lu "
		  "contexts total/available/busy=%u/%lu/%lu waiters=%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)busy, (unsigned long)unavailable,
		  (unsigned int)LARGE_MEMCPY_GDMA_CONTEXT_COUNT,
		  (unsigned long)available_count, (unsigned long)busy_count,
		  (unsigned long)waiters);
	rt_printf("[LARGE_MEMCPY_GDMA][%lu][PHASE] contiguous attempt/success/bytes="
		  "%lu/%lu/%lu us total/avg/max cache_clean=%lu/%lu/%lu "
		  "dma_wait=%lu/%lu/%lu cache_invalidate=%lu/%lu/%lu "
		  "cpu_edge=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)contiguous_attempts,
		  (unsigned long)contiguous_successes,
		  (unsigned long)contiguous_bytes,
		  (unsigned long)cache_clean_us,
		  (unsigned long)(contiguous_attempts != 0U ?
			cache_clean_us / contiguous_attempts : 0U),
		  (unsigned long)cache_clean_max_us,
		  (unsigned long)dma_wait_us,
		  (unsigned long)(contiguous_attempts != 0U ?
			dma_wait_us / contiguous_attempts : 0U),
		  (unsigned long)dma_wait_max_us,
		  (unsigned long)cache_invalidate_us,
		  (unsigned long)(contiguous_attempts != 0U ?
			cache_invalidate_us / contiguous_attempts : 0U),
		  (unsigned long)cache_invalidate_max_us,
		  (unsigned long)cpu_edge_us,
		  (unsigned long)(contiguous_attempts != 0U ?
			cpu_edge_us / contiguous_attempts : 0U),
		  (unsigned long)cpu_edge_max_us);

	/* Keep the first/last channel-programming evidence in the periodic report.
	 * A channel may be permanently deinitialized immediately after a fatal
	 * error, so reading only the current adaptor registers would lose the values
	 * that explain why TX subsequently reports success=0/fallback=N. */
	for (i = 0U; i < LARGE_MEMCPY_GDMA_CONTEXT_COUNT; ++i) {
		large_memcpy_gdma_context_t *context = &large_memcpy_gdma[i];
		uint32_t attempts = __atomic_exchange_n(&context->program_attempts,
			0U, __ATOMIC_RELAXED);
		uint32_t programmed = __atomic_exchange_n(
			&context->program_successes, 0U, __ATOMIC_RELAXED);
		uint32_t completed = __atomic_exchange_n(
			&context->completion_successes, 0U, __ATOMIC_RELAXED);
		uint32_t completion_failed = __atomic_exchange_n(
			&context->completion_failures, 0U, __ATOMIC_RELAXED);
		uint32_t acquisitions = __atomic_exchange_n(
			&context->acquisitions, 0U, __ATOMIC_RELAXED);
		phal_gdma_adaptor_t adaptor = &context->dma.hal_gdma_adaptor;
		const char *reason = context->available ? "ready" :
			(context->disabled_reason != NULL ? context->disabled_reason :
			 "not-initialized");

		rt_printf("[LARGE_MEMCPY_GDMA][%lu][slot%u] "
			  "bind=%u/%u have=%u avail/busy=%u/%u claims=%lu "
			  "reason=%s raw=0x%02x "
			  "hal_program attempt/ok=%lu/%lu "
			  "completion ok/fail=%lu/%lu "
			  "snapshot=%u ch_en=0x%02lx "
			  "actual sar/dar/llp=%08lx/%08lx/%08lx ctl=%08lx/%08lx "
			  "cfg=%08lx "
			  "expected=%08lx/%08lx/%08lx ctl=%08lx/%08lx\r\n",
			  (unsigned long)sequence, i,
			  (unsigned int)adaptor->gdma_index,
			  (unsigned int)adaptor->ch_num,
			  (unsigned int)adaptor->have_chnl,
			  (unsigned int)context->available,
			  (unsigned int)context->busy,
			  (unsigned long)acquisitions,
			  reason,
			  (unsigned int)context->disabled_raw_error,
			  (unsigned long)attempts, (unsigned long)programmed,
			  (unsigned long)completed,
			  (unsigned long)completion_failed,
			  (unsigned int)__atomic_load_n(
				  &context->last_channel_valid, __ATOMIC_ACQUIRE),
			  (unsigned long)context->last_ch_en,
			  (unsigned long)context->last_actual_sar,
			  (unsigned long)context->last_actual_dar,
			  (unsigned long)context->last_actual_llp,
			  (unsigned long)context->last_actual_ctl_low,
			  (unsigned long)context->last_actual_ctl_up,
			  (unsigned long)context->last_actual_cfg_low,
			  (unsigned long)context->last_expected_sar,
			  (unsigned long)context->last_expected_dar,
			  (unsigned long)context->last_expected_llp,
			  (unsigned long)context->last_expected_ctl_low,
			  (unsigned long)context->last_expected_ctl_up);
		rt_printf("[LARGE_MEMCPY_GDMA][%lu][slot%u][HAL] "
			  "cfg before/after=%08lx/%08lx\r\n",
			  (unsigned long)sequence, i,
			  (unsigned long)context->last_cfg_before_program,
			  (unsigned long)context->last_cfg_after_program);
	}
}

static large_memcpy_gdma_context_t *
large_memcpy_gdma_context_acquire(int wait_for_channel,
				   uint8_t *waited_out,
				   uint32_t *wait_us_out)
{
	large_memcpy_gdma_context_t *context;
	uint32_t wait_start_us = 0U;
	uint8_t waited = 0U;
	unsigned int i;

	if (waited_out != NULL) {
		*waited_out = 0U;
	}
	if (wait_us_out != NULL) {
		*wait_us_out = 0U;
	}
	if (large_memcpy_gdma_channel_released == NULL) {
		__atomic_fetch_add(&large_memcpy_gdma_unavailable_fallbacks, 1U,
			__ATOMIC_RELAXED);
		return NULL;
	}

	for (;;) {
		unsigned int any_available = 0U;
		unsigned int scan;

		context = NULL;
		taskENTER_CRITICAL();
		/* Round-robin selection distributes traffic and diagnostics over all
		 * four persistent hardware channels.  The same critical section protects
		 * the cursor, busy claim and waiter registration. */
		for (scan = 0U; scan < LARGE_MEMCPY_GDMA_CONTEXT_COUNT; ++scan) {
			i = (large_memcpy_gdma_next_context + scan) %
				LARGE_MEMCPY_GDMA_CONTEXT_COUNT;
			if (!large_memcpy_gdma[i].available) {
				continue;
			}
			any_available = 1U;
			if (!large_memcpy_gdma[i].busy) {
				large_memcpy_gdma[i].busy = 1U;
				large_memcpy_gdma_next_context = (uint8_t)
					((i + 1U) % LARGE_MEMCPY_GDMA_CONTEXT_COUNT);
				__atomic_fetch_add(&large_memcpy_gdma[i].acquisitions,
					1U, __ATOMIC_RELAXED);
				context = &large_memcpy_gdma[i];
				break;
			}
		}
		if (context == NULL && any_available && wait_for_channel) {
			/* Registration and the free-channel check share one critical
			 * section, so a release cannot be lost between them. */
			large_memcpy_gdma_channel_waiters++;
		}
		taskEXIT_CRITICAL();
		if (context != NULL) {
			if (waited_out != NULL) {
				*waited_out = waited;
			}
			if (wait_us_out != NULL && waited) {
				*wait_us_out = hal_read_curtime_us() - wait_start_us;
			}
			return context;
		}
		if (!any_available) {
			__atomic_fetch_add(&large_memcpy_gdma_unavailable_fallbacks, 1U,
				__ATOMIC_RELAXED);
			return NULL;
		}
		if (!wait_for_channel) {
			__atomic_fetch_add(&large_memcpy_gdma_busy_fallbacks, 1U,
				__ATOMIC_RELAXED);
			return NULL;
		}
		if (!waited) {
			waited = 1U;
			wait_start_us = hal_read_curtime_us();
		}

		/* This blocks the task and yields the CPU without periodic wakeups.  A
		 * hardware timeout/error is handled by the current channel owner, which
		 * disables that channel and still signals registered waiters on release. */
		(void)xSemaphoreTake(large_memcpy_gdma_channel_released,
			portMAX_DELAY);
		taskENTER_CRITICAL();
		large_memcpy_gdma_channel_waiters--;
		taskEXIT_CRITICAL();
	}
}

static void
large_memcpy_gdma_context_release(large_memcpy_gdma_context_t *context)
{
	int notify_waiter;

	taskENTER_CRITICAL();
	context->busy = 0U;
	notify_waiter = large_memcpy_gdma_channel_waiters != 0U;
	taskEXIT_CRITICAL();
	if (notify_waiter && large_memcpy_gdma_channel_released != NULL) {
		xSemaphoreGive(large_memcpy_gdma_channel_released);
	}
}

static int large_memcpy_gdma_wait(large_memcpy_gdma_context_t *context)
{
	phal_gdma_adaptor_t adaptor = &context->dma.hal_gdma_adaptor;
	uint32_t channel_mask = 1U << adaptor->ch_num;
	uint32_t raw_error;
	uint32_t hardware_wait_start_us;

	if (xSemaphoreTake(context->done,
			   pdMS_TO_TICKS(LARGE_MEMCPY_GDMA_TIMEOUT_MS)) == pdTRUE &&
	    !context->irq_error) {
		/* The ROM IRQ callback can run before a non-sequential linked list has
		 * completed every descriptor.  CH_EN is the documented hardware truth:
		 * it is automatically cleared only after the final AXI destination write.
		 * Yield while it remains active so the boosted owner does not busy-spin. */
		hardware_wait_start_us = hal_read_curtime_us();
		if ((adaptor->gdma_dev->ch_en_reg & channel_mask) != 0U &&
		    !large_memcpy_gdma_early_irq_reported) {
			large_memcpy_gdma_early_irq_reported = 1U;
			rt_printf("[LARGE_MEMCPY_GDMA] completion IRQ arrived before "
				  "CH_EN cleared; waiting for final AXI write\r\n");
		}
		while ((adaptor->gdma_dev->ch_en_reg & channel_mask) != 0U) {
			raw_error = adaptor->gdma_dev->raw_err & channel_mask;
			if (raw_error != 0U) {
				context->irq_error = 1U;
				break;
			}
			if ((hal_read_curtime_us() - hardware_wait_start_us) >=
			    (LARGE_MEMCPY_GDMA_TIMEOUT_MS * 1000U)) {
				break;
			}
			taskYIELD();
		}
		if ((adaptor->gdma_dev->ch_en_reg & channel_mask) == 0U &&
		    !context->irq_error) {
			__DSB();
			return 0;
		}
	}

	raw_error = adaptor->gdma_dev != NULL ?
		(adaptor->gdma_dev->raw_err & channel_mask) : 0U;
	context->disabled_reason = context->irq_error ?
		"completion-transfer-error" : "completion-timeout";
	context->disabled_raw_error = raw_error;
	rt_printf("[LARGE_MEMCPY_GDMA][ERROR] gdma=%u channel=%u "
		  "reason=%s raw=0x%02x; disabling channel and using M33 fallback\r\n",
		  (unsigned int)adaptor->gdma_index,
		  (unsigned int)adaptor->ch_num,
		  context->disabled_reason,
		  (unsigned int)context->disabled_raw_error);
	/* memcpy cannot return an error.  Quiesce the channel before the wrapper
	 * recopies the complete original range with M33, otherwise a late DMA write
	 * could corrupt the CPU fallback result.  Keep GDMA disabled after a runtime
	 * fault rather than risking an unvalidated recovery path.  A context can
	 * enter this path only once because it remains unavailable afterwards, so
	 * the error line cannot continuously flood the UART. */
	hal_gdma_isr_dis(adaptor);
	context->in_flight = 0U;
	hal_gdma_abort(adaptor);
	hal_gdma_clean_pending_isr(adaptor);
	dma_memcpy_deinit(&context->dma);
	context->available = 0U;
	large_memcpy_gdma_drain_completion(context);
	return -1;
}

int carbox_large_memcpy_gdma_try(void *dst, const void *src, size_t len)
{
	large_memcpy_gdma_context_t *context = NULL;
	uint8_t *destination = (uint8_t *)dst;
	const uint8_t *source = (const uint8_t *)src;
	TaskHandle_t owner;
	UBaseType_t original_priority;
	uint32_t prefix;
	uint32_t dma_len;
	uint32_t tail_len;
	uint32_t cache_clean_us = 0U;
	uint32_t dma_wait_us = 0U;
	uint32_t cache_invalidate_us = 0U;
	uint32_t cpu_edge_us = 0U;
	uint32_t phase_start_us;
	int copied = 0;

	/* Four persistent linked-channel contexts are claimed non-blockingly.  They
	 * cover channel 4 and 5 on both GDMA controllers and are shared by every
	 * linked-copy user.  M33 is used only when every usable context is occupied
	 * (or the request is unsuitable for DMA). */
	if (len <= LARGE_MEMCPY_GDMA_THRESHOLD) {
		return 0;
	}
	if (len > UINT32_MAX) {
		rt_printf("[LARGE_MEMCPY_GDMA][FALLBACK] reason=len-overflow len=%u using=M33\r\n",
			  (unsigned int)len);
		return 0;
	}
	if (large_memcpy_in_interrupt()) {
		rt_printf("[LARGE_MEMCPY_GDMA][FALLBACK] reason=isr len=%u using=M33\r\n",
			  (unsigned int)len);
		return 0;
	}
	if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
		rt_printf("[LARGE_MEMCPY_GDMA][FALLBACK] reason=scheduler len=%u using=M33\r\n",
			  (unsigned int)len);
		return 0;
	}
	if ((((uintptr_t)destination ^ (uintptr_t)source) & 3U) != 0U) {
		rt_printf("[LARGE_MEMCPY_GDMA][FALLBACK] reason=alignment len=%u using=M33\r\n",
			  (unsigned int)len);
		return 0;
	}

	context = large_memcpy_gdma_context_acquire(0, NULL, NULL);
	if (context == NULL) {
		return 0;
	}
	__atomic_fetch_add(&large_memcpy_gdma_contiguous_attempts, 1U,
		__ATOMIC_RELAXED);

	owner = xTaskGetCurrentTaskHandle();
	original_priority = uxTaskPriorityGet(owner);
	if (original_priority < LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY) {
		vTaskPrioritySet(owner, LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY);
	}

	/* Isolate every DMA destination range to complete cache lines.  Because
	 * source and destination have the same modulo-4 alignment, aligning the
	 * destination also satisfies the board-verified source alignment rule. */
	prefix = (LARGE_MEMCPY_GDMA_CACHE_LINE -
		  ((uintptr_t)destination & (LARGE_MEMCPY_GDMA_CACHE_LINE - 1U))) &
		 (LARGE_MEMCPY_GDMA_CACHE_LINE - 1U);
	if (prefix > len) {
		prefix = (uint32_t)len;
	}
	destination += prefix;
	source += prefix;
	dma_len = ((uint32_t)len - prefix) &
		  ~(LARGE_MEMCPY_GDMA_CACHE_LINE - 1U);
	tail_len = (uint32_t)len - prefix - dma_len;

	/* Do not recursively call memcpy for the small edges.  They are copied only
	 * after all DMA blocks succeed, so an error simply returns to the wrapper,
	 * which performs one complete M33 copy from the original buffers. */
	while (dma_len != 0U) {
		/* hal_gdma_memcpy_config() selects a four-byte transfer width for
		 * these aligned, cache-line-sized ranges.  The HAL then divides this
		 * transaction into at most 16 linked descriptors and starts it once. */
		uint32_t transaction_len =
			dma_len > LARGE_MEMCPY_GDMA_TRANSACTION_MAX ?
			LARGE_MEMCPY_GDMA_TRANSACTION_MAX : dma_len;

		large_memcpy_gdma_drain_completion(context);
		hal_gdma_clean_pending_isr(&context->dma.hal_gdma_adaptor);
		context->irq_error = 0U;
		/* The destination was commonly zero-filled by malloc users and may
		 * therefore contain dirty cache lines.  If those lines are evicted
		 * while GDMA is running, stale zeros overwrite the DMA result in DRAM.
		 * Clean before handing ownership to GDMA, then invalidate the complete
		 * range after completion.  destination and transaction_len are both
		 * cache-line aligned, so unrelated caller data cannot be discarded. */
		phase_start_us = hal_read_curtime_us();
		if (context->dma.hal_gdma_adaptor.dcache_clean_by_addr !=
		    NULL) {
			uintptr_t source_cache_start =
				(uintptr_t)source &
				~(uintptr_t)(LARGE_MEMCPY_GDMA_CACHE_LINE - 1U);
			uint32_t source_cache_prefix =
				(uint32_t)((uintptr_t)source - source_cache_start);

			/* The ROM cache callback requires a cache-line-aligned start.
			 * source is only word aligned and can span one more line than
			 * transaction_len; omitting that final line lets GDMA read stale
			 * DRAM bytes at the end of the transfer. */
			context->dma.hal_gdma_adaptor.dcache_clean_by_addr(
				(uint32_t *)source_cache_start,
				(int32_t)(transaction_len + source_cache_prefix));
			context->dma.hal_gdma_adaptor.dcache_clean_by_addr(
				(uint32_t *)destination, (int32_t)transaction_len);
		}
		__DSB();
		cache_clean_us += hal_read_curtime_us() - phase_start_us;
		context->in_flight = 1U;
		phase_start_us = hal_read_curtime_us();
		if (hal_gdma_memcpy(&context->dma.hal_gdma_adaptor,
				    destination, (void *)source,
				    transaction_len) != HAL_OK) {
			context->in_flight = 0U;
			context->disabled_reason = "contiguous-submit";
			context->disabled_raw_error =
				context->dma.hal_gdma_adaptor.gdma_dev != NULL ?
				(context->dma.hal_gdma_adaptor.gdma_dev->raw_err &
				 (1U << context->dma.hal_gdma_adaptor.ch_num)) : 0U;
			rt_printf("[LARGE_MEMCPY_GDMA][FATAL] gdma=%u channel=%u "
				  "reason=%s raw=0x%02x; channel permanently disabled\r\n",
				  (unsigned int)context->dma.hal_gdma_adaptor.gdma_index,
				  (unsigned int)context->dma.hal_gdma_adaptor.ch_num,
				  context->disabled_reason,
				  (unsigned int)context->disabled_raw_error);
			hal_gdma_isr_dis(&context->dma.hal_gdma_adaptor);
			hal_gdma_abort(&context->dma.hal_gdma_adaptor);
			hal_gdma_clean_pending_isr(
				&context->dma.hal_gdma_adaptor);
			dma_memcpy_deinit(&context->dma);
			context->available = 0U;
			goto out;
		}
		if (large_memcpy_gdma_wait(context) != 0) {
			dma_wait_us += hal_read_curtime_us() - phase_start_us;
			goto out;
		}
		dma_wait_us += hal_read_curtime_us() - phase_start_us;
		phase_start_us = hal_read_curtime_us();
		if (context->dma.hal_gdma_adaptor.
		    dcache_invalidate_by_addr != NULL) {
			context->dma.hal_gdma_adaptor.
				dcache_invalidate_by_addr((uint32_t *)destination,
						  (int32_t)transaction_len);
		}
		__DSB();
		cache_invalidate_us += hal_read_curtime_us() - phase_start_us;
		destination += transaction_len;
		source += transaction_len;
		dma_len -= transaction_len;
	}

	phase_start_us = hal_read_curtime_us();
	if (prefix != 0U) {
		uint8_t *edge_dst = (uint8_t *)dst;
		const uint8_t *edge_src = (const uint8_t *)src;
		uint32_t i;

		for (i = 0U; i < prefix; ++i) {
			edge_dst[i] = edge_src[i];
		}
	}
	if (tail_len != 0U) {
		uint32_t i;

		for (i = 0U; i < tail_len; ++i) {
			destination[i] = source[i];
		}
	}
	cpu_edge_us += hal_read_curtime_us() - phase_start_us;

	copied = 1;

out:
	__atomic_fetch_add(&large_memcpy_gdma_contiguous_cache_clean_us,
		cache_clean_us, __ATOMIC_RELAXED);
	__atomic_fetch_add(&large_memcpy_gdma_contiguous_dma_wait_us,
		dma_wait_us, __ATOMIC_RELAXED);
	__atomic_fetch_add(&large_memcpy_gdma_contiguous_cache_invalidate_us,
		cache_invalidate_us, __ATOMIC_RELAXED);
	__atomic_fetch_add(&large_memcpy_gdma_contiguous_cpu_edge_us,
		cpu_edge_us, __ATOMIC_RELAXED);
	large_memcpy_gdma_atomic_max(
		&large_memcpy_gdma_contiguous_cache_clean_max_us, cache_clean_us);
	large_memcpy_gdma_atomic_max(
		&large_memcpy_gdma_contiguous_dma_wait_max_us, dma_wait_us);
	large_memcpy_gdma_atomic_max(
		&large_memcpy_gdma_contiguous_cache_invalidate_max_us,
		cache_invalidate_us);
	large_memcpy_gdma_atomic_max(
		&large_memcpy_gdma_contiguous_cpu_edge_max_us, cpu_edge_us);
	if (copied) {
		__atomic_fetch_add(&large_memcpy_gdma_contiguous_successes, 1U,
			__ATOMIC_RELAXED);
		__atomic_fetch_add(&large_memcpy_gdma_contiguous_bytes,
			(uint32_t)len, __ATOMIC_RELAXED);
	}
	large_memcpy_gdma_context_release(context);
	if (uxTaskPriorityGet(owner) != original_priority) {
		vTaskPrioritySet(owner, original_priority);
	}
	return copied;
}

static void large_memcpy_copyv_cpu(void *dst, const void *src, uint32_t len)
{
	volatile uint8_t *destination = (volatile uint8_t *)dst;
	volatile const uint8_t *source = (volatile const uint8_t *)src;

	/* This helper is used only for small cache-line edges in copyv.  Keep it
	 * completely independent from the globally wrapped memcpy symbol while the
	 * scatter/gather path is being board-validated.  Volatile accesses also make
	 * the diagnostic ordering relative to the following verification explicit. */
	while (len-- != 0U) {
		*destination++ = *source++;
	}
	__DSB();
}

static int large_memcpy_copyv_bus_accessible(uintptr_t start, uint32_t len)
{
	uintptr_t end = start + len;

	if (end < start) {
		return 0;
	}
	return ((start >= 0x20100000U && end <= 0x2017A000U) ||
		(start >= 0x60000000U && end <= 0x60800000U) ||
		(start >= 0x70000000U && end <= 0x72000000U));
}

static uint32_t large_memcpy_copyv_body(
	const carbox_gdma_copy_block_t *block, uint32_t *prefix_out)
{
	uintptr_t destination = (uintptr_t)block->dst;
	uintptr_t source = (uintptr_t)block->src;
	uint32_t prefix;
	uint32_t body;

	/* Generic BSD recv() callers may use a DTCM stack buffer.  GDMA cannot see
	 * DTCM, so only bus-visible SRAM/PSRAM/LPDDR ranges are eligible. */
	if (!large_memcpy_copyv_bus_accessible(destination, block->len) ||
	    !large_memcpy_copyv_bus_accessible(source, block->len)) {
		*prefix_out = 0U;
		return 0U;
	}

	prefix = (LARGE_MEMCPY_GDMA_CACHE_LINE -
		  (destination & (LARGE_MEMCPY_GDMA_CACHE_LINE - 1U))) &
		 (LARGE_MEMCPY_GDMA_CACHE_LINE - 1U);
	if (prefix >= block->len || ((source + prefix) & 3U) != 0U) {
		*prefix_out = 0U;
		return 0U;
	}
	/* The non-sequential ROM linked-list helper programs a 32-bit, 16-beat
	 * memory burst.  Unlike hal_gdma_memcpy(), it does not safely preserve the
	 * final partial burst.  Make every descriptor body a whole 64-byte burst;
	 * the remaining cache-line edge is copied by M33 after DMA completion. */
	body = (block->len - prefix) &
		~(LARGE_MEMCPY_GDMA_LINK_BURST_BYTES - 1U);
	if (body == 0U) {
		*prefix_out = 0U;
		return 0U;
	}
	*prefix_out = prefix;
	return body;
}

static void large_memcpy_gdma_disable_after_submit_error(
	large_memcpy_gdma_context_t *context, const char *reason)
{
	phal_gdma_adaptor_t adaptor = &context->dma.hal_gdma_adaptor;

	context->disabled_reason = reason;
	context->disabled_raw_error = adaptor->gdma_dev != NULL ?
		(adaptor->gdma_dev->raw_err & (1U << adaptor->ch_num)) : 0U;
	rt_printf("[LARGE_MEMCPY_GDMA][FATAL] gdma=%u channel=%u "
		  "reason=%s raw=0x%02x; channel permanently disabled\r\n",
		  (unsigned int)adaptor->gdma_index,
		  (unsigned int)adaptor->ch_num,
		  context->disabled_reason,
		  (unsigned int)context->disabled_raw_error);
	hal_gdma_isr_dis(adaptor);
	context->in_flight = 0U;
	hal_gdma_abort(adaptor);
	hal_gdma_clean_pending_isr(adaptor);
	dma_memcpy_deinit(&context->dma);
	context->available = 0U;
	large_memcpy_gdma_drain_completion(context);
}

static const char *large_memcpy_gdma_program_linked_channel(
	large_memcpy_gdma_context_t *context, uint8_t block_count)
{
	phal_gdma_adaptor_t adaptor;
	gdma_ch_lli_t *first;
	uint32_t channel_mask;

	if (context == NULL) {
		return "channel-invalid";
	}
	adaptor = &context->dma.hal_gdma_adaptor;
	if (adaptor->gdma_dev == NULL ||
	    adaptor->chnl_dev == NULL || adaptor->pgdma_ch_lli == NULL ||
	    block_count == 0U || block_count > MAX_MULTI_BLOCK_NUM ||
	    adaptor->ch_num < MIN_MULTI_CHNL_NUM ||
	    adaptor->ch_num >= MAX_GDMA_CHNL) {
		return "channel-invalid";
	}

	__atomic_fetch_add(&context->program_attempts, 1U, __ATOMIC_RELAXED);
	/* Mark the multi-word snapshot invalid while it is being replaced.  The
	 * periodic reporter uses acquire/release ordering so it never treats a
	 * partially updated snapshot as authoritative. */
	__atomic_store_n(&context->last_channel_valid, 0U, __ATOMIC_RELEASE);
	first = &adaptor->pgdma_ch_lli[0];
	context->last_expected_sar = first->sarx;
	context->last_expected_dar = first->darx;
	context->last_expected_llp = first->llpx;
	context->last_expected_ctl_low = first->ctlx_low;
	context->last_expected_ctl_up = first->ctlx_up;
	context->last_ch_en = adaptor->gdma_dev->ch_en_reg;
	context->last_actual_sar = adaptor->chnl_dev->sar;
	context->last_actual_dar = adaptor->chnl_dev->dar;
	context->last_actual_llp = adaptor->chnl_dev->llp;
	context->last_actual_ctl_low = adaptor->chnl_dev->ctl_low;
	context->last_actual_ctl_up = adaptor->chnl_dev->ctl_up;
	context->last_actual_cfg_low = adaptor->chnl_dev->cfg_low;
	__atomic_store_n(&context->last_channel_valid, 1U, __ATOMIC_RELEASE);

	channel_mask = 1U << adaptor->ch_num;
	if ((adaptor->gdma_dev->ch_en_reg & channel_mask) != 0U) {
		/* Reprogramming an active channel would redirect an in-flight transfer.
		 * The channel is retained exclusively by this wrapper, so this indicates
		 * an ownership/completion error and must not be recovered in place. */
		return "channel-active";
	}

	/* Use the same channel-programming sequence as the TCP receive SGDMA path in
	 * the last board-validated revision.  RX and TX are both memory-to-memory
	 * scatter copies; only their source/destination lists differ.  The direct
	 * register experiment used for TX diagnostics was not equivalent to this HAL
	 * operation: on RTL8195B its writes to SAR/DAR/CTL_UP were rejected even
	 * though LLP and CTL_LOW appeared writable.  Do not reset or hand-program the
	 * live channel here.  hal_gdma_chnl_block_setting() performs the controller's
	 * required internal load from the already validated LLI/adaptor state.
	 *
	 * Keep the readback snapshot for the ten-second report, but do not compare it
	 * byte-for-byte with LLI[0].  The HAL is allowed to encode the live first
	 * block from gdma_ctl while the following blocks are fetched from the LLI. */
	context->last_cfg_before_program = adaptor->chnl_dev->cfg_low;
	if (hal_gdma_chnl_block_setting(adaptor) != HAL_OK) {
		return "channel-config";
	}
	__DSB();
	__atomic_store_n(&context->last_channel_valid, 0U, __ATOMIC_RELEASE);
	context->last_ch_en = adaptor->gdma_dev->ch_en_reg;
	context->last_actual_sar = adaptor->chnl_dev->sar;
	context->last_actual_dar = adaptor->chnl_dev->dar;
	context->last_actual_llp = adaptor->chnl_dev->llp;
	context->last_actual_ctl_low = adaptor->chnl_dev->ctl_low;
	context->last_actual_ctl_up = adaptor->chnl_dev->ctl_up;
	context->last_actual_cfg_low = adaptor->chnl_dev->cfg_low;
	context->last_cfg_after_program = adaptor->chnl_dev->cfg_low;
	__atomic_store_n(&context->last_channel_valid, 1U, __ATOMIC_RELEASE);

	__atomic_fetch_add(&context->program_successes, 1U,
		__ATOMIC_RELAXED);
	return NULL;
}

static int large_memcpy_gdma_copyv_submit(
	large_memcpy_gdma_context_t *context,
	hal_gdma_block_t *blocks, uint8_t block_count,
	carbox_gdma_copyv_result_t *phase)
{
	phal_gdma_adaptor_t adaptor = &context->dma.hal_gdma_adaptor;
	uint32_t lli_cache_bytes;
	uint32_t expected_transfers;
	uint32_t descriptor_transfers;
	uint32_t source_width;
	uint32_t destination_width;
	uint32_t phase_start_us;
	const char *channel_error;
	uint8_t i;

	if (block_count == 0U || block_count > MAX_MULTI_BLOCK_NUM) {
		return -1;
	}

	large_memcpy_gdma_drain_completion(context);
	hal_gdma_clean_pending_isr(adaptor);
	context->irq_error = 0U;

	/* The stock hal_gdma_multiblk_memcpy() derives one cache range from the
	 * first and last source address.  That is invalid for fragmented pbufs.
	 * Maintain every source and destination block independently instead. */
	phase_start_us = hal_read_curtime_us();
	for (i = 0U; i < block_count; ++i) {
		if (adaptor->dcache_clean_by_addr != NULL) {
			uintptr_t source = (uintptr_t)blocks[i].src_addr;
			uintptr_t source_cache_start = source &
				~(uintptr_t)(LARGE_MEMCPY_GDMA_CACHE_LINE - 1U);
			uint32_t source_cache_prefix =
				(uint32_t)(source - source_cache_start);

			/* Cache maintenance APIs require a 32-byte-aligned address.
			 * Each pbuf source is only word aligned; include its leading
			 * offset so the loop also cleans the final cache line touched by
			 * this DMA block. */
			adaptor->dcache_clean_by_addr(
				(uint32_t *)source_cache_start,
				(int32_t)(blocks[i].block_length +
					  source_cache_prefix));
			adaptor->dcache_clean_by_addr(
				(uint32_t *)(uintptr_t)blocks[i].dst_addr,
				(int32_t)blocks[i].block_length);
		}
	}
	__DSB();
	if (phase != NULL) {
		phase->cache_clean_us += hal_read_curtime_us() - phase_start_us;
	}

	/* The ROM linked-list helper interprets block_length using the adaptor's
	 * common transfer width.  Realtek's own non-sequential multi-block example
	 * uses the byte-transfer defaults established by hal_gdma_memcpy_init() and
	 * passes block lengths in bytes.  This adaptor is also used by the ordinary
	 * hal_gdma_memcpy() path, which can leave four-byte width state behind.
	 * Restore the documented multi-block baseline before every LLI build so a
	 * 1460-byte pbuf always means exactly 1460 byte transfers.  Calling the
	 * single-block hal_gdma_memcpy_config() here is invalid: board testing showed
	 * that the ROM linked-list helper rejects that resulting state. */
	adaptor->gdma_ctl.tt_fc = TTFCMemToMem;
	adaptor->gdma_ctl.src_tr_width = TrWidthOneByte;
	adaptor->gdma_ctl.dst_tr_width = TrWidthOneByte;
	adaptor->gdma_ctl.src_msize = MsizeOne;
	adaptor->gdma_ctl.dest_msize = MsizeOne;
	adaptor->gdma_ctl.sinc = IncType;
	adaptor->gdma_ctl.dinc = IncType;
	adaptor->gdma_ctl.block_size = 0U;
	adaptor->gdma_ctl.int_en = 1U;

	if (hal_gdma_linked_list_block_config(adaptor, blocks,
					       block_count) != HAL_OK) {
		large_memcpy_gdma_disable_after_submit_error(context,
			"descriptor-config");
		return -1;
	}

	/* Use the ROM helper only as an address/link constructor.  Board diagnostics
	 * have verified its SAR/DAR/LLP output, but its CTL words are not trusted:
	 * they can retain transfer-width state from a preceding ordinary memcpy.
	 * Compose the complete CTL contract ourselves instead of preserving any
	 * ROM-generated fields.  0x18000000 means memory-to-memory, byte width,
	 * incrementing addresses, one-item bursts, and source/destination LLP.
	 * BLOCK_TS is therefore exactly the byte length.
	 *
	 * INT_EN must only be set on the final LLI.  The ROM helper sets it on every
	 * item, which wakes the completion semaphore after the first block while the
	 * channel is still following LLP.  Besides producing false completion, that
	 * lets the ROM IRQ path touch a live linked transfer repeatedly. */
	for (i = 0U; i < block_count; ++i) {
		adaptor->pgdma_ch_lli[i].ctlx_low =
			LARGE_MEMCPY_GDMA_CTL_LLP_ENABLE;
		if (i + 1U == block_count) {
			adaptor->pgdma_ch_lli[i].ctlx_low |=
				LARGE_MEMCPY_GDMA_CTL_INT_EN;
		}
		adaptor->pgdma_ch_lli[i].ctlx_up = blocks[i].block_length;
	}

	/* Validate the ROM-generated descriptors before hardware can consume them.
	 * BLOCK_TS is expressed in source transfers, not bytes.  A malformed LLI is
	 * a deterministic configuration failure and must fall back to M33 instead of
	 * producing a partially copied socket buffer. */
	for (i = 0U; i < block_count; ++i) {
		uint32_t expected_control = LARGE_MEMCPY_GDMA_CTL_LLP_ENABLE |
			(i + 1U == block_count ? LARGE_MEMCPY_GDMA_CTL_INT_EN : 0U);

		source_width = (adaptor->pgdma_ch_lli[i].ctlx_low >> 4) & 0x7U;
		destination_width =
			(adaptor->pgdma_ch_lli[i].ctlx_low >> 1) & 0x7U;
		descriptor_transfers =
			adaptor->pgdma_ch_lli[i].ctlx_up & 0xFFFU;
		if (adaptor->pgdma_ch_lli[i].ctlx_low != expected_control) {
			rt_printf("[LARGE_MEMCPY_GDMA][FATAL] descriptor=%u "
				  "CTL=%08x/%08x\r\n",
				  (unsigned int)i,
				  (unsigned int)adaptor->pgdma_ch_lli[i].ctlx_low,
				  (unsigned int)expected_control);
			large_memcpy_gdma_disable_after_submit_error(context,
				"descriptor-control");
			return -1;
		}
		if (source_width > TrWidthFourBytes ||
		    destination_width != source_width ||
		    (blocks[i].block_length & ((1U << source_width) - 1U)) != 0U) {
			large_memcpy_gdma_disable_after_submit_error(context,
				"descriptor-width");
			return -1;
		}
		expected_transfers = blocks[i].block_length >> source_width;
		if (descriptor_transfers != expected_transfers) {
			large_memcpy_gdma_disable_after_submit_error(context,
				"descriptor-length");
			return -1;
		}
		if (adaptor->pgdma_ch_lli[i].sarx != blocks[i].src_addr ||
		    adaptor->pgdma_ch_lli[i].darx != blocks[i].dst_addr) {
			rt_printf("[LARGE_MEMCPY_GDMA][FATAL] descriptor=%u "
				  "SAR=%08x/%08x DAR=%08x/%08x\r\n",
				  (unsigned int)i,
				  (unsigned int)adaptor->pgdma_ch_lli[i].sarx,
				  (unsigned int)blocks[i].src_addr,
				  (unsigned int)adaptor->pgdma_ch_lli[i].darx,
				  (unsigned int)blocks[i].dst_addr);
			large_memcpy_gdma_disable_after_submit_error(context,
				"descriptor-address");
			return -1;
		}
		{
			uint32_t expected_llp = i + 1U < block_count ?
				(uint32_t)(uintptr_t)&adaptor->pgdma_ch_lli[i + 1U] : 0U;

			if (adaptor->pgdma_ch_lli[i].llpx != expected_llp) {
				rt_printf("[LARGE_MEMCPY_GDMA][FATAL] descriptor=%u "
					  "LLP=%08x/%08x\r\n",
					  (unsigned int)i,
					  (unsigned int)adaptor->pgdma_ch_lli[i].llpx,
					  (unsigned int)expected_llp);
				large_memcpy_gdma_disable_after_submit_error(context,
					"descriptor-link");
				return -1;
			}
		}
	}
	if (!large_memcpy_gdma_lli_reported) {
		gdma_ch_lli_t *first = &adaptor->pgdma_ch_lli[0];
		gdma_ch_lli_t *last = &adaptor->pgdma_ch_lli[block_count - 1U];

		large_memcpy_gdma_lli_reported = 1U;
		rt_printf("[RECVGDMA][LLI] validated blocks=%u base=%p "
			  "first sar/dar/llp=%08x/%08x/%08x len=%u ctl=%08x/%08x "
			  "last sar/dar/llp=%08x/%08x/%08x len=%u ctl=%08x/%08x\r\n",
			  (unsigned int)block_count,
			  adaptor->pgdma_ch_lli,
			  (unsigned int)first->sarx,
			  (unsigned int)first->darx,
			  (unsigned int)first->llpx,
			  (unsigned int)blocks[0].block_length,
			  (unsigned int)first->ctlx_low,
			  (unsigned int)first->ctlx_up,
			  (unsigned int)last->sarx,
			  (unsigned int)last->darx,
			  (unsigned int)last->llpx,
			  (unsigned int)blocks[block_count - 1U].block_length,
			  (unsigned int)last->ctlx_low,
			  (unsigned int)last->ctlx_up);
	}
	/* Hardware reads the persistent LLI array, not the temporary block list.
	 * Cache APIs require both an aligned start and a cache-line-sized range.
	 * gdma_ch_lli_t is 20 bytes, so block_count * sizeof(LLI) usually is not a
	 * cache-line multiple.  Round it up, publish it, then invalidate and read it
	 * back below.  The second validation therefore checks the DRAM image that
	 * GDMA will consume instead of merely checking the CPU's dirty cache copy. */
	lli_cache_bytes = (block_count * sizeof(gdma_ch_lli_t) +
		LARGE_MEMCPY_GDMA_CACHE_LINE - 1U) &
		~(LARGE_MEMCPY_GDMA_CACHE_LINE - 1U);
	if (adaptor->dcache_clean_by_addr != NULL) {
		phase_start_us = hal_read_curtime_us();
		adaptor->dcache_clean_by_addr((uint32_t *)adaptor->pgdma_ch_lli,
			(int32_t)lli_cache_bytes);
		__DSB();
		if (phase != NULL) {
			phase->cache_clean_us +=
				hal_read_curtime_us() - phase_start_us;
		}
	}
	if (adaptor->dcache_invalidate_by_addr != NULL) {
		phase_start_us = hal_read_curtime_us();
		adaptor->dcache_invalidate_by_addr(
			(uint32_t *)adaptor->pgdma_ch_lli,
			(int32_t)lli_cache_bytes);
		__DSB();
		if (phase != NULL) {
			phase->cache_invalidate_us +=
				hal_read_curtime_us() - phase_start_us;
		}
	}

	for (i = 0U; i < block_count; ++i) {
		uint32_t expected_control = LARGE_MEMCPY_GDMA_CTL_LLP_ENABLE |
			(i + 1U == block_count ? LARGE_MEMCPY_GDMA_CTL_INT_EN : 0U);
		uint32_t expected_llp = i + 1U < block_count ?
			(uint32_t)(uintptr_t)&adaptor->pgdma_ch_lli[i + 1U] : 0U;

		source_width = (adaptor->pgdma_ch_lli[i].ctlx_low >> 4) & 0x7U;
		descriptor_transfers =
			adaptor->pgdma_ch_lli[i].ctlx_up & 0xFFFU;
		expected_transfers = blocks[i].block_length >> source_width;
		if (adaptor->pgdma_ch_lli[i].sarx != blocks[i].src_addr ||
		    adaptor->pgdma_ch_lli[i].darx != blocks[i].dst_addr ||
		    adaptor->pgdma_ch_lli[i].llpx != expected_llp ||
		    adaptor->pgdma_ch_lli[i].ctlx_low != expected_control ||
		    descriptor_transfers != expected_transfers) {
			rt_printf("[LARGE_MEMCPY_GDMA][FATAL] descriptor-cache=%u "
				  "SAR=%08x/%08x DAR=%08x/%08x LLP=%08x/%08x "
				  "CTL=%08x/%08x LEN=%u/%u\r\n",
				  (unsigned int)i,
				  (unsigned int)adaptor->pgdma_ch_lli[i].sarx,
				  (unsigned int)blocks[i].src_addr,
				  (unsigned int)adaptor->pgdma_ch_lli[i].darx,
				  (unsigned int)blocks[i].dst_addr,
				  (unsigned int)adaptor->pgdma_ch_lli[i].llpx,
				  (unsigned int)expected_llp,
				  (unsigned int)adaptor->pgdma_ch_lli[i].ctlx_low,
				  (unsigned int)expected_control,
				  (unsigned int)descriptor_transfers,
				  (unsigned int)expected_transfers);
			large_memcpy_gdma_disable_after_submit_error(context,
				"descriptor-cache-readback");
			return -1;
		}
	}

	context->in_flight = 1U;
	hal_gdma_on(adaptor);
	/* Match the RX-proven order: publish the completion route before programming
	 * and enabling the linked channel. */
	hal_gdma_isr_en(adaptor);
	channel_error = large_memcpy_gdma_program_linked_channel(context,
							block_count);
	if (channel_error != NULL) {
		context->in_flight = 0U;
		large_memcpy_gdma_disable_after_submit_error(context,
			channel_error);
		return -1;
	}
	if (!large_memcpy_gdma_channel_reported) {
		large_memcpy_gdma_channel_reported = 1U;
		rt_printf("[RECVGDMA][CHANNEL] mode=rx-validated-hal gdma=%u ch=%u "
			  "sar/dar/llp=%08x/%08x/%08x ctl=%08x/%08x "
			  "expected=%08x/%08x/%08x ctl=%08x/%08x\r\n",
			  (unsigned int)adaptor->gdma_index,
			  (unsigned int)adaptor->ch_num,
			  (unsigned int)adaptor->chnl_dev->sar,
			  (unsigned int)adaptor->chnl_dev->dar,
			  (unsigned int)adaptor->chnl_dev->llp,
			  (unsigned int)adaptor->chnl_dev->ctl_low,
			  (unsigned int)adaptor->chnl_dev->ctl_up,
			  (unsigned int)adaptor->pgdma_ch_lli[0].sarx,
			  (unsigned int)adaptor->pgdma_ch_lli[0].darx,
			  (unsigned int)adaptor->pgdma_ch_lli[0].llpx,
			  (unsigned int)adaptor->pgdma_ch_lli[0].ctlx_low,
			  (unsigned int)adaptor->pgdma_ch_lli[0].ctlx_up);
	}
	phase_start_us = hal_read_curtime_us();
	hal_gdma_chnl_en(adaptor);
	if (large_memcpy_gdma_wait(context) != 0) {
		if (phase != NULL) {
			phase->dma_wait_us += hal_read_curtime_us() - phase_start_us;
		}
		__atomic_fetch_add(&context->completion_failures, 1U,
			__ATOMIC_RELAXED);
		return -1;
	}
	if (phase != NULL) {
		phase->dma_wait_us += hal_read_curtime_us() - phase_start_us;
	}
	__atomic_fetch_add(&context->completion_successes, 1U,
		__ATOMIC_RELAXED);

	phase_start_us = hal_read_curtime_us();
	for (i = 0U; i < block_count; ++i) {
		if (adaptor->dcache_invalidate_by_addr != NULL) {
			adaptor->dcache_invalidate_by_addr(
				(uint32_t *)(uintptr_t)blocks[i].dst_addr,
				(int32_t)blocks[i].block_length);
		}
	}
	__DSB();
	if (phase != NULL) {
		phase->cache_invalidate_us +=
			hal_read_curtime_us() - phase_start_us;
	}
	return 0;
}

#if LARGE_MEMCPY_GDMA_COPYV_VERIFY
/* Diagnostic validation for the new socket-recv scatter/gather path.  Volatile
 * byte reads deliberately keep this independent from libc memcmp/memcpy and
 * force the M33 to observe destination memory after cache invalidation. */
static int large_memcpy_gdma_copyv_verify(
	const carbox_gdma_copy_block_t *blocks, size_t block_count)
{
	size_t block_index;

	for (block_index = 0U; block_index < block_count; ++block_index) {
		volatile const uint8_t *source =
			(volatile const uint8_t *)blocks[block_index].src;
		volatile const uint8_t *destination =
			(volatile const uint8_t *)blocks[block_index].dst;
		uint32_t offset;

		for (offset = 0U; offset < blocks[block_index].len; ++offset) {
			uint8_t expected = source[offset];
			uint8_t actual = destination[offset];

			if (expected != actual) {
				rt_printf("[RECVGDMA][VERIFY] mismatch block=%u/%u "
					  "len=%u offset=%u expected=0x%02x "
					  "actual=0x%02x; recopying batch with M33\r\n",
					  (unsigned int)block_index,
					  (unsigned int)block_count,
					  (unsigned int)blocks[block_index].len,
					  (unsigned int)offset,
					  (unsigned int)expected,
					  (unsigned int)actual);
				return -1;
			}
		}
	}
	return 0;
}
#endif

static int large_memcpy_gdma_copyv(const carbox_gdma_copy_block_t *blocks,
				   size_t block_count,
				   carbox_gdma_copyv_result_t *result,
				   int wait_for_channel,
				   int bypass_size_threshold)
{
	large_memcpy_gdma_context_t *context = NULL;
	hal_gdma_block_t dma_blocks[MAX_MULTI_BLOCK_NUM];
	TaskHandle_t owner;
	UBaseType_t original_priority;
	uint64_t total_bytes = 0U;
	uint64_t eligible_bytes = 0U;
	size_t i;
	uint8_t dma_count = 0U;
	uint8_t channel_waited = 0U;
	uint32_t channel_wait_us = 0U;
	uint32_t dma_bytes = 0U;
	uint32_t cpu_bytes = 0U;
	uint16_t dma_block_count = 0U;
	uint16_t dma_batch_count = 0U;
	int copied = 0;

	if (result != NULL) {
		result->dma_bytes = 0U;
		result->cpu_edge_bytes = 0U;
		result->channel_wait_us = 0U;
		result->cache_clean_us = 0U;
		result->dma_wait_us = 0U;
		result->cache_invalidate_us = 0U;
		result->cpu_edge_us = 0U;
		result->dma_blocks = 0U;
		result->dma_batches = 0U;
		result->channel_waited = 0U;
	}
	if (blocks == NULL || block_count == 0U ||
	    large_memcpy_in_interrupt() ||
	    xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
		return 0;
	}

	/* First pass must not modify caller memory.  It decides whether enough
	 * aligned body data exists to amortize one linked-DMA transaction. */
	for (i = 0U; i < block_count; ++i) {
		uint32_t prefix;
		uint32_t body;

		if (blocks[i].len == 0U || blocks[i].dst == NULL ||
		    blocks[i].src == NULL) {
			continue;
		}
		total_bytes += blocks[i].len;
		body = large_memcpy_copyv_body(&blocks[i], &prefix);
		eligible_bytes += body;
	}
	if (eligible_bytes == 0U ||
	    (!bypass_size_threshold &&
	     (total_bytes <= LARGE_MEMCPY_GDMA_THRESHOLD ||
	      eligible_bytes <= LARGE_MEMCPY_GDMA_THRESHOLD))) {
		return 0;
	}

	context = large_memcpy_gdma_context_acquire(wait_for_channel,
		&channel_waited, &channel_wait_us);
	if (context == NULL) {
		return 0;
	}
	if (result != NULL) {
		result->channel_waited = channel_waited;
		result->channel_wait_us = channel_wait_us;
	}

	owner = xTaskGetCurrentTaskHandle();
	original_priority = uxTaskPriorityGet(owner);
	if (original_priority < LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY) {
		vTaskPrioritySet(owner, LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY);
	}

	for (i = 0U; i < block_count; ++i) {
		uint8_t *destination = (uint8_t *)blocks[i].dst;
		const uint8_t *source = (const uint8_t *)blocks[i].src;
		uint32_t prefix;
		uint32_t body;
		uint32_t tail;

		if (blocks[i].len == 0U) {
			continue;
		}
		body = large_memcpy_copyv_body(&blocks[i], &prefix);
		if (body == 0U) {
			cpu_bytes += blocks[i].len;
			continue;
		}
		tail = blocks[i].len - prefix - body;
		cpu_bytes += prefix + tail;
		destination += prefix;
		source += prefix;

		while (body != 0U) {
			uint32_t chunk = body > LARGE_MEMCPY_GDMA_LINK_BLOCK_BYTES ?
				LARGE_MEMCPY_GDMA_LINK_BLOCK_BYTES : body;
			hal_gdma_block_t *dma_block = &dma_blocks[dma_count++];

			dma_block->src_addr = (uint32_t)(uintptr_t)source;
			dma_block->dst_addr = (uint32_t)(uintptr_t)destination;
			dma_block->block_length = chunk;
			dma_block->src_offset = 0U;
			dma_block->dst_offset = 0U;
			dma_bytes += chunk;
			dma_block_count++;
			source += chunk;
			destination += chunk;
			body -= chunk;

			if (dma_count == MAX_MULTI_BLOCK_NUM) {
				if (large_memcpy_gdma_copyv_submit(context,
						dma_blocks, dma_count, result) != 0) {
					goto out;
				}
				dma_batch_count++;
				dma_count = 0U;
			}
		}
	}
	if (dma_count != 0U) {
		if (large_memcpy_gdma_copyv_submit(context, dma_blocks,
					    dma_count, result) != 0) {
			goto out;
		}
		dma_batch_count++;
	}

	/* Do one final visibility pass after the complete logical copyv, not merely
	 * after each hardware batch.  Board diagnostics showed the stale byte moving
	 * exactly with the final DMA cache line when the body boundary changed,
	 * indicating that the ROM per-batch invalidate can leave that line resident.
	 * CPU-owned edges have not been written yet, so invalidating every complete
	 * DMA body again cannot discard caller data. */
	{
		uint32_t phase_start_us = hal_read_curtime_us();

	for (i = 0U; i < block_count; ++i) {
		uint8_t *destination = (uint8_t *)blocks[i].dst;
		uint32_t prefix;
		uint32_t body;

		if (blocks[i].len == 0U) {
			continue;
		}
		body = large_memcpy_copyv_body(&blocks[i], &prefix);
		if (body != 0U &&
		    context->dma.hal_gdma_adaptor.dcache_invalidate_by_addr != NULL) {
			context->dma.hal_gdma_adaptor.dcache_invalidate_by_addr(
				(uint32_t *)(destination + prefix), (int32_t)body);
		}
	}
	__DSB();
		if (result != NULL) {
			result->cache_invalidate_us +=
				hal_read_curtime_us() - phase_start_us;
		}
	}

	/* Cache invalidate helpers are allowed to operate at cache-line
	 * granularity.  Do not write CPU-owned prefix/tail bytes before any DMA
	 * invalidate, even though the DMA body itself is cache-line isolated: some
	 * ROM implementations include the line at the exclusive end address.  Copy
	 * every CPU edge only after all DMA batches and invalidations complete. */
	{
		uint32_t phase_start_us = hal_read_curtime_us();

	for (i = 0U; i < block_count; ++i) {
		uint8_t *destination = (uint8_t *)blocks[i].dst;
		const uint8_t *source = (const uint8_t *)blocks[i].src;
		uint32_t prefix;
		uint32_t body;
		uint32_t tail;

		if (blocks[i].len == 0U) {
			continue;
		}
		body = large_memcpy_copyv_body(&blocks[i], &prefix);
		if (body == 0U) {
			large_memcpy_copyv_cpu(destination, source, blocks[i].len);
			continue;
		}
		tail = blocks[i].len - prefix - body;
		if (prefix != 0U) {
			large_memcpy_copyv_cpu(destination, source, prefix);
		}
		if (tail != 0U) {
			large_memcpy_copyv_cpu(destination + prefix + body,
				source + prefix + body, tail);
		}
	}
		if (result != NULL) {
			result->cpu_edge_us += hal_read_curtime_us() - phase_start_us;
		}
	}
#if LARGE_MEMCPY_GDMA_COPYV_VERIFY
	if (large_memcpy_gdma_copyv_verify(blocks, block_count) != 0) {
		/* Returning zero invokes the socket layer's existing full-batch M33
		 * fallback while every source pbuf is still owned and valid. */
		goto out;
	}
#endif
	copied = 1;
	if (result != NULL) {
		result->dma_bytes = dma_bytes;
		result->cpu_edge_bytes = cpu_bytes;
		result->dma_blocks = dma_block_count;
		result->dma_batches = dma_batch_count;
	}

out:
	large_memcpy_gdma_context_release(context);
	if (uxTaskPriorityGet(owner) != original_priority) {
		vTaskPrioritySet(owner, original_priority);
	}
	return copied;
}

int carbox_linked_gdma_copyv_try(const carbox_gdma_copy_block_t *blocks,
				 size_t block_count,
				 carbox_gdma_copyv_result_t *result)
{
	return large_memcpy_gdma_copyv(blocks, block_count, result, 0, 0);
}

int carbox_linked_gdma_copyv_force_try(
	const carbox_gdma_copy_block_t *blocks, size_t block_count,
	carbox_gdma_copyv_result_t *result)
{
	return large_memcpy_gdma_copyv(blocks, block_count, result, 0, 1);
}

int carbox_linked_gdma_copyv_wait(const carbox_gdma_copy_block_t *blocks,
				  size_t block_count,
				  carbox_gdma_copyv_result_t *result)
{
	return large_memcpy_gdma_copyv(blocks, block_count, result, 1, 0);
}

#else

void carbox_large_memcpy_gdma_init(void)
{
}

void carbox_large_memcpy_gdma_report(uint32_t sequence)
{
	(void)sequence;
}

int carbox_large_memcpy_gdma_try(void *dst, const void *src, size_t len)
{
	(void)dst;
	(void)src;
	(void)len;
	return 0;
}

int carbox_linked_gdma_copyv_try(const carbox_gdma_copy_block_t *blocks,
				 size_t block_count,
				 carbox_gdma_copyv_result_t *result)
{
	(void)blocks;
	(void)block_count;
	if (result != NULL) {
		result->dma_bytes = 0U;
		result->cpu_edge_bytes = 0U;
		result->channel_wait_us = 0U;
		result->cache_clean_us = 0U;
		result->dma_wait_us = 0U;
		result->cache_invalidate_us = 0U;
		result->cpu_edge_us = 0U;
		result->dma_blocks = 0U;
		result->dma_batches = 0U;
		result->channel_waited = 0U;
	}
	return 0;
}

int carbox_linked_gdma_copyv_wait(const carbox_gdma_copy_block_t *blocks,
				  size_t block_count,
				  carbox_gdma_copyv_result_t *result)
{
	return carbox_linked_gdma_copyv_try(blocks, block_count, result);
}

int carbox_linked_gdma_copyv_force_try(
	const carbox_gdma_copy_block_t *blocks, size_t block_count,
	carbox_gdma_copyv_result_t *result)
{
	return carbox_linked_gdma_copyv_try(blocks, block_count, result);
}

#endif /* CONFIG_LARGE_MEMCPY_GDMA */
