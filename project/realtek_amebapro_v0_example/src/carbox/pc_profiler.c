#include "pc_profiler.h"
#include "gcd_sync_profiler.h"
#include "screen_queue_profiler.h"
#include "usb_hcd_profiler.h"
#include "net_queue_profiler.h"
#include "crypto_engine_profiler.h"
#include "memcpy_task_profiler.h"
#include "irq_profiler.h"
#include "video_handover_zero_copy.h"
#include "screen_tx_direct_crypto.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis.h"
#include "diag.h"
#include "hal_timer.h"
#include "lwip_intf.h"
#include "lwip/sockets.h"

#ifndef CONFIG_PC_PROFILER
#define CONFIG_PC_PROFILER 0
#endif
#ifndef CONFIG_PC_PROFILER_PC_DETAIL
#define CONFIG_PC_PROFILER_PC_DETAIL 0
#endif
#ifndef CONFIG_PC_PROFILER_RTW_RECV_DETAIL
#define CONFIG_PC_PROFILER_RTW_RECV_DETAIL 0
#endif
#ifndef CONFIG_PC_PROFILER_RTW_DUMP_PROFILE
#define CONFIG_PC_PROFILER_RTW_DUMP_PROFILE 0
#endif

#define PCPROF_NEEDS_PC_HASH \
	(CONFIG_PC_PROFILER_PC_DETAIL || CONFIG_PC_PROFILER_RTW_RECV_DETAIL)

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
#define PCPROF_TARGET_TOP_COUNT         32U
#define PCPROF_TASK_TOP_COUNT           12U
#define PCPROF_MAX_TASK_SNAPSHOT        64U
#define PCPROF_RTW_DUMP_EXIT_COUNT         6U
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

#if CONFIG_PC_PROFILER_RTW_DUMP_PROFILE
typedef struct pcprof_rtw_dump_exit_s {
	uint32_t calls;
	uint64_t cycles;
	uint32_t cycles_max;
} pcprof_rtw_dump_exit_t;

typedef struct pcprof_rtw_dump_active_s {
	uint32_t start_cycles;
	uint32_t buffer;
	uint32_t active;
} pcprof_rtw_dump_active_t;

typedef struct pcprof_rtw_dump_stage_s {
	uint32_t calls;
	uint32_t bytes;
	uint64_t cycles;
	uint32_t cycles_max;
} pcprof_rtw_dump_stage_t;
#endif

static hal_timer_adapter_t pcprof_timer;
/* Large diagnostic buffers belong in LPDDR, not the limited internal SRAM. */
static pcprof_record_t pcprof_records[2][PCPROF_RECORDS_PER_BUFFER]
	__attribute__((section(".lpddr.bss.pcprof_records")));
static pcprof_hash_entry_t pcprof_hash[PCPROF_HASH_SIZE]
	__attribute__((section(".lpddr.bss.pcprof_hash")));
static TaskStatus_t pcprof_tasks[PCPROF_MAX_TASK_SNAPSHOT]
	__attribute__((section(".lpddr.bss.pcprof_tasks")));
#if CONFIG_PC_PROFILER_RTW_DUMP_PROFILE
static pcprof_rtw_dump_exit_t
	pcprof_rtw_dump_exits[2][PCPROF_RTW_DUMP_EXIT_COUNT];
static pcprof_rtw_dump_active_t pcprof_rtw_dump_active;
static pcprof_rtw_dump_stage_t pcprof_rtw_dump_cache[2];
static pcprof_rtw_dump_stage_t pcprof_rtw_dump_txdesc[2];
#endif

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
static volatile uint32_t pcprof_timer_callbacks;
static volatile uint32_t pcprof_timer_irq = UINT32_MAX;
#if CONFIG_PC_PROFILER_RTW_DUMP_PROFILE
static volatile uint32_t pcprof_rtw_dump_reentry[2];
static volatile uint32_t pcprof_rtw_dump_unmatched_exit[2];
static TaskHandle_t volatile pcprof_tcp_ip_task;
#endif
static uint32_t pcprof_period_cycles;
static uint32_t pcprof_late_reject_cycles;
static uint32_t pcprof_late_10us_cycles;
static uint32_t pcprof_late_100us_cycles;

extern void * volatile pxCurrentTCB;
extern void vPortExitCritical(void);

#if CONFIG_PC_PROFILER_RTW_DUMP_PROFILE
extern void __real_rtw_enter_critical(void *lock, void *irq_state);
extern void __real_rtw_exit_critical(void *lock, void *irq_state);
extern void __real_clean_cache_wlan(void *address, uint32_t length);
extern void __real_rtl8195b_update_txdesc(void *adapter, void *txdesc);
extern void rtw_dump_xframe(void *adapter, void *xmit_frame);

#define RTW_DUMP_ENTER_PRIMARY_OFFSET  0x0000007eU
#define RTW_DUMP_ENTER_BUDDY_OFFSET    0x000001b6U

static const uint16_t pcprof_rtw_dump_exit_offset[PCPROF_RTW_DUMP_EXIT_COUNT] = {
	0x0098U, 0x00f6U, 0x0138U, 0x01d0U, 0x0204U, 0x02c6U,
};

static uint32_t pcprof_rtw_dump_base(void)
{
	return (uint32_t)(uintptr_t)rtw_dump_xframe & ~1U;
}

static int pcprof_rtw_dump_exit_index(uint32_t caller)
{
	uint32_t i;
	uint32_t base = pcprof_rtw_dump_base();

	for (i = 0U; i < PCPROF_RTW_DUMP_EXIT_COUNT; ++i) {
		if (base + pcprof_rtw_dump_exit_offset[i] == caller) {
			return (int)i;
		}
	}
	return -1;
}

void __attribute__((section(".itcm.text.pcprof_rtw_dump_wrapper")))
__wrap_rtw_enter_critical(void *lock, void *irq_state)
{
	uint32_t caller =
		(uint32_t)(uintptr_t)__builtin_return_address(0) & ~1U;
	uint32_t base = pcprof_rtw_dump_base();
	TaskHandle_t target = pcprof_tcp_ip_task;

	__real_rtw_enter_critical(lock, irq_state);
	if ((caller != base + RTW_DUMP_ENTER_PRIMARY_OFFSET &&
	     caller != base + RTW_DUMP_ENTER_BUDDY_OFFSET) ||
	    target == NULL || (TaskHandle_t)pxCurrentTCB != target) {
		return;
	}
	if (pcprof_rtw_dump_active.active != 0U) {
		pcprof_rtw_dump_reentry[pcprof_active_buffer]++;
		return;
	}
	pcprof_rtw_dump_active.buffer = pcprof_active_buffer;
	pcprof_rtw_dump_active.start_cycles = DWT->CYCCNT;
	pcprof_rtw_dump_active.active = 1U;
}

void __attribute__((section(".itcm.text.pcprof_rtw_dump_wrapper")))
__wrap_rtw_exit_critical(void *lock, void *irq_state)
{
	uint32_t caller =
		(uint32_t)(uintptr_t)__builtin_return_address(0) & ~1U;
	int exit_index;

	if (pcprof_rtw_dump_active.active == 0U) {
		__real_rtw_exit_critical(lock, irq_state);
		return;
	}
	exit_index = pcprof_rtw_dump_exit_index(caller);
	if (exit_index >= 0 && (TaskHandle_t)pxCurrentTCB == pcprof_tcp_ip_task) {
		uint32_t elapsed =
			DWT->CYCCNT - pcprof_rtw_dump_active.start_cycles;
		uint32_t buffer = pcprof_rtw_dump_active.buffer;
		pcprof_rtw_dump_exit_t *entry =
			&pcprof_rtw_dump_exits[buffer][(uint32_t)exit_index];

		entry->calls++;
		entry->cycles += elapsed;
		if (elapsed > entry->cycles_max) {
			entry->cycles_max = elapsed;
		}
		pcprof_rtw_dump_active.active = 0U;
	} else if (exit_index >= 0) {
		pcprof_rtw_dump_unmatched_exit[pcprof_active_buffer]++;
	}
	__real_rtw_exit_critical(lock, irq_state);
}

void __attribute__((section(".itcm.text.pcprof_rtw_dump_wrapper")))
__wrap_clean_cache_wlan(void *address, uint32_t length)
{
	if (pcprof_rtw_dump_active.active != 0U &&
	    (TaskHandle_t)pxCurrentTCB == pcprof_tcp_ip_task) {
		uint32_t start = DWT->CYCCNT;
		uint32_t elapsed;
		pcprof_rtw_dump_stage_t *stage =
			&pcprof_rtw_dump_cache[pcprof_rtw_dump_active.buffer];

		__real_clean_cache_wlan(address, length);
		elapsed = DWT->CYCCNT - start;
		stage->calls++;
		stage->bytes += length;
		stage->cycles += elapsed;
		if (elapsed > stage->cycles_max) {
			stage->cycles_max = elapsed;
		}
		return;
	}
	__real_clean_cache_wlan(address, length);
}

void __attribute__((section(".itcm.text.pcprof_rtw_dump_wrapper")))
__wrap_rtl8195b_update_txdesc(void *adapter, void *txdesc)
{
	if (pcprof_rtw_dump_active.active != 0U &&
	    (TaskHandle_t)pxCurrentTCB == pcprof_tcp_ip_task) {
		uint32_t start = DWT->CYCCNT;
		uint32_t elapsed;
		pcprof_rtw_dump_stage_t *stage =
			&pcprof_rtw_dump_txdesc[pcprof_rtw_dump_active.buffer];

		__real_rtl8195b_update_txdesc(adapter, txdesc);
		elapsed = DWT->CYCCNT - start;
		stage->calls++;
		stage->cycles += elapsed;
		if (elapsed > stage->cycles_max) {
			stage->cycles_max = elapsed;
		}
		return;
	}
	__real_rtl8195b_update_txdesc(adapter, txdesc);
}
#endif

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
	pcprof_timer_callbacks++;
	if (pcprof_timer_irq >= 32U) {
		uint32_t exception = __get_IPSR();

		if (exception >= 16U && exception < 48U) {
			pcprof_timer_irq = exception - 16U;
		}
	}
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

static void pcprof_top_insert(pcprof_top_entry_t *top, uint32_t top_count,
			      const pcprof_hash_entry_t *entry)
{
	uint32_t position;

	if (top_count == 0U || entry->hits <= top[top_count - 1U].hits) {
		return;
	}
	for (position = top_count - 1U; position > 0U; --position) {
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

#if CONFIG_PC_PROFILER_RTW_DUMP_PROFILE
static void pcprof_rtw_dump_report(uint32_t sequence, uint32_t buffer)
{
	uint64_t total_cycles = 0U;
	uint32_t total_calls = 0U;
	uint32_t max_cycles = 0U;
	uint32_t i;

	for (i = 0U; i < PCPROF_RTW_DUMP_EXIT_COUNT; ++i) {
		pcprof_rtw_dump_exit_t *entry = &pcprof_rtw_dump_exits[buffer][i];

		total_calls += entry->calls;
		total_cycles += entry->cycles;
		if (entry->cycles_max > max_cycles) {
			max_cycles = entry->cycles_max;
		}
	}
	if (total_calls != 0U || pcprof_rtw_dump_reentry[buffer] != 0U ||
	    pcprof_rtw_dump_unmatched_exit[buffer] != 0U) {
		uint64_t window_cycles =
			(uint64_t)SystemCoreClock * PCPROF_REPORT_PERIOD_MS / 1000U;
		uint32_t cpu_10000 = window_cycles != 0U ?
			(uint32_t)((total_cycles * 10000U) / window_cycles) : 0U;

		rt_printf("[PCPROF][%lu][RTW_DUMP] critical calls=%lu "
			  "hold_cycles_avg/max=%lu/%lu cpu=%lu.%02lu%% "
			  "reentry/unmatched=%lu/%lu\r\n",
			  (unsigned long)sequence, (unsigned long)total_calls,
			  (unsigned long)(total_calls != 0U ?
					  total_cycles / total_calls : 0U),
			  (unsigned long)max_cycles,
			  (unsigned long)(cpu_10000 / 100U),
			  (unsigned long)(cpu_10000 % 100U),
			  (unsigned long)pcprof_rtw_dump_reentry[buffer],
			  (unsigned long)pcprof_rtw_dump_unmatched_exit[buffer]);
		for (i = 0U; i < PCPROF_RTW_DUMP_EXIT_COUNT; ++i) {
			pcprof_rtw_dump_exit_t *entry = &pcprof_rtw_dump_exits[buffer][i];

			if (entry->calls != 0U) {
				rt_printf("[PCPROF][%lu][RTW_DUMP] exit=0x%08lx "
					  "calls=%lu hold_cycles_avg/max=%lu/%lu\r\n",
					  (unsigned long)sequence,
					  (unsigned long)(pcprof_rtw_dump_base() +
						pcprof_rtw_dump_exit_offset[i]),
					  (unsigned long)entry->calls,
					  (unsigned long)(entry->cycles / entry->calls),
					  (unsigned long)entry->cycles_max);
			}
		}
		rt_printf("[PCPROF][%lu][RTW_DUMP] stages "
			  "txdesc calls/cycles=%lu/%llu cache calls/bytes/cycles=%lu/%lu/%llu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)pcprof_rtw_dump_txdesc[buffer].calls,
			  (unsigned long long)pcprof_rtw_dump_txdesc[buffer].cycles,
			  (unsigned long)pcprof_rtw_dump_cache[buffer].calls,
			  (unsigned long)pcprof_rtw_dump_cache[buffer].bytes,
			  (unsigned long long)pcprof_rtw_dump_cache[buffer].cycles);
	}
	memset(pcprof_rtw_dump_exits[buffer], 0,
	       sizeof(pcprof_rtw_dump_exits[buffer]));
	memset(&pcprof_rtw_dump_cache[buffer], 0,
	       sizeof(pcprof_rtw_dump_cache[buffer]));
	memset(&pcprof_rtw_dump_txdesc[buffer], 0,
	       sizeof(pcprof_rtw_dump_txdesc[buffer]));
	pcprof_rtw_dump_reentry[buffer] = 0U;
	pcprof_rtw_dump_unmatched_exit[buffer] = 0U;
}
#endif

static void pcprof_report(uint32_t sequence, uint32_t buffer, uint32_t count,
			  uint32_t invalid, uint32_t nested, uint32_t late,
			  uint32_t dropped, uint32_t isr_cycles,
			  uint32_t isr_cycles_max, uint32_t interval_cycles_sum,
			  uint32_t interval_cycles_max, uint32_t interval_count,
			  uint32_t late_10us, uint32_t late_100us,
			  uint32_t caller_attributed)
{
#if CONFIG_PC_PROFILER_PC_DETAIL
	pcprof_top_entry_t top[PCPROF_TOP_COUNT];
#endif
#if CONFIG_PC_PROFILER_RTW_RECV_DETAIL
	pcprof_top_entry_t rtw_recv_top[PCPROF_TARGET_TOP_COUNT];
	TaskHandle_t rtw_recv_task;
	uint32_t rtw_recv_hits = 0U;
	uint32_t rtw_recv_printed_hits = 0U;
#endif
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
#if CONFIG_PC_PROFILER_PC_DETAIL
	uint32_t printed_hits = 0U;
#endif
	uint32_t task_printed_hits = 0U;
	uint32_t late_avg_ns = interval_count != 0U && SystemCoreClock != 0U ?
		(uint32_t)(((uint64_t)interval_cycles_sum * 1000000000ULL) /
			   ((uint64_t)interval_count * SystemCoreClock)) : 0U;
	uint32_t late_max_ns = SystemCoreClock != 0U ?
		(uint32_t)(((uint64_t)interval_cycles_max * 1000000000ULL) /
			   SystemCoreClock) : 0U;
	uint64_t window_cycles =
		(uint64_t)SystemCoreClock * PCPROF_REPORT_PERIOD_MS / 1000U;

#if PCPROF_NEEDS_PC_HASH
	memset(pcprof_hash, 0, sizeof(pcprof_hash));
#endif
#if CONFIG_PC_PROFILER_PC_DETAIL
	memset(top, 0, sizeof(top));
#endif
#if CONFIG_PC_PROFILER_RTW_RECV_DETAIL
	memset(rtw_recv_top, 0, sizeof(rtw_recv_top));
#endif
	memset(task_entries, 0, sizeof(task_entries));
	memset(task_top, 0, sizeof(task_top));
	for (i = 0; i < count; ++i) {
#if PCPROF_NEEDS_PC_HASH
		pcprof_hash_add(pcprof_records[buffer][i].task,
				pcprof_records[buffer][i].pc,
				pcprof_records[buffer][i].raw_pc,
				pcprof_records[buffer][i].lr);
#endif
		pcprof_task_hit_add(task_entries, pcprof_records[buffer][i].task,
				    &task_entry_count, &task_overflow_hits);
	}
	task_count = uxTaskGetSystemState(pcprof_tasks,
					  PCPROF_MAX_TASK_SNAPSHOT, NULL);
#if CONFIG_PC_PROFILER_RTW_DUMP_PROFILE
	pcprof_tcp_ip_task = pcprof_task_handle("TCP_IP", task_count);
#endif
#if CONFIG_PC_PROFILER_RTW_RECV_DETAIL
	rtw_recv_task = pcprof_task_handle("rtw_recv_tasklet", task_count);
#endif
#if PCPROF_NEEDS_PC_HASH
	for (i = 0; i < PCPROF_HASH_SIZE; ++i) {
		if (pcprof_hash[i].hits == 0U) {
			continue;
		}
#if CONFIG_PC_PROFILER_PC_DETAIL
		pcprof_top_insert(top, PCPROF_TOP_COUNT, &pcprof_hash[i]);
#endif
#if CONFIG_PC_PROFILER_RTW_RECV_DETAIL
		if (pcprof_hash[i].task == rtw_recv_task) {
			rtw_recv_hits += pcprof_hash[i].hits;
			pcprof_top_insert(rtw_recv_top, PCPROF_TARGET_TOP_COUNT,
					  &pcprof_hash[i]);
		}
#endif
	}
#endif
	for (i = 0U; i < task_entry_count; ++i) {
		pcprof_task_top_insert(task_top, &task_entries[i]);
	}
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

#if CONFIG_PC_PROFILER_PC_DETAIL
	for (i = 0; i < PCPROF_TOP_COUNT && top[i].hits != 0U; ++i) {
		uint32_t pct_100 = count != 0U ?
			(uint32_t)(((uint64_t)top[i].hits * 10000U) / count) : 0U;

		printed_hits += top[i].hits;
		rt_printf("[PCPROF][%lu][PC] #%02lu task=%-24s p=%lu pc=0x%08lx "
			  "lr=0x%08lx raw=0x%08lx hits=%lu pct=%lu.%02lu%%\r\n",
			  (unsigned long)sequence, (unsigned long)(i + 1U),
			  pcprof_task_name(top[i].task, task_count),
			  (unsigned long)pcprof_task_priority(top[i].task, task_count),
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
#endif
#if CONFIG_PC_PROFILER_RTW_RECV_DETAIL
	for (i = 0; i < PCPROF_TARGET_TOP_COUNT &&
	     rtw_recv_top[i].hits != 0U; ++i) {
		uint32_t pct_100 = rtw_recv_hits != 0U ?
			(uint32_t)(((uint64_t)rtw_recv_top[i].hits * 10000U) /
				   rtw_recv_hits) : 0U;

		rtw_recv_printed_hits += rtw_recv_top[i].hits;
		rt_printf("[PCPROF][%lu][RTW_RECV_PC] #%02lu pc=0x%08lx "
			  "lr=0x%08lx raw=0x%08lx hits=%lu task_pct=%lu.%02lu%%\r\n",
			  (unsigned long)sequence, (unsigned long)(i + 1U),
			  (unsigned long)rtw_recv_top[i].pc,
			  (unsigned long)rtw_recv_top[i].lr,
			  (unsigned long)rtw_recv_top[i].raw_pc,
			  (unsigned long)rtw_recv_top[i].hits,
			  (unsigned long)(pct_100 / 100U),
			  (unsigned long)(pct_100 % 100U));
	}
	if (rtw_recv_hits > rtw_recv_printed_hits) {
		uint32_t others = rtw_recv_hits - rtw_recv_printed_hits;
		uint32_t pct_100 = (uint32_t)(((uint64_t)others * 10000U) /
					      rtw_recv_hits);

		rt_printf("[PCPROF][%lu][RTW_RECV_PC] others hits=%lu "
			  "task_pct=%lu.%02lu%%\r\n", (unsigned long)sequence,
			  (unsigned long)others, (unsigned long)(pct_100 / 100U),
			  (unsigned long)(pct_100 % 100U));
	}
#endif
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
		uint32_t timer_callbacks;
		uint32_t timer_irq;
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
		timer_callbacks = pcprof_timer_callbacks;
		timer_irq = pcprof_timer_irq;
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
		pcprof_timer_callbacks = 0U;
		/* Keep the IRQ and PC sample windows aligned, and deduct this
		 * profiler timer callbacks from the IRQ totals. */
		carbox_irq_profiler_snapshot(timer_irq, timer_callbacks);
		if (primask == 0U) {
			__enable_irq();
		}

		sequence++;
		pcprof_report(sequence, old_buffer, count, invalid, nested, late,
			      dropped, isr_cycles, isr_cycles_max,
			      interval_cycles_sum, interval_cycles_max,
			      interval_count, late_10us, late_100us,
			      caller_attributed);
		carbox_irq_profiler_report(sequence, PCPROF_REPORT_PERIOD_MS);
#if defined(CONFIG_SCREEN_BLOCK_PROFILE) && CONFIG_SCREEN_BLOCK_PROFILE
		carbox_screen_block_profile_report(sequence);
#endif
#if defined(CONFIG_SCREEN_TCP_ACK_PROFILE) && CONFIG_SCREEN_TCP_ACK_PROFILE
		lwip_diag_screen_tcp_ack_report(sequence);
#endif
#if defined(CONFIG_TCP_OWNED_AGE_PROFILE) && CONFIG_TCP_OWNED_AGE_PROFILE && \
	defined(CONFIG_TCP_OWNED_WRITE) && CONFIG_TCP_OWNED_WRITE && \
	(!defined(CONFIG_SCREEN_QUEUE_PROFILE) || !CONFIG_SCREEN_QUEUE_PROFILE)
		lwip_tcp_owned_report(sequence);
#endif
#if CONFIG_PC_PROFILER_RTW_DUMP_PROFILE
		pcprof_rtw_dump_report(sequence, old_buffer);
#endif
#if defined(CONFIG_GCD_SYNC_PROFILE) && CONFIG_GCD_SYNC_PROFILE
		gcd_sync_profiler_report(sequence);
#endif
#if defined(CONFIG_SCREEN_QUEUE_PROFILE) && CONFIG_SCREEN_QUEUE_PROFILE
		screen_queue_profiler_report(sequence);
		carbox_video_handover_report(sequence);
		carbox_screen_tx_direct_crypto_report(sequence);
#if defined(CONFIG_TCP_OWNED_WRITE) && CONFIG_TCP_OWNED_WRITE
		lwip_tcp_owned_report(sequence);
#endif
#endif
#if (defined(CONFIG_USB_HCD_PROFILE) && CONFIG_USB_HCD_PROFILE) || \
	(defined(CONFIG_USB_HCD_CHANNEL_PROFILE) && CONFIG_USB_HCD_CHANNEL_PROFILE)
		usb_hcd_profiler_report(sequence);
#endif
#if defined(CONFIG_NET_QUEUE_PROFILE) && CONFIG_NET_QUEUE_PROFILE
		net_queue_profiler_report(sequence);
#endif
#if defined(CONFIG_CRYPTO_ENGINE_PROFILE) && CONFIG_CRYPTO_ENGINE_PROFILE
		crypto_engine_profiler_report(sequence);
#endif
#if defined(CONFIG_MEMCPY_TASK_PROFILE) && CONFIG_MEMCPY_TASK_PROFILE
		carbox_memcpy_task_profiler_report(sequence,
						 PCPROF_REPORT_PERIOD_MS);
#endif
#if defined(CONFIG_WLAN_RX_SWAP_BRINGUP_PROFILE) && \
	CONFIG_WLAN_RX_SWAP_BRINGUP_PROFILE
		rltk_wlan_rx_swap_profile_report(sequence);
#endif
	}
}

void carbox_pc_profiler_start(void)
{
	/* Install before later drivers register or replace their IRQ vectors. */
	carbox_irq_profiler_init();
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
