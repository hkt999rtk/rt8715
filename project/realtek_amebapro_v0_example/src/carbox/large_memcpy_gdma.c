#include "large_memcpy_gdma.h"

#include "cmsis.h"
#include "diag.h"
#include "dma_api.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <stdint.h>

#ifndef CONFIG_LARGE_MEMCPY_GDMA
#define CONFIG_LARGE_MEMCPY_GDMA 0
#endif

#ifndef LARGE_MEMCPY_GDMA_THRESHOLD
#define LARGE_MEMCPY_GDMA_THRESHOLD 4096U
#endif

#ifndef LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY
#define LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY 11U
#endif

#if LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY >= configMAX_PRIORITIES
#error "LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY must be below configMAX_PRIORITIES"
#endif

#define LARGE_MEMCPY_GDMA_CACHE_LINE 32U
/* One linked descriptor holds 4095 transfers and the HAL has storage for 16
 * descriptors.  The DMA body is word-aligned, so each transfer is four bytes.
 * Larger memcpy requests are intentionally split into multiple linked-list
 * transactions instead of relying on undocumented HAL overflow behaviour. */
#define LARGE_MEMCPY_GDMA_TRANSACTION_MAX \
	(MAX_DMA_BLOCK_SIZE * MAX_MULTI_BLOCK_NUM * sizeof(uint32_t))
#define LARGE_MEMCPY_GDMA_TIMEOUT_MS 20U
#define LARGE_MEMCPY_GDMA_CONTEXT_COUNT 2U

#if CONFIG_LARGE_MEMCPY_GDMA

typedef struct large_memcpy_gdma_context_s {
	gdma_t dma;
	SemaphoreHandle_t done;
	volatile uint8_t busy;
	volatile uint8_t in_flight;
	volatile uint8_t irq_error;
	uint8_t available;
} large_memcpy_gdma_context_t;

static large_memcpy_gdma_context_t large_memcpy_gdma[LARGE_MEMCPY_GDMA_CONTEXT_COUNT];
static uint8_t large_memcpy_gdma_initialized;

static int large_memcpy_in_interrupt(void)
{
	uint32_t ipsr;

	__asm volatile("mrs %0, ipsr" : "=r" (ipsr));
	return ipsr != 0U;
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

static const dma_irq_handler large_memcpy_gdma_callbacks[] = {
	large_memcpy_gdma_done0,
	large_memcpy_gdma_done1,
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
		return -1;
	}

	/* dma_memcpy_init() starts with an ordinary channel.  Convert it once at
	 * boot to channel 4/5 and retain it, so the first frame cannot trigger a
	 * hidden free/reallocate operation inside hal_gdma_memcpy(). */
	if (adaptor->ch_num < MIN_MULTI_CHNL_NUM) {
		if (hal_gdma_chnl_alloc(adaptor, MultiBlkEn) != HAL_OK) {
			return -1;
		}
		hal_gdma_chnl_init(adaptor);
		hal_gdma_irq_reg(adaptor,
			(irq_handler_t)hal_gdma_stubs.hal_gdma_memcpy_irq_handler,
			adaptor);
		hal_gdma_memcpy_irq_hook(adaptor,
			(gdma_callback_t)callback, adaptor);
	}
	if (adaptor->ch_num < MIN_MULTI_CHNL_NUM ||
	    adaptor->ch_num >= MAX_GDMA_CHNL || adaptor->pgdma_ch_lli == NULL) {
		if (adaptor->have_chnl) {
			dma_memcpy_deinit(&context->dma);
		}
		return -1;
	}
	large_memcpy_gdma_configure_irq(context);
	context->available = 1U;
	return 0;
}

void carbox_large_memcpy_gdma_init(void)
{
	unsigned int available = 0U;
	unsigned int i;

	if (large_memcpy_gdma_initialized) {
		return;
	}
	large_memcpy_gdma_initialized = 1U;

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
		++available;
		rt_printf("[LARGE_MEMCPY_GDMA] slot=%u ready gdma=%u channel=%u\r\n",
			  i, (unsigned int)adaptor->gdma_index,
			  (unsigned int)adaptor->ch_num);
	}

	rt_printf("[LARGE_MEMCPY_GDMA] available=%u threshold=>%u "
		  "linked_blocks=%u transaction_max=%u\r\n",
		  available,
		  (unsigned int)LARGE_MEMCPY_GDMA_THRESHOLD,
		  (unsigned int)MAX_MULTI_BLOCK_NUM,
		  (unsigned int)LARGE_MEMCPY_GDMA_TRANSACTION_MAX);
}

static int large_memcpy_gdma_wait(large_memcpy_gdma_context_t *context)
{
	phal_gdma_adaptor_t adaptor = &context->dma.hal_gdma_adaptor;
	uint32_t channel_mask = 1U << adaptor->ch_num;
	uint32_t raw_error;

	if (xSemaphoreTake(context->done,
			   pdMS_TO_TICKS(LARGE_MEMCPY_GDMA_TIMEOUT_MS)) == pdTRUE &&
	    !context->irq_error) {
		return 0;
	}

	raw_error = adaptor->gdma_dev != NULL ?
		(adaptor->gdma_dev->raw_err & channel_mask) : 0U;
	rt_printf("[LARGE_MEMCPY_GDMA][ERROR] %s gdma=%u channel=%u raw_err=0x%02x; "
		  "current and future copies use M33\r\n",
		  context->irq_error ? "transfer error" : "timeout",
		  (unsigned int)adaptor->gdma_index,
		  (unsigned int)adaptor->ch_num,
		  (unsigned int)raw_error);

	/* memcpy cannot return an error.  Quiesce the channel before the wrapper
	 * recopies the complete original range with M33, otherwise a late DMA write
	 * could corrupt the CPU fallback result.  Keep GDMA disabled after a runtime
	 * fault rather than risking an unvalidated recovery path. */
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
	unsigned int i;
	unsigned int any_available = 0U;
	uint32_t prefix;
	uint32_t dma_len;
	uint32_t tail_len;
	int copied = 0;

	/* Two persistent linked-channel contexts are claimed non-blockingly.  This
	 * lets ScreenThread and AirPlayScreenReceiver overlap one large copy each.
	 * M33 is used only when both usable contexts are already occupied (or the
	 * request is unsuitable for DMA), rather than serializing either task. */
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

	taskENTER_CRITICAL();
	for (i = 0U; i < LARGE_MEMCPY_GDMA_CONTEXT_COUNT; ++i) {
		if (!large_memcpy_gdma[i].available) {
			continue;
		}
		any_available = 1U;
		if (!large_memcpy_gdma[i].busy) {
			large_memcpy_gdma[i].busy = 1U;
			context = &large_memcpy_gdma[i];
			break;
		}
	}
	taskEXIT_CRITICAL();
	if (context == NULL) {
		rt_printf("[LARGE_MEMCPY_GDMA][FALLBACK] reason=%s len=%u using=M33\r\n",
			  any_available ? "all-busy" : "unavailable",
			  (unsigned int)len);
		return 0;
	}

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
		if (context->dma.hal_gdma_adaptor.dcache_clean_by_addr !=
		    NULL) {
			context->dma.hal_gdma_adaptor.dcache_clean_by_addr(
				(uint32_t *)destination, (int32_t)transaction_len);
		}
		__DSB();
		context->in_flight = 1U;
		if (hal_gdma_memcpy(&context->dma.hal_gdma_adaptor,
				    destination, (void *)source,
				    transaction_len) != HAL_OK) {
			context->in_flight = 0U;
			rt_printf("[LARGE_MEMCPY_GDMA][ERROR] submit failed len=%u; "
				  "disabling GDMA\r\n",
				  (unsigned int)transaction_len);
			hal_gdma_isr_dis(&context->dma.hal_gdma_adaptor);
			hal_gdma_abort(&context->dma.hal_gdma_adaptor);
			hal_gdma_clean_pending_isr(
				&context->dma.hal_gdma_adaptor);
			dma_memcpy_deinit(&context->dma);
			context->available = 0U;
			rt_printf("[LARGE_MEMCPY_GDMA][FALLBACK] reason=submit len=%u using=M33\r\n",
				  (unsigned int)len);
			goto out;
		}
		if (large_memcpy_gdma_wait(context) != 0) {
			rt_printf("[LARGE_MEMCPY_GDMA][FALLBACK] reason=completion len=%u using=M33\r\n",
				  (unsigned int)len);
			goto out;
		}
		if (context->dma.hal_gdma_adaptor.
		    dcache_invalidate_by_addr != NULL) {
			context->dma.hal_gdma_adaptor.
				dcache_invalidate_by_addr((uint32_t *)destination,
						  (int32_t)transaction_len);
		}
		__DSB();
		destination += transaction_len;
		source += transaction_len;
		dma_len -= transaction_len;
	}

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

	copied = 1;

out:
	context->busy = 0U;
	if (uxTaskPriorityGet(owner) != original_priority) {
		vTaskPrioritySet(owner, original_priority);
	}
	return copied;
}

#else

void carbox_large_memcpy_gdma_init(void)
{
}

int carbox_large_memcpy_gdma_try(void *dst, const void *src, size_t len)
{
	(void)dst;
	(void)src;
	(void)len;
	return 0;
}

#endif /* CONFIG_LARGE_MEMCPY_GDMA */
