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
#ifndef CONFIG_SCREEN_FPS_PROFILE
#define CONFIG_SCREEN_FPS_PROFILE 0
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
#define LARGE_MEMCPY_GDMA_CONTEXT_COUNT 2U
#define LARGE_MEMCPY_GDMA_LINK_BLOCK_BYTES \
	(MAX_DMA_BLOCK_SIZE & \
	 ~(LARGE_MEMCPY_GDMA_LINK_BURST_BYTES - 1U))

#if CONFIG_LARGE_MEMCPY_GDMA

typedef struct large_memcpy_gdma_context_s {
	gdma_t dma;
	SemaphoreHandle_t done;
	const char *disabled_reason;
	uint32_t disabled_raw_error;
	volatile uint8_t busy;
	volatile uint8_t in_flight;
	volatile uint8_t irq_error;
	uint8_t available;
} large_memcpy_gdma_context_t;

static large_memcpy_gdma_context_t large_memcpy_gdma[LARGE_MEMCPY_GDMA_CONTEXT_COUNT];
static uint8_t large_memcpy_gdma_initialized;
static uint8_t large_memcpy_gdma_early_irq_reported;
static uint8_t large_memcpy_gdma_lli_reported;
static uint32_t large_memcpy_gdma_unavailable_fallbacks;
static uint32_t large_memcpy_gdma_busy_fallbacks;

#if CONFIG_SCREEN_FPS_PROFILE
typedef struct screen_gdma_stats_s {
	uint32_t calls;
	uint32_t success;
	uint32_t fallback;
	uint64_t requested_bytes;
	uint64_t dma_bytes;
	uint64_t time_sum_us;
	uint32_t time_max_us;
	uint32_t over_1ms;
	uint32_t over_4ms;
	uint32_t over_8ms;
	uint32_t over_16ms;
} screen_gdma_stats_t;

static screen_gdma_stats_t screen_gdma_stats[2];
static TaskHandle_t screen_gdma_tasks[2];

static int screen_gdma_task_role(TaskHandle_t task)
{
	const char *name;

	if (task == screen_gdma_tasks[0]) return 0;
	if (task == screen_gdma_tasks[1]) return 1;
	name = pcTaskGetName(task);
	if (name != NULL && strcmp(name, "AirPlayScreenReceiver") == 0) {
		screen_gdma_tasks[0] = task;
		return 0;
	}
	if (name != NULL && strcmp(name, "ScreenThread") == 0) {
		screen_gdma_tasks[1] = task;
		return 1;
	}
	return -1;
}

static void screen_gdma_record(int role, size_t len, int success,
			       uint32_t start_us)
{
	uint32_t elapsed_us;
	screen_gdma_stats_t *stats;

	if (role < 0) return;
	elapsed_us = hal_read_curtime_us() - start_us;
	taskENTER_CRITICAL();
	stats = &screen_gdma_stats[role];
	stats->calls++;
	stats->success += success != 0;
	stats->fallback += success == 0;
	stats->requested_bytes += len;
	if (success) stats->dma_bytes += len;
	stats->time_sum_us += elapsed_us;
	if (elapsed_us > stats->time_max_us) stats->time_max_us = elapsed_us;
	stats->over_1ms += elapsed_us > 1000U;
	stats->over_4ms += elapsed_us > 4000U;
	stats->over_8ms += elapsed_us > 8000U;
	stats->over_16ms += elapsed_us > 16667U;
	taskEXIT_CRITICAL();
}
#endif

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
		hal_gdma_irq_reg(adaptor,
			(irq_handler_t)hal_gdma_stubs.hal_gdma_memcpy_irq_handler,
			adaptor);
		hal_gdma_memcpy_irq_hook(adaptor,
			(gdma_callback_t)callback, adaptor);
	}
	if (adaptor->ch_num < MIN_MULTI_CHNL_NUM ||
	    adaptor->ch_num >= MAX_GDMA_CHNL || adaptor->pgdma_ch_lli == NULL) {
		context->disabled_reason = "invalid-linked-channel";
		if (adaptor->have_chnl) {
			dma_memcpy_deinit(&context->dma);
		}
		return -1;
	}
	large_memcpy_gdma_configure_irq(context);
	context->disabled_reason = NULL;
	context->disabled_raw_error = 0U;
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

void carbox_large_memcpy_gdma_report(uint32_t sequence)
{
	uint32_t busy = __atomic_exchange_n(&large_memcpy_gdma_busy_fallbacks,
		0U, __ATOMIC_RELAXED);
	uint32_t unavailable = __atomic_exchange_n(
		&large_memcpy_gdma_unavailable_fallbacks, 0U, __ATOMIC_RELAXED);
	const large_memcpy_gdma_context_t *slot0 = &large_memcpy_gdma[0];
	const large_memcpy_gdma_context_t *slot1 = &large_memcpy_gdma[1];

	/* Keep the 10-second output quiet unless the shared memcpy engine itself
	 * rejected work.  Per-screen latency, when present, is printed below. */
	if ((busy != 0U) || (unavailable != 0U)) {
		rt_printf("[LARGE_MEMCPY_GDMA][%lu] fallback busy/unavailable=%lu/%lu "
			  "slot0 avail/busy=%u/%u reason=%s raw=0x%02x "
			  "slot1 avail/busy=%u/%u reason=%s raw=0x%02x\r\n",
			  (unsigned long)sequence,
			  (unsigned long)busy, (unsigned long)unavailable,
			  (unsigned int)slot0->available, (unsigned int)slot0->busy,
			  slot0->available ? "ready" :
				(slot0->disabled_reason != NULL ? slot0->disabled_reason :
				 "not-initialized"),
			  (unsigned int)slot0->disabled_raw_error,
			  (unsigned int)slot1->available, (unsigned int)slot1->busy,
			  slot1->available ? "ready" :
				(slot1->disabled_reason != NULL ? slot1->disabled_reason :
				 "not-initialized"),
			  (unsigned int)slot1->disabled_raw_error);
	}
#if CONFIG_SCREEN_FPS_PROFILE
	{
		screen_gdma_stats_t stats[2];
		uint32_t i;

		taskENTER_CRITICAL();
		stats[0] = screen_gdma_stats[0];
		stats[1] = screen_gdma_stats[1];
		screen_gdma_stats[0] = (screen_gdma_stats_t){ 0 };
		screen_gdma_stats[1] = (screen_gdma_stats_t){ 0 };
		taskEXIT_CRITICAL();
		for (i = 0U; i < 2U; ++i) {
			if (stats[i].calls == 0U) continue;
			rt_printf("[SCREENGDMA][%lu] task=%s calls/ok/fallback=%lu/%lu/%lu "
				  "bytes request/dma=%llu/%llu us avg/max=%llu/%lu "
				  "over1/4/8/16ms=%lu/%lu/%lu/%lu\r\n",
				  (unsigned long)sequence,
				  i == 0U ? "AirPlayScreenReceiver" : "ScreenThread",
				  (unsigned long)stats[i].calls,
				  (unsigned long)stats[i].success,
				  (unsigned long)stats[i].fallback,
				  (unsigned long long)stats[i].requested_bytes,
				  (unsigned long long)stats[i].dma_bytes,
				  (unsigned long long)(stats[i].calls != 0U ?
					stats[i].time_sum_us / stats[i].calls : 0U),
				  (unsigned long)stats[i].time_max_us,
				  (unsigned long)stats[i].over_1ms,
				  (unsigned long)stats[i].over_4ms,
				  (unsigned long)stats[i].over_8ms,
				  (unsigned long)stats[i].over_16ms);
		}
	}
#endif
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
	/* memcpy cannot return an error.  Quiesce the channel before the wrapper
	 * recopies the complete original range with M33, otherwise a late DMA write
	 * could corrupt the CPU fallback result.  Keep GDMA disabled after a runtime
	 * fault rather than risking an unvalidated recovery path.  The latched reason
	 * and raw status are printed by the ten-second RECVGDMA report, avoiding UART
	 * work in this failure path. */
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
#if CONFIG_SCREEN_FPS_PROFILE
	int profile_role = -1;
	uint32_t profile_start_us = 0U;
#endif

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
	owner = xTaskGetCurrentTaskHandle();
#if CONFIG_SCREEN_FPS_PROFILE
	profile_role = screen_gdma_task_role(owner);
	profile_start_us = hal_read_curtime_us();
#endif
	if ((((uintptr_t)destination ^ (uintptr_t)source) & 3U) != 0U) {
		rt_printf("[LARGE_MEMCPY_GDMA][FALLBACK] reason=alignment len=%u using=M33\r\n",
			  (unsigned int)len);
#if CONFIG_SCREEN_FPS_PROFILE
		screen_gdma_record(profile_role, len, 0, profile_start_us);
#endif
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
		if (any_available) {
			__atomic_fetch_add(&large_memcpy_gdma_busy_fallbacks, 1U,
				__ATOMIC_RELAXED);
		} else {
			__atomic_fetch_add(&large_memcpy_gdma_unavailable_fallbacks,
				1U, __ATOMIC_RELAXED);
		}
#if CONFIG_SCREEN_FPS_PROFILE
		screen_gdma_record(profile_role, len, 0, profile_start_us);
#endif
		return 0;
	}

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
		context->in_flight = 1U;
		if (hal_gdma_memcpy(&context->dma.hal_gdma_adaptor,
				    destination, (void *)source,
				    transaction_len) != HAL_OK) {
			context->in_flight = 0U;
			context->disabled_reason = "contiguous-submit";
			context->disabled_raw_error =
				context->dma.hal_gdma_adaptor.gdma_dev != NULL ?
				(context->dma.hal_gdma_adaptor.gdma_dev->raw_err &
				 (1U << context->dma.hal_gdma_adaptor.ch_num)) : 0U;
			hal_gdma_isr_dis(&context->dma.hal_gdma_adaptor);
			hal_gdma_abort(&context->dma.hal_gdma_adaptor);
			hal_gdma_clean_pending_isr(
				&context->dma.hal_gdma_adaptor);
			dma_memcpy_deinit(&context->dma);
			context->available = 0U;
			goto out;
		}
		if (large_memcpy_gdma_wait(context) != 0) {
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
#if CONFIG_SCREEN_FPS_PROFILE
	screen_gdma_record(profile_role, len, copied, profile_start_us);
#endif
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
	const carbox_gdma_copy_block_t *block, uint32_t *prefix_out,
	int allow_unaligned_source)
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
	if (prefix >= block->len ||
	    (!allow_unaligned_source && ((source + prefix) & 3U) != 0U)) {
		*prefix_out = 0U;
		return 0U;
	}
	/* copyv_submit() explicitly programs byte-width descriptors, so fragmented
	 * pbuf sources need no word alignment.  This matters for NCM TX: after an
	 * Ethernet/IP/TCP header, a contiguous NTB destination and its external TCP
	 * payload source commonly have different address mod-4 values.  Keep only
	 * the destination body cache-line isolated; its remaining prefix/tail is
	 * copied by M33 after every DMA invalidate has completed. */
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
	hal_gdma_isr_dis(adaptor);
	context->in_flight = 0U;
	hal_gdma_abort(adaptor);
	hal_gdma_clean_pending_isr(adaptor);
	dma_memcpy_deinit(&context->dma);
	context->available = 0U;
	large_memcpy_gdma_drain_completion(context);
}

static int large_memcpy_gdma_copyv_submit(
	large_memcpy_gdma_context_t *context,
	hal_gdma_block_t *blocks, uint8_t block_count)
{
	phal_gdma_adaptor_t adaptor = &context->dma.hal_gdma_adaptor;
	uint32_t expected_transfers;
	uint32_t descriptor_transfers;
	uint32_t source_width;
	uint32_t destination_width;
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

	/* The ROM helper reliably constructs SAR/DAR/LLP, but board diagnostics
	 * show that its per-LLI CTL words can retain transfer-width state from a
	 * preceding ordinary memcpy even after the adaptor fields are restored.
	 * Program the documented memory-to-memory byte-copy fields explicitly.
	 * Preserve INT_EN, TT_FC and the helper's LLP enable bits; bits 1..16 are
	 * width/increment/burst and are all zero for byte, incrementing, MsizeOne.
	 * BLOCK_TS is then exactly the byte length. */
	for (i = 0U; i < block_count; ++i) {
		adaptor->pgdma_ch_lli[i].ctlx_low &= ~0x0001FFFEU;
		adaptor->pgdma_ch_lli[i].ctlx_up =
			(adaptor->pgdma_ch_lli[i].ctlx_up & ~0xFFFU) |
			(blocks[i].block_length & 0xFFFU);
	}

	/* Validate the ROM-generated descriptors before hardware can consume them.
	 * BLOCK_TS is expressed in source transfers, not bytes.  A malformed LLI is
	 * a deterministic configuration failure and must fall back to M33 instead of
	 * producing a partially copied socket buffer. */
	for (i = 0U; i < block_count; ++i) {
		source_width = (adaptor->pgdma_ch_lli[i].ctlx_low >> 4) & 0x7U;
		destination_width =
			(adaptor->pgdma_ch_lli[i].ctlx_low >> 1) & 0x7U;
		descriptor_transfers =
			adaptor->pgdma_ch_lli[i].ctlx_up & 0xFFFU;
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
	}
	if (!large_memcpy_gdma_lli_reported) {
		gdma_ch_lli_t *first = &adaptor->pgdma_ch_lli[0];
		gdma_ch_lli_t *last = &adaptor->pgdma_ch_lli[block_count - 1U];

		large_memcpy_gdma_lli_reported = 1U;
		rt_printf("[RECVGDMA][LLI] validated blocks=%u "
			  "first len=%u ctl=%08x/%08x last len=%u ctl=%08x/%08x\r\n",
			  (unsigned int)block_count,
			  (unsigned int)blocks[0].block_length,
			  (unsigned int)first->ctlx_low,
			  (unsigned int)first->ctlx_up,
			  (unsigned int)blocks[block_count - 1U].block_length,
			  (unsigned int)last->ctlx_low,
			  (unsigned int)last->ctlx_up);
	}
	/* Hardware reads the persistent LLI array, not the temporary block list.
	 * Clean both explicitly because the ROM configuration helper's cache
	 * behaviour is not part of its public contract. */
	if (adaptor->dcache_clean_by_addr != NULL) {
		adaptor->dcache_clean_by_addr((uint32_t *)blocks,
			(int32_t)(block_count * sizeof(*blocks)));
		adaptor->dcache_clean_by_addr((uint32_t *)adaptor->pgdma_ch_lli,
			(int32_t)(block_count * sizeof(gdma_ch_lli_t)));
	}
	__DSB();

	context->in_flight = 1U;
	hal_gdma_on(adaptor);
	hal_gdma_isr_en(adaptor);
	if (hal_gdma_chnl_block_setting(adaptor) != HAL_OK) {
		context->in_flight = 0U;
		large_memcpy_gdma_disable_after_submit_error(context,
			"channel-config");
		return -1;
	}
	hal_gdma_chnl_en(adaptor);
	if (large_memcpy_gdma_wait(context) != 0) {
		return -1;
	}

	for (i = 0U; i < block_count; ++i) {
		if (adaptor->dcache_invalidate_by_addr != NULL) {
			adaptor->dcache_invalidate_by_addr(
				(uint32_t *)(uintptr_t)blocks[i].dst_addr,
				(int32_t)blocks[i].block_length);
		}
	}
	__DSB();
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

static int large_memcpy_gdma_copyv_try_internal(
	const carbox_gdma_copy_block_t *blocks, size_t block_count,
	carbox_gdma_copyv_result_t *result, int allow_unaligned_source)
{
	large_memcpy_gdma_context_t *context = NULL;
	hal_gdma_block_t dma_blocks[MAX_MULTI_BLOCK_NUM];
	TaskHandle_t owner;
	UBaseType_t original_priority;
	uint64_t total_bytes = 0U;
	uint64_t eligible_bytes = 0U;
	size_t i;
	unsigned int context_index;
	unsigned int any_available = 0U;
	uint8_t dma_count = 0U;
	uint32_t dma_bytes = 0U;
	uint32_t cpu_bytes = 0U;
	uint16_t dma_block_count = 0U;
	uint16_t dma_batch_count = 0U;
	int copied = 0;

	if (result != NULL) {
		result->dma_bytes = 0U;
		result->cpu_edge_bytes = 0U;
		result->dma_blocks = 0U;
		result->dma_batches = 0U;
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
		body = large_memcpy_copyv_body(&blocks[i], &prefix,
			allow_unaligned_source);
		eligible_bytes += body;
	}
	if (total_bytes <= LARGE_MEMCPY_GDMA_THRESHOLD ||
	    eligible_bytes <= LARGE_MEMCPY_GDMA_THRESHOLD) {
		return 0;
	}

	taskENTER_CRITICAL();
	for (context_index = 0U;
	     context_index < LARGE_MEMCPY_GDMA_CONTEXT_COUNT;
	     ++context_index) {
		if (!large_memcpy_gdma[context_index].available) {
			continue;
		}
		any_available = 1U;
		if (!large_memcpy_gdma[context_index].busy) {
			large_memcpy_gdma[context_index].busy = 1U;
			context = &large_memcpy_gdma[context_index];
			break;
		}
	}
	taskEXIT_CRITICAL();
	if (context == NULL) {
		/* Socket recv can attempt this hundreds of times per window.  The caller
		 * profiles busy fallbacks; avoid a UART line for every occurrence. */
		if (any_available) {
			__atomic_fetch_add(&large_memcpy_gdma_busy_fallbacks, 1U,
				__ATOMIC_RELAXED);
		} else {
			__atomic_fetch_add(&large_memcpy_gdma_unavailable_fallbacks,
				1U, __ATOMIC_RELAXED);
		}
		return 0;
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
		body = large_memcpy_copyv_body(&blocks[i], &prefix,
			allow_unaligned_source);
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
						dma_blocks, dma_count) != 0) {
					goto out;
				}
				dma_batch_count++;
				dma_count = 0U;
			}
		}
	}
	if (dma_count != 0U) {
		if (large_memcpy_gdma_copyv_submit(context, dma_blocks,
					    dma_count) != 0) {
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
	for (i = 0U; i < block_count; ++i) {
		uint8_t *destination = (uint8_t *)blocks[i].dst;
		uint32_t prefix;
		uint32_t body;

		if (blocks[i].len == 0U) {
			continue;
		}
		body = large_memcpy_copyv_body(&blocks[i], &prefix,
			allow_unaligned_source);
		if (body != 0U &&
		    context->dma.hal_gdma_adaptor.dcache_invalidate_by_addr != NULL) {
			context->dma.hal_gdma_adaptor.dcache_invalidate_by_addr(
				(uint32_t *)(destination + prefix), (int32_t)body);
		}
	}
	__DSB();

	/* Cache invalidate helpers are allowed to operate at cache-line
	 * granularity.  Do not write CPU-owned prefix/tail bytes before any DMA
	 * invalidate, even though the DMA body itself is cache-line isolated: some
	 * ROM implementations include the line at the exclusive end address.  Copy
	 * every CPU edge only after all DMA batches and invalidations complete. */
	for (i = 0U; i < block_count; ++i) {
		uint8_t *destination = (uint8_t *)blocks[i].dst;
		const uint8_t *source = (const uint8_t *)blocks[i].src;
		uint32_t prefix;
		uint32_t body;
		uint32_t tail;

		if (blocks[i].len == 0U) {
			continue;
		}
		body = large_memcpy_copyv_body(&blocks[i], &prefix,
			allow_unaligned_source);
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
	context->busy = 0U;
	if (uxTaskPriorityGet(owner) != original_priority) {
		vTaskPrioritySet(owner, original_priority);
	}
	return copied;
}

int carbox_linked_gdma_copyv_try(const carbox_gdma_copy_block_t *blocks,
				 size_t block_count,
				 carbox_gdma_copyv_result_t *result)
{
	return large_memcpy_gdma_copyv_try_internal(blocks, block_count, result, 0);
}

int carbox_linked_gdma_copyv_bytes_try(const carbox_gdma_copy_block_t *blocks,
				       size_t block_count,
				       carbox_gdma_copyv_result_t *result)
{
	return large_memcpy_gdma_copyv_try_internal(blocks, block_count, result, 1);
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
		result->dma_blocks = 0U;
		result->dma_batches = 0U;
	}
	return 0;
}

int carbox_linked_gdma_copyv_bytes_try(const carbox_gdma_copy_block_t *blocks,
				       size_t block_count,
				       carbox_gdma_copyv_result_t *result)
{
	return carbox_linked_gdma_copyv_try(blocks, block_count, result);
}

#endif /* CONFIG_LARGE_MEMCPY_GDMA */
