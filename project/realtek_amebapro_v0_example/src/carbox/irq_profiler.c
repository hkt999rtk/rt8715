#include "irq_profiler.h"

#include <stdint.h>
#include <string.h>

#include "cmsis.h"
#include "diag.h"
#include "hal_irq.h"

#ifndef CONFIG_IRQ_PROFILE
#define CONFIG_IRQ_PROFILE 0
#endif

#if CONFIG_IRQ_PROFILE

#define IRQPROF_IRQ_COUNT 32U
#define IRQPROF_TOP_COUNT 8U

/*
 * IRQ names follow rtl8195bhp.h.  Keep the IRQ number in the report as the
 * authoritative identifier in case a future chip revision renames a block.
 */
static const char *const irqprof_names[IRQPROF_IRQ_COUNT] = {
	"SystemOn", "TimerGroup0", "TimerGroup1", "GPIO",
	"PWM", "ADC", "SGPIO", "UART",
	"I2C", "SSI", "I2S", "I3C",
	"USB", "SDIOH", "SDIOD", "ETHERNET",
	"WLAN", "GDMA0", "GDMA1", "Crypto",
	"SPIC", "ICC", "ISP", "H264",
	"VOE", "TFT", "MJPG", "SGDMA0",
	"SGDMA1", "SCrypto", "SLowPri", "LowPri",
};

/*
 * Two banks let the reporting task rotate counters without putting a lock in
 * any ISR.  The handler table and counters stay in internal RAM; the common
 * trampoline itself is placed in ITCM.
 */
static volatile uint32_t irqprof_handlers[IRQPROF_IRQ_COUNT]
	__attribute__((used));
static volatile uint32_t irqprof_counts[2][IRQPROF_IRQ_COUNT]
	__attribute__((used));
static volatile uint32_t irqprof_active_bank __attribute__((used));
static uint32_t irqprof_snapshot[IRQPROF_IRQ_COUNT];
static hal_irq_api_t irqprof_original_api;
static hal_irq_api_t irqprof_hook_api;
static uint32_t irqprof_initialized;
static uint32_t irqprof_hook_ok;
static uint32_t irqprof_initial_vectors;
static uint32_t irqprof_repairs;
static uint32_t irqprof_snapshot_repairs;
static uint32_t irqprof_snapshot_profiler_irq = UINT32_MAX;
static uint32_t irqprof_snapshot_profiler_callbacks;
static uint32_t irqprof_snapshot_profiler_accounted;

/*
 * All peripheral vectors point here.  IPSR identifies the current vector, so
 * one trampoline covers all 32 IRQs.  It deliberately uses only r0-r3, which
 * Cortex-M hardware already saved on exception entry, and never touches SP.
 * The final BX is a tail branch: the vendor handler receives the original
 * EXC_RETURN in LR exactly as if it were installed directly in the vector
 * table.  There is no C call frame, lock, semaphore, or profiling print in the
 * interrupt path.
 */
static void __attribute__((naked, used, section(".itcm.text.irqprof_dispatch")))
irqprof_dispatch(void)
{
	__asm volatile(
		"mrs r0, ipsr\n"
		"subs r0, r0, #16\n"
		"cmp r0, #31\n"
		"bhi 2f\n"
		"ldr r1, =irqprof_active_bank\n"
		"ldr r2, [r1]\n"
		"ldr r1, =irqprof_counts\n"
		"add.w r1, r1, r2, lsl #7\n"
		"ldr.w r2, [r1, r0, lsl #2]\n"
		"adds r2, r2, #1\n"
		"str.w r2, [r1, r0, lsl #2]\n"
		"ldr r1, =irqprof_handlers\n"
		"ldr.w r1, [r1, r0, lsl #2]\n"
		"cbz r1, 2f\n"
		"bx r1\n"
		"2: bx lr\n");
}

static void irqprof_set_vector(int32_t irqn, uint32_t vector)
{
	uint32_t dispatch = (uint32_t)(uintptr_t)irqprof_dispatch;

	if (irqn < 0 || irqn >= (int32_t)IRQPROF_IRQ_COUNT ||
	    irqprof_original_api.irq_set_vector == NULL) {
		if (irqprof_original_api.irq_set_vector != NULL) {
			irqprof_original_api.irq_set_vector(irqn, vector);
		}
		return;
	}

	/* An internal refresh may reinstall the trampoline; do not replace the
	 * saved vendor handler with the trampoline itself. */
	if ((vector & ~1U) != (dispatch & ~1U)) {
		irqprof_handlers[(uint32_t)irqn] = vector;
		__DMB();
	}
	irqprof_original_api.irq_set_vector(
		irqn, vector != 0U ? dispatch : 0U);
}

/* Must be called with peripheral interrupts masked. */
static uint32_t irqprof_audit_vectors(void)
{
	int_vector_t *vectors = hal_int_vector_stubs.ram_vector_table;
	uint32_t dispatch = (uint32_t)(uintptr_t)irqprof_dispatch;
	uint32_t repaired = 0U;
	uint32_t i;

	if (!irqprof_hook_ok || vectors == NULL ||
	    irqprof_original_api.irq_set_vector == NULL) {
		return 0U;
	}

	for (i = 0U; i < IRQPROF_IRQ_COUNT; ++i) {
		uint32_t vector = (uint32_t)(uintptr_t)vectors[16U + i];

		if (vector == 0U) {
			irqprof_handlers[i] = 0U;
			continue;
		}
		if ((vector & ~1U) == (dispatch & ~1U)) {
			continue;
		}

		/* Catch code which bypassed the HAL API table and wrote VTOR directly. */
		irqprof_handlers[i] = vector;
		__DMB();
		irqprof_original_api.irq_set_vector((int32_t)i, dispatch);
		repaired++;
	}
	return repaired;
}

void carbox_irq_profiler_init(void)
{
	hal_irq_api_t *current;
	uint32_t primask;
	uint32_t i;

	if (irqprof_initialized) {
		return;
	}
	irqprof_initialized = 1U;
	current = hal_int_vector_stubs.pirq_api_tbl;
	if (current == NULL || current->irq_set_vector == NULL ||
	    current->irq_get_vector == NULL ||
	    hal_int_vector_stubs.hal_irq_api_init == NULL) {
		rt_printf("[IRQPROF] ERROR: HAL IRQ API table unavailable\r\n");
		return;
	}

	irqprof_original_api = *current;
	irqprof_hook_api = *current;
	irqprof_hook_api.irq_set_vector = irqprof_set_vector;

	primask = __get_PRIMASK();
	__disable_irq();
	/* Preserve handlers registered before main(), then install the supported
	 * HAL API hook so every later registration is wrapped automatically. */
	for (i = 0U; i < IRQPROF_IRQ_COUNT; ++i) {
		irqprof_handlers[i] = irqprof_original_api.irq_get_vector((int32_t)i);
	}
	hal_int_vector_stubs.hal_irq_api_init(&irqprof_hook_api);
	current = hal_int_vector_stubs.pirq_api_tbl;
	irqprof_hook_ok = current != NULL &&
		current->irq_set_vector == irqprof_set_vector;
	if (irqprof_hook_ok) {
		irqprof_initial_vectors = irqprof_audit_vectors();
	}
	if (primask == 0U) {
		__enable_irq();
	}

	if (!irqprof_hook_ok) {
		rt_printf("[IRQPROF] ERROR: HAL IRQ API hook rejected; disabled\r\n");
		return;
	}
	rt_printf("[IRQPROF] enabled scope=external irq=0..31 hook=HAL-api-table "
		  "initial_vectors=%lu hotpath=naked-counter-tailbranch\r\n",
		  (unsigned long)irqprof_initial_vectors);
}

void carbox_irq_profiler_snapshot(uint32_t profiler_irq,
				  uint32_t profiler_callbacks)
{
	uint32_t primask;
	uint32_t old_bank;
	uint32_t new_bank;
	uint32_t i;

	if (!irqprof_hook_ok) {
		return;
	}

	/* This is normally called from inside the PC profiler's existing PRIMASK
	 * snapshot.  Retaining the local save/restore makes the API safe if it is
	 * reused independently without ever enabling interrupts unexpectedly. */
	primask = __get_PRIMASK();
	__disable_irq();
	irqprof_snapshot_repairs = irqprof_audit_vectors();
	irqprof_repairs += irqprof_snapshot_repairs;
	old_bank = irqprof_active_bank;
	new_bank = old_bank ^ 1U;
	memset((void *)irqprof_counts[new_bank], 0,
	       sizeof(irqprof_counts[new_bank]));
	irqprof_active_bank = new_bank;
	__DMB();
	for (i = 0U; i < IRQPROF_IRQ_COUNT; ++i) {
		irqprof_snapshot[i] = irqprof_counts[old_bank][i];
	}
	irqprof_snapshot_profiler_irq = profiler_irq;
	irqprof_snapshot_profiler_callbacks = profiler_callbacks;
	irqprof_snapshot_profiler_accounted = 0U;
	if (profiler_irq < IRQPROF_IRQ_COUNT) {
		uint32_t group_irqs = irqprof_snapshot[profiler_irq];

		irqprof_snapshot_profiler_accounted =
			profiler_callbacks < group_irqs ? profiler_callbacks : group_irqs;
	}
	if (primask == 0U) {
		__enable_irq();
	}
}

static uint32_t irqprof_adjusted_count(uint32_t irq)
{
	uint32_t count = irqprof_snapshot[irq];

	if (irq == irqprof_snapshot_profiler_irq) {
		count -= irqprof_snapshot_profiler_accounted;
	}
	return count;
}

void carbox_irq_profiler_report(uint32_t sequence, uint32_t window_ms)
{
	uint8_t selected[IRQPROF_IRQ_COUNT] = { 0U };
	uint32_t top[IRQPROF_TOP_COUNT];
	uint32_t raw_total = 0U;
	uint32_t adjusted_total = 0U;
	uint32_t raw_active = 0U;
	uint32_t adjusted_active = 0U;
	uint32_t printed = 0U;
	uint32_t top_count = 0U;
	uint32_t i;

	if (!irqprof_hook_ok || window_ms == 0U) {
		return;
	}

	for (i = 0U; i < IRQPROF_IRQ_COUNT; ++i) {
		uint32_t adjusted = irqprof_adjusted_count(i);

		raw_total += irqprof_snapshot[i];
		adjusted_total += adjusted;
		if (irqprof_snapshot[i] != 0U) {
			raw_active++;
		}
		if (adjusted != 0U) {
			adjusted_active++;
		}
	}
	for (top_count = 0U; top_count < IRQPROF_TOP_COUNT; ++top_count) {
		uint32_t best = IRQPROF_IRQ_COUNT;

		for (i = 0U; i < IRQPROF_IRQ_COUNT; ++i) {
			if (!selected[i] && irqprof_adjusted_count(i) != 0U &&
			    (best == IRQPROF_IRQ_COUNT ||
			     irqprof_adjusted_count(i) >
				irqprof_adjusted_count(best))) {
				best = i;
			}
		}
		if (best == IRQPROF_IRQ_COUNT) {
			break;
		}
		selected[best] = 1U;
		top[top_count] = best;
	}

	rt_printf("[IRQPROF][%lu] window_ms=%lu scope=external "
		  "raw/adjusted=%lu/%lu adjusted_rate=%lu/s "
		  "active raw/adjusted=%lu/%lu top=%lu repairs_now/total=%lu/%lu "
		  "initial_vectors=%lu adjustment=pc-timer-only "
		  "uart_includes_profiler_log=1 excludes=SysTick/PendSV/SVC\r\n",
		  (unsigned long)sequence, (unsigned long)window_ms,
		  (unsigned long)raw_total, (unsigned long)adjusted_total,
		  (unsigned long)(((uint64_t)adjusted_total * 1000U) / window_ms),
		  (unsigned long)raw_active, (unsigned long)adjusted_active,
		  (unsigned long)top_count,
		  (unsigned long)irqprof_snapshot_repairs,
		  (unsigned long)irqprof_repairs,
		  (unsigned long)irqprof_initial_vectors);
	if (irqprof_snapshot_profiler_irq < IRQPROF_IRQ_COUNT) {
		uint32_t irq = irqprof_snapshot_profiler_irq;
		uint32_t raw = irqprof_snapshot[irq];
		uint32_t unmatched = irqprof_snapshot_profiler_callbacks -
			irqprof_snapshot_profiler_accounted;

		rt_printf("[IRQPROF][%lu][SELF] irq=%lu name=%s group_raw=%lu "
			  "callbacks/deducted/unmatched=%lu/%lu/%lu other_est=%lu\r\n",
			  (unsigned long)sequence, (unsigned long)irq,
			  irqprof_names[irq], (unsigned long)raw,
			  (unsigned long)irqprof_snapshot_profiler_callbacks,
			  (unsigned long)irqprof_snapshot_profiler_accounted,
			  (unsigned long)unmatched,
			  (unsigned long)(raw -
				irqprof_snapshot_profiler_accounted));
	}

	for (i = 0U; i < top_count; ++i) {
		uint32_t irq = top[i];
		uint32_t raw = irqprof_snapshot[irq];
		uint32_t adjusted = irqprof_adjusted_count(irq);
		uint32_t pct_100 = adjusted_total != 0U ?
			(uint32_t)(((uint64_t)adjusted * 10000U) /
				   adjusted_total) : 0U;

		printed += adjusted;
		rt_printf("[IRQPROF][%lu][IRQ] #%02lu irq=%lu vector=%lu "
			  "name=%-11s raw/adjusted=%lu/%lu rate=%lu/s "
			  "pct_adjusted=%lu.%02lu%%\r\n",
			  (unsigned long)sequence, (unsigned long)(i + 1U),
			  (unsigned long)irq, (unsigned long)(irq + 16U),
			  irqprof_names[irq], (unsigned long)raw,
			  (unsigned long)adjusted,
			  (unsigned long)(((uint64_t)adjusted * 1000U) /
					  window_ms),
			  (unsigned long)(pct_100 / 100U),
			  (unsigned long)(pct_100 % 100U));
	}
	if (adjusted_total > printed) {
		rt_printf("[IRQPROF][%lu][IRQ] others adjusted=%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)(adjusted_total - printed));
	}
}

#else

void carbox_irq_profiler_init(void)
{
}

void carbox_irq_profiler_snapshot(uint32_t profiler_irq,
				  uint32_t profiler_callbacks)
{
	(void)profiler_irq;
	(void)profiler_callbacks;
}

void carbox_irq_profiler_report(uint32_t sequence, uint32_t window_ms)
{
	(void)sequence;
	(void)window_ms;
}

#endif /* CONFIG_IRQ_PROFILE */
