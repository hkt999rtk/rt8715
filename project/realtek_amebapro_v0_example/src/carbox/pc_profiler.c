#include "pc_profiler.h"
#include "gcd_sync_profiler.h"
#include "screen_queue_profiler.h"
#include "usb_hcd_profiler.h"
#include "net_queue_profiler.h"
#include "crypto_engine_profiler.h"
#include "memcpy_task_profiler.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis.h"
#include "diag.h"
#include "hal_timer.h"

#ifndef CONFIG_PC_PROFILER
#define CONFIG_PC_PROFILER 0
#endif

#if CONFIG_PC_PROFILER

/*
 * Use a prime period close to 2 ms so sampling does not remain phase locked to
 * the 1 ms USB cadence, the RTOS tick, or 30/60 Hz media work.
 */
#define PCPROF_SAMPLE_PERIOD_US       2003U
#define PCPROF_REPORT_PERIOD_MS      10000U
#define PCPROF_RECORDS_PER_BUFFER     6144U
#define PCPROF_PC_BUCKET_BYTES          64U
#define PCPROF_HASH_SIZE              4096U
#define PCPROF_TOP_COUNT                24U
#define PCPROF_TASK_TOP_COUNT           12U
#define PCPROF_MAX_TASK_SNAPSHOT        64U
#define PCPROF_TASK_STACK_BYTES       8192U
#define PCPROF_LATE_REJECT_US            10U

typedef struct pcprof_record_s {
	uint32_t pc;
	uint32_t raw_pc;
	uint32_t lr;
	TaskHandle_t task;
} pcprof_record_t;

typedef struct pcprof_hash_entry_s {
	uint32_t pc;
	uint32_t raw_pc;
	uint32_t lr;
	TaskHandle_t task;
	uint32_t hits;
} pcprof_hash_entry_t;

typedef struct pcprof_top_entry_s {
	uint32_t pc;
	uint32_t raw_pc;
	uint32_t lr;
	TaskHandle_t task;
	uint32_t hits;
} pcprof_top_entry_t;

typedef struct pcprof_task_entry_s {
	TaskHandle_t task;
	uint32_t hits;
} pcprof_task_entry_t;

static hal_timer_adapter_t pcprof_timer;
/* Large diagnostic buffers belong in LPDDR, not the limited internal SRAM. */
static pcprof_record_t pcprof_records[2][PCPROF_RECORDS_PER_BUFFER]
	__attribute__((section(".lpddr.bss.pcprof_records")));
static pcprof_hash_entry_t pcprof_hash[PCPROF_HASH_SIZE]
	__attribute__((section(".lpddr.bss.pcprof_hash")));
static TaskStatus_t pcprof_tasks[PCPROF_MAX_TASK_SNAPSHOT]
	__attribute__((section(".lpddr.bss.pcprof_tasks")));

static volatile uint32_t pcprof_active_buffer;
static volatile uint32_t pcprof_record_count[2];
static volatile uint32_t pcprof_invalid_samples;
static volatile uint32_t pcprof_nested_samples;
static volatile uint32_t pcprof_late_samples;
static volatile uint32_t pcprof_dropped_samples;
static volatile uint32_t pcprof_isr_cycles;
static volatile uint32_t pcprof_isr_cycles_max;
static volatile uint32_t pcprof_interval_cycles_sum;
static volatile uint32_t pcprof_interval_cycles_max;
static volatile uint32_t pcprof_interval_count;
static volatile uint32_t pcprof_late_10us_count;
static volatile uint32_t pcprof_late_100us_count;
static volatile uint32_t pcprof_previous_cycle;
static volatile uint32_t pcprof_caller_attributed_samples;
static uint32_t pcprof_period_cycles;
static uint32_t pcprof_late_reject_cycles;
static uint32_t pcprof_late_10us_cycles;
static uint32_t pcprof_late_100us_cycles;

extern void * volatile pxCurrentTCB;
extern void vPortExitCritical(void);

static inline __attribute__((always_inline)) int pcprof_valid_pc(uint32_t pc)
{
	pc &= ~1U;

	/* ROM/ITCM, secure ROM, SRAM, LPDDR text, and NOR XIP text. */
	return (pc < 0x00100000U) ||
	       (pc >= 0x10000000U && pc < 0x10200000U) ||
	       (pc >= 0x20000000U && pc < 0x20200000U) ||
	       (pc >= 0x70000000U && pc < 0x71000000U) ||
	       (pc >= 0x98000000U && pc < 0x99000000U);
}

static inline __attribute__((always_inline)) int pcprof_valid_xpsr(uint32_t xpsr)
{
	/* A task interrupted from Thread mode has T=1 and exception number zero. */
	return ((xpsr & (1UL << 24)) != 0U) && ((xpsr & 0x1FFU) == 0U);
}

static inline __attribute__((always_inline)) int
pcprof_interrupted_frame(uint32_t *pc_out, uint32_t *lr_out)
{
	const uint32_t *frame = (const uint32_t *)(uintptr_t)__get_PSP();
	uint32_t pc;
	uint32_t lr;

	if (frame == NULL) {
		return 0;
	}

	/* Basic exception frame: r0-r3, r12, lr, pc, xPSR. */
	pc = frame[6] & ~1U;
	if (pcprof_valid_xpsr(frame[7]) && pcprof_valid_pc(pc)) {
		lr = frame[5] & ~1U;
		*pc_out = pc;
		*lr_out = pcprof_valid_pc(lr) ? lr : 0U;
		return 1;
	}

	/*
	 * With an active floating-point context, the hardware places 18 words
	 * (s0-s15, FPSCR, reserved) before the same basic frame.  Checking both
	 * xPSR and the executable address ranges avoids treating FP data as a PC.
	 */
	pc = frame[24] & ~1U;
	if (pcprof_valid_xpsr(frame[25]) && pcprof_valid_pc(pc)) {
		lr = frame[23] & ~1U;
		*pc_out = pc;
		*lr_out = pcprof_valid_pc(lr) ? lr : 0U;
		return 1;
	}

	return 0;
}

static inline __attribute__((always_inline)) int
pcprof_is_port_exit_critical(uint32_t pc)
{
	uint32_t start = (uint32_t)(uintptr_t)vPortExitCritical & ~1U;

	/* The implementation is currently 32 bytes; leave room for build variants. */
	return pc >= start && pc < (start + 64U);
}

static void __attribute__((section(".itcm.text.pcprof_timer_callback")))
pcprof_timer_callback(void *arg)
{
	uint32_t start = DWT->CYCCNT;
	uint32_t previous = pcprof_previous_cycle;
	uint32_t late_cycles = 0U;
	uint32_t active = pcprof_active_buffer;
	uint32_t index = pcprof_record_count[active];
	uint32_t pc;
	uint32_t raw_pc;
	uint32_t lr;
	uint32_t elapsed;

	(void)arg;
	pcprof_previous_cycle = start;
	if (previous != 0U) {
		uint32_t interval = start - previous;

		if (interval > pcprof_period_cycles) {
			late_cycles = interval - pcprof_period_cycles;
		}
		pcprof_interval_cycles_sum += late_cycles;
		pcprof_interval_count++;
		if (late_cycles > pcprof_interval_cycles_max) {
			pcprof_interval_cycles_max = late_cycles;
		}
		if (late_cycles >= pcprof_late_10us_cycles) {
			pcprof_late_10us_count++;
		}
		if (late_cycles >= pcprof_late_100us_cycles) {
			pcprof_late_100us_count++;
		}
	}

	/*
	 * The timer IRQ is shared by eight HAL timers and therefore cannot safely
	 * be raised above configMAX_SYSCALL_INTERRUPT_PRIORITY.  A callback that
	 * arrives late commonly samples the instruction which unmasks BASEPRI,
	 * rather than the code that consumed the CPU.  Exclude those biased samples
	 * and report them separately instead of presenting them as hot functions.
	 */
	if (previous != 0U && late_cycles >= pcprof_late_reject_cycles) {
		pcprof_late_samples++;
		goto account_isr;
	}

	/* RETTOBASE=0 means this timer preempted another handler, not a task. */
	if ((SCB->ICSR & SCB_ICSR_RETTOBASE_Msk) == 0U) {
		pcprof_nested_samples++;
		goto account_isr;
	}

	if (!pcprof_interrupted_frame(&raw_pc, &lr) || pxCurrentTCB == NULL) {
		pcprof_invalid_samples++;
	} else if (index < PCPROF_RECORDS_PER_BUFFER) {
		pc = raw_pc;
		/*
		 * A pending low-priority timer IRQ commonly stacks the final instruction
		 * of vPortExitCritical after BASEPRI is cleared.  Its stacked LR is the
		 * useful caller site, so attribute this known artefact to that caller.
		 * Keep raw_pc as evidence and retain LR for all other ROM-only symbols.
		 */
		if (pcprof_is_port_exit_critical(raw_pc) && lr != 0U) {
			pc = lr;
			pcprof_caller_attributed_samples++;
		}
		/* Keep a real instruction address for accurate host-side addr2line. */
		pcprof_records[active][index].pc = pc;
		pcprof_records[active][index].raw_pc = raw_pc;
		pcprof_records[active][index].lr = lr;
		pcprof_records[active][index].task = (TaskHandle_t)pxCurrentTCB;
		pcprof_record_count[active] = index + 1U;
	} else {
		pcprof_dropped_samples++;
	}

account_isr:
	elapsed = DWT->CYCCNT - start;
	pcprof_isr_cycles += elapsed;
	if (elapsed > pcprof_isr_cycles_max) {
		pcprof_isr_cycles_max = elapsed;
	}
}

static void pcprof_task_hit_add(pcprof_task_entry_t entries[PCPROF_MAX_TASK_SNAPSHOT],
				TaskHandle_t task, uint32_t *entry_count,
				uint32_t *overflow_hits)
{
	uint32_t i;

	for (i = 0U; i < *entry_count; ++i) {
		if (entries[i].task == task) {
			entries[i].hits++;
			return;
		}
	}
	if (*entry_count < PCPROF_MAX_TASK_SNAPSHOT) {
		entries[*entry_count].task = task;
		entries[*entry_count].hits = 1U;
		(*entry_count)++;
	} else {
		(*overflow_hits)++;
	}
}

static void pcprof_task_top_insert(pcprof_task_entry_t top[PCPROF_TASK_TOP_COUNT],
				   const pcprof_task_entry_t *entry)
{
	uint32_t position;

	if (entry->hits <= top[PCPROF_TASK_TOP_COUNT - 1U].hits) {
		return;
	}
	for (position = PCPROF_TASK_TOP_COUNT - 1U; position > 0U; --position) {
		if (entry->hits <= top[position - 1U].hits) {
			break;
		}
		top[position] = top[position - 1U];
	}
	top[position] = *entry;
}

static uint32_t pcprof_hash_key(TaskHandle_t task, uint32_t pc, uint32_t lr)
{
	uintptr_t value = (uintptr_t)task;

	value ^= value >> 7;
	value ^= (uintptr_t)(pc >> 6);
	value ^= (uintptr_t)(lr >> 6);
	value *= 2654435761UL;
	return (uint32_t)value & (PCPROF_HASH_SIZE - 1U);
}

static void pcprof_hash_add(TaskHandle_t task, uint32_t pc, uint32_t raw_pc,
			    uint32_t lr)
{
	uint32_t bucket = pc & ~(PCPROF_PC_BUCKET_BYTES - 1U);
	uint32_t lr_bucket = lr & ~(PCPROF_PC_BUCKET_BYTES - 1U);
	uint32_t slot = pcprof_hash_key(task, bucket, lr_bucket);
	uint32_t probes;

	for (probes = 0; probes < PCPROF_HASH_SIZE; ++probes) {
		pcprof_hash_entry_t *entry = &pcprof_hash[slot];

		if (entry->hits == 0U) {
			entry->task = task;
			entry->pc = pc;
			entry->raw_pc = raw_pc;
			entry->lr = lr;
			entry->hits = 1U;
			return;
		}
		if (entry->task == task &&
		    (entry->pc & ~(PCPROF_PC_BUCKET_BYTES - 1U)) == bucket &&
		    (entry->lr & ~(PCPROF_PC_BUCKET_BYTES - 1U)) == lr_bucket) {
			entry->hits++;
			return;
		}
		slot = (slot + 1U) & (PCPROF_HASH_SIZE - 1U);
	}
}

static void pcprof_top_insert(pcprof_top_entry_t top[PCPROF_TOP_COUNT],
			      const pcprof_hash_entry_t *entry)
{
	uint32_t position;

	if (entry->hits <= top[PCPROF_TOP_COUNT - 1U].hits) {
		return;
	}
	for (position = PCPROF_TOP_COUNT - 1U; position > 0U; --position) {
		if (entry->hits <= top[position - 1U].hits) {
			break;
		}
		top[position] = top[position - 1U];
	}
	top[position].pc = entry->pc;
	top[position].raw_pc = entry->raw_pc;
	top[position].lr = entry->lr;
	top[position].task = entry->task;
	top[position].hits = entry->hits;
}

static const char *pcprof_task_name(TaskHandle_t handle, UBaseType_t task_count)
{
	UBaseType_t i;

	for (i = 0; i < task_count; ++i) {
		if (pcprof_tasks[i].xHandle == handle) {
			return pcprof_tasks[i].pcTaskName;
		}
	}
	return "<deleted>";
}

static TaskHandle_t pcprof_task_handle(const char *name, UBaseType_t task_count)
{
	UBaseType_t i;

	for (i = 0; i < task_count; ++i) {
		if (strcmp(pcprof_tasks[i].pcTaskName, name) == 0)
			return pcprof_tasks[i].xHandle;
	}
	return NULL;
}

static UBaseType_t pcprof_task_priority(TaskHandle_t handle,
				       UBaseType_t task_count)
{
	UBaseType_t i;

	for (i = 0; i < task_count; ++i) {
		if (pcprof_tasks[i].xHandle == handle) {
			return pcprof_tasks[i].uxCurrentPriority;
		}
	}
	return 0U;
}

static void pcprof_report(uint32_t sequence, uint32_t buffer, uint32_t count,
			  uint32_t invalid, uint32_t nested, uint32_t late,
			  uint32_t dropped, uint32_t isr_cycles,
			  uint32_t isr_cycles_max, uint32_t interval_cycles_sum,
			  uint32_t interval_cycles_max, uint32_t interval_count,
			  uint32_t late_10us, uint32_t late_100us,
			  uint32_t caller_attributed)
{
	pcprof_top_entry_t top[PCPROF_TOP_COUNT];
	pcprof_task_entry_t task_entries[PCPROF_MAX_TASK_SNAPSHOT];
	pcprof_task_entry_t task_top[PCPROF_TASK_TOP_COUNT];
	UBaseType_t task_count;
	uint32_t i;
	uint32_t task_entry_count = 0U;
	uint32_t task_overflow_hits = 0U;
	uint32_t irq_count = count + invalid + nested + late + dropped;
	uint32_t avg_cycles = irq_count != 0U ? isr_cycles / irq_count : 0U;
	uint32_t accepted_10000 = irq_count != 0U ?
		(uint32_t)(((uint64_t)count * 10000U) / irq_count) : 0U;
	uint32_t overhead_10000;
	uint32_t printed_hits = 0U;
	uint32_t task_printed_hits = 0U;
	uint32_t late_avg_ns = interval_count != 0U && SystemCoreClock != 0U ?
		(uint32_t)(((uint64_t)interval_cycles_sum * 1000000000ULL) /
			   ((uint64_t)interval_count * SystemCoreClock)) : 0U;
	uint32_t late_max_ns = SystemCoreClock != 0U ?
		(uint32_t)(((uint64_t)interval_cycles_max * 1000000000ULL) /
			   SystemCoreClock) : 0U;
	uint64_t window_cycles =
		(uint64_t)SystemCoreClock * PCPROF_REPORT_PERIOD_MS / 1000U;

	memset(pcprof_hash, 0, sizeof(pcprof_hash));
	memset(top, 0, sizeof(top));
	memset(task_entries, 0, sizeof(task_entries));
	memset(task_top, 0, sizeof(task_top));
	for (i = 0; i < count; ++i) {
		pcprof_hash_add(pcprof_records[buffer][i].task,
				pcprof_records[buffer][i].pc,
				pcprof_records[buffer][i].raw_pc,
				pcprof_records[buffer][i].lr);
		pcprof_task_hit_add(task_entries, pcprof_records[buffer][i].task,
				    &task_entry_count, &task_overflow_hits);
	}
	for (i = 0; i < PCPROF_HASH_SIZE; ++i) {
		if (pcprof_hash[i].hits != 0U) {
			pcprof_top_insert(top, &pcprof_hash[i]);
		}
	}
	for (i = 0U; i < task_entry_count; ++i) {
		pcprof_task_top_insert(task_top, &task_entries[i]);
	}

	task_count = uxTaskGetSystemState(pcprof_tasks,
					  PCPROF_MAX_TASK_SNAPSHOT, NULL);
	carbox_memcpy_task_profiler_set_targets(
		pcprof_task_handle("ScreenThread", task_count),
		pcprof_task_handle("AirPlayScreenReceiver", task_count));
	overhead_10000 = window_cycles != 0U ?
		(uint32_t)(((uint64_t)isr_cycles * 10000U) / window_cycles) : 0U;
	rt_printf("[PCPROF][%lu] window_ms=%u attempts=%lu samples=%lu "
		  "accepted=%lu.%02lu%% rejected="
		  "frame:%lu/nested:%lu/late:%lu dropped=%lu "
		  "isr_cycles_avg/max=%lu/%lu overhead=%lu.%02lu%%\r\n",
		  (unsigned long)sequence, PCPROF_REPORT_PERIOD_MS,
		  (unsigned long)irq_count, (unsigned long)count,
		  (unsigned long)(accepted_10000 / 100U),
		  (unsigned long)(accepted_10000 % 100U),
		  (unsigned long)invalid,
		  (unsigned long)nested, (unsigned long)late,
		  (unsigned long)dropped, (unsigned long)avg_cycles,
		  (unsigned long)isr_cycles_max,
		  (unsigned long)(overhead_10000 / 100U),
		  (unsigned long)(overhead_10000 % 100U));
	rt_printf("[PCPROF][%lu] irq_late_ns avg/max=%lu/%lu "
		  "ge10us=%lu ge100us=%lu reject_us=%u caller_attributed=%lu\r\n",
		  (unsigned long)sequence, (unsigned long)late_avg_ns,
		  (unsigned long)late_max_ns, (unsigned long)late_10us,
		  (unsigned long)late_100us, PCPROF_LATE_REJECT_US,
		  (unsigned long)caller_attributed);

	for (i = 0U; i < PCPROF_TASK_TOP_COUNT && task_top[i].hits != 0U; ++i) {
		uint32_t pct_100 = count != 0U ?
			(uint32_t)(((uint64_t)task_top[i].hits * 10000U) / count) : 0U;

		task_printed_hits += task_top[i].hits;
		rt_printf("[PCPROF][%lu][TASK] #%02lu task=%-24s p=%lu hits=%lu "
			  "pct=%lu.%02lu%%\r\n",
			  (unsigned long)sequence, (unsigned long)(i + 1U),
			  pcprof_task_name(task_top[i].task, task_count),
			  (unsigned long)pcprof_task_priority(task_top[i].task,
							 task_count),
			  (unsigned long)task_top[i].hits,
			  (unsigned long)(pct_100 / 100U),
			  (unsigned long)(pct_100 % 100U));
	}
	if (count > task_printed_hits) {
		uint32_t others = count - task_printed_hits;
		uint32_t pct_100 = (uint32_t)(((uint64_t)others * 10000U) / count);

		rt_printf("[PCPROF][%lu][TASK] others hits=%lu pct=%lu.%02lu%% "
			  "overflow_hits=%lu\r\n", (unsigned long)sequence,
			  (unsigned long)others, (unsigned long)(pct_100 / 100U),
			  (unsigned long)(pct_100 % 100U),
			  (unsigned long)task_overflow_hits);
	}

	for (i = 0; i < PCPROF_TOP_COUNT && top[i].hits != 0U; ++i) {
		uint32_t pct_100 = count != 0U ?
			(uint32_t)(((uint64_t)top[i].hits * 10000U) / count) : 0U;

		printed_hits += top[i].hits;
		rt_printf("[PCPROF][%lu][PC] #%02lu task=%-24s p=%lu pc=0x%08lx "
			  "lr=0x%08lx raw=0x%08lx hits=%lu pct=%lu.%02lu%%\r\n",
			  (unsigned long)sequence, (unsigned long)(i + 1U),
			  pcprof_task_name(top[i].task, task_count),
			  (unsigned long)pcprof_task_priority(top[i].task,
							 task_count),
			  (unsigned long)top[i].pc, (unsigned long)top[i].lr,
			  (unsigned long)top[i].raw_pc, (unsigned long)top[i].hits,
			  (unsigned long)(pct_100 / 100U),
			  (unsigned long)(pct_100 % 100U));
	}
	if (count > printed_hits) {
		uint32_t others = count - printed_hits;
		uint32_t pct_100 = (uint32_t)(((uint64_t)others * 10000U) / count);

		rt_printf("[PCPROF][%lu][PC] others hits=%lu pct=%lu.%02lu%%\r\n",
			  (unsigned long)sequence, (unsigned long)others,
			  (unsigned long)(pct_100 / 100U),
			  (unsigned long)(pct_100 % 100U));
	}
}

static void pcprof_task(void *arg)
{
	timer_id_t timer_id;
	TickType_t last_wake;
	uint32_t sequence = 0U;

	(void)arg;
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	pcprof_period_cycles =
		(uint32_t)(((uint64_t)SystemCoreClock * PCPROF_SAMPLE_PERIOD_US) /
			   1000000U);
	pcprof_late_reject_cycles =
		(uint32_t)(((uint64_t)SystemCoreClock * PCPROF_LATE_REJECT_US) /
			   1000000U);
	pcprof_late_10us_cycles = SystemCoreClock / 100000U;
	pcprof_late_100us_cycles = SystemCoreClock / 10000U;

	timer_id = hal_timer_allocate(NULL);
	if (timer_id >= MaxGTimerNum || hal_timer_init(&pcprof_timer, timer_id) != HAL_OK) {
		rt_printf("[PCPROF] ERROR: no GTimer available\r\n");
		vTaskDelete(NULL);
		return;
	}

	hal_timer_start_periodical(&pcprof_timer, PCPROF_SAMPLE_PERIOD_US,
				   pcprof_timer_callback, NULL);
	rt_printf("[PCPROF] enabled timer=%u sample_us=%u window_ms=%u "
		  "bucket=%uB first_report_ms=%u\r\n",
		  timer_id, PCPROF_SAMPLE_PERIOD_US, PCPROF_REPORT_PERIOD_MS,
		  PCPROF_PC_BUCKET_BYTES, PCPROF_REPORT_PERIOD_MS);
	last_wake = xTaskGetTickCount();

	for (;;) {
		uint32_t old_buffer;
		uint32_t count;
		uint32_t invalid;
		uint32_t nested;
		uint32_t late;
		uint32_t dropped;
		uint32_t isr_cycles;
		uint32_t isr_cycles_max;
		uint32_t interval_cycles_sum;
		uint32_t interval_cycles_max;
		uint32_t interval_count;
		uint32_t late_10us;
		uint32_t late_100us;
		uint32_t caller_attributed;
		uint32_t primask;

		vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PCPROF_REPORT_PERIOD_MS));
		/*
		 * The HAL owns the timer IRQ priority.  Use PRIMASK for this very short
		 * pointer/count snapshot instead of assuming BASEPRI masks that IRQ.
		 */
		primask = __get_PRIMASK();
		__disable_irq();
		old_buffer = pcprof_active_buffer;
		pcprof_active_buffer ^= 1U;
		pcprof_record_count[pcprof_active_buffer] = 0U;
		count = pcprof_record_count[old_buffer];
		invalid = pcprof_invalid_samples;
		nested = pcprof_nested_samples;
		late = pcprof_late_samples;
		dropped = pcprof_dropped_samples;
		isr_cycles = pcprof_isr_cycles;
		isr_cycles_max = pcprof_isr_cycles_max;
		interval_cycles_sum = pcprof_interval_cycles_sum;
		interval_cycles_max = pcprof_interval_cycles_max;
		interval_count = pcprof_interval_count;
		late_10us = pcprof_late_10us_count;
		late_100us = pcprof_late_100us_count;
		caller_attributed = pcprof_caller_attributed_samples;
		pcprof_invalid_samples = 0U;
		pcprof_nested_samples = 0U;
		pcprof_late_samples = 0U;
		pcprof_dropped_samples = 0U;
		pcprof_isr_cycles = 0U;
		pcprof_isr_cycles_max = 0U;
		pcprof_interval_cycles_sum = 0U;
		pcprof_interval_cycles_max = 0U;
		pcprof_interval_count = 0U;
		pcprof_late_10us_count = 0U;
		pcprof_late_100us_count = 0U;
		pcprof_caller_attributed_samples = 0U;
		if (primask == 0U) {
			__enable_irq();
		}

		sequence++;
		pcprof_report(sequence, old_buffer, count, invalid, nested, late,
			      dropped, isr_cycles, isr_cycles_max,
			      interval_cycles_sum, interval_cycles_max,
			      interval_count, late_10us, late_100us,
			      caller_attributed);
		gcd_sync_profiler_report(sequence);
		screen_queue_profiler_report(sequence);
		usb_hcd_profiler_report(sequence);
		net_queue_profiler_report(sequence);
		crypto_engine_profiler_report(sequence);
		carbox_memcpy_task_profiler_report(sequence,
						 PCPROF_REPORT_PERIOD_MS);
	}
}

void carbox_pc_profiler_start(void)
{
	if (xTaskCreate(pcprof_task, "pcprof",
			PCPROF_TASK_STACK_BYTES / sizeof(StackType_t), NULL,
			tskIDLE_PRIORITY + 1U, NULL) != pdPASS) {
		rt_printf("[PCPROF] ERROR: reporter task creation failed\r\n");
	}
}

#else

void carbox_pc_profiler_start(void)
{
}

#endif /* CONFIG_PC_PROFILER */
