#include "memcpy_task_profiler.h"

#include "cmsis.h"
#include "diag.h"

#include <string.h>

#ifndef CONFIG_MEMCPY_TASK_PROFILE
#define CONFIG_MEMCPY_TASK_PROFILE 0
#endif

#if CONFIG_MEMCPY_TASK_PROFILE

#define MEMCPY_PROF_BUFFERS          2U
#define MEMCPY_PROF_CALLERS          8U
#define MEMCPY_PROF_REPORT_CALLERS   4U
#define MEMCPY_PROF_SIZE_BINS        6U
#define MEMCPY_PROF_ALIGN_BINS       4U
#define MEMCPY_PROF_REGIONS          5U

enum memcpy_prof_region {
	MEMCPY_PROF_REGION_ITCM = 0,
	MEMCPY_PROF_REGION_SRAM,
	MEMCPY_PROF_REGION_LPDDR,
	MEMCPY_PROF_REGION_XIP,
	MEMCPY_PROF_REGION_OTHER
};

typedef struct memcpy_prof_caller_s {
	uintptr_t caller;
	uint32_t calls;
	uint32_t bytes;
	uint32_t cycles;
	uint32_t min_len;
	uint32_t max_len;
} memcpy_prof_caller_t;

typedef struct memcpy_prof_target_s {
	uint32_t calls;
	uint32_t bytes;
	uint32_t cycle_samples;
	uint32_t cycles;
	uint32_t max_cycles;
	uint32_t min_len;
	uint32_t max_len;
	uint32_t align_calls[MEMCPY_PROF_ALIGN_BINS];
	uint32_t align_bytes[MEMCPY_PROF_ALIGN_BINS];
	uint32_t src_region_bytes[MEMCPY_PROF_REGIONS];
	uint32_t dst_region_bytes[MEMCPY_PROF_REGIONS];
	uint32_t bin_calls[MEMCPY_PROF_SIZE_BINS];
	uint32_t bin_bytes[MEMCPY_PROF_SIZE_BINS];
	uint32_t caller_overflow_calls;
	uint32_t caller_overflow_bytes;
	memcpy_prof_caller_t callers[MEMCPY_PROF_CALLERS];
} memcpy_prof_target_t;

void * volatile
	carbox_memcpy_profile_targets[CARBOX_MEMCPY_PROFILE_TARGETS];

static volatile uint32_t memcpy_prof_active_buffer;
static memcpy_prof_target_t
	memcpy_prof_stats[MEMCPY_PROF_BUFFERS][CARBOX_MEMCPY_PROFILE_TARGETS]
			 [CARBOX_MEMOP_COUNT];

static const char * const memcpy_prof_operation_names[CARBOX_MEMOP_COUNT] = {
	"memcpy", "memmove", "memset"
};

static uint32_t memcpy_prof_region(const void *pointer)
{
	uintptr_t address = (uintptr_t)pointer;

	if (address >= 0x00010000U && address < 0x00030000U)
		return MEMCPY_PROF_REGION_ITCM;
	if (address >= 0x20000000U && address < 0x20200A00U)
		return MEMCPY_PROF_REGION_SRAM;
	if (address >= 0x70000000U && address < 0x72000000U)
		return MEMCPY_PROF_REGION_LPDDR;
	if (address >= 0x98000000U && address < 0xA0000000U)
		return MEMCPY_PROF_REGION_XIP;
	return MEMCPY_PROF_REGION_OTHER;
}

static uint32_t memcpy_prof_alignment(const void *dst, const void *src,
				       unsigned int operation)
{
	uintptr_t destination = (uintptr_t)dst;

	if (operation == CARBOX_MEMOP_MEMSET) {
		if ((destination & 15U) == 0U)
			return 0U;
		if ((destination & 3U) == 0U)
			return 1U;
		return 2U;
	}

	if (((destination | (uintptr_t)src) & 15U) == 0U)
		return 0U;
	if (((destination | (uintptr_t)src) & 3U) == 0U)
		return 1U;
	if (((destination ^ (uintptr_t)src) & 3U) == 0U)
		return 2U;
	return 3U;
}

static uint32_t memcpy_prof_size_bin(uint32_t len)
{
	if (len <= 64U)
		return 0U;
	if (len <= 256U)
		return 1U;
	if (len <= 1024U)
		return 2U;
	if (len <= 4096U)
		return 3U;
	if (len <= 16384U)
		return 4U;
	return 5U;
}

__attribute__((noinline))
void carbox_memcpy_task_profiler_record(void *task, uintptr_t caller,
					const void *dst, const void *src,
					size_t length, unsigned int operation,
					uint32_t cycles, int cycles_valid)
{
	memcpy_prof_target_t *stats;
	memcpy_prof_caller_t *entry = NULL;
	uint32_t target;
	uint32_t i;
	uint32_t len;
	uint32_t bin;
	uint32_t alignment;

	if (task == NULL || length == 0U || length > UINT32_MAX ||
	    operation >= CARBOX_MEMOP_COUNT)
		return;

	if (task == carbox_memcpy_profile_targets[0])
		target = 0U;
	else if (task == carbox_memcpy_profile_targets[1])
		target = 1U;
	else
		return;

	len = (uint32_t)length;
	caller &= ~(uintptr_t)1U;
	stats = &memcpy_prof_stats[memcpy_prof_active_buffer][target][operation];
	stats->calls++;
	stats->bytes += len;
	if (cycles_valid) {
		stats->cycle_samples++;
		stats->cycles += cycles;
		if (cycles > stats->max_cycles)
			stats->max_cycles = cycles;
	}
	if (stats->min_len == 0U || len < stats->min_len)
		stats->min_len = len;
	if (len > stats->max_len)
		stats->max_len = len;

	alignment = memcpy_prof_alignment(dst, src, operation);
	stats->align_calls[alignment]++;
	stats->align_bytes[alignment] += len;
	stats->dst_region_bytes[memcpy_prof_region(dst)] += len;
	if (operation != CARBOX_MEMOP_MEMSET)
		stats->src_region_bytes[memcpy_prof_region(src)] += len;

	bin = memcpy_prof_size_bin(len);
	stats->bin_calls[bin]++;
	stats->bin_bytes[bin] += len;

	for (i = 0U; i < MEMCPY_PROF_CALLERS; ++i) {
		if (stats->callers[i].caller == caller) {
			entry = &stats->callers[i];
			break;
		}
		if (entry == NULL && stats->callers[i].caller == 0U)
			entry = &stats->callers[i];
	}
	if (entry == NULL) {
		stats->caller_overflow_calls++;
		stats->caller_overflow_bytes += len;
		return;
	}
	if (entry->caller == 0U)
		entry->caller = caller;
	entry->calls++;
	entry->bytes += len;
	if (cycles_valid)
		entry->cycles += cycles;
	if (entry->min_len == 0U || len < entry->min_len)
		entry->min_len = len;
	if (len > entry->max_len)
		entry->max_len = len;
}

void carbox_memcpy_task_profiler_set_targets(TaskHandle_t screen,
					     TaskHandle_t receiver)
{
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	carbox_memcpy_profile_targets[0] = (void *)screen;
	carbox_memcpy_profile_targets[1] = (void *)receiver;
	if (primask == 0U)
		__enable_irq();
}

static void memcpy_prof_report_target(uint32_t sequence, const char *name,
				      unsigned int operation,
				      const memcpy_prof_target_t *stats,
				      uint32_t window_ms)
{
	uint32_t avg = stats->calls != 0U ? stats->bytes / stats->calls : 0U;
	uint32_t cycle_avg = stats->cycle_samples != 0U ?
		stats->cycles / stats->cycle_samples : 0U;
	uint32_t cycles_per_kib = stats->bytes != 0U ?
		(uint32_t)(((uint64_t)stats->cycles * 1024U) / stats->bytes) : 0U;
	uint64_t window_cycles =
		(uint64_t)SystemCoreClock * window_ms / 1000U;
	uint32_t cpu_10000 = window_cycles != 0U ?
		(uint32_t)(((uint64_t)stats->cycles * 10000U) / window_cycles) : 0U;
	uint32_t rate = window_ms != 0U ?
		(uint32_t)(((uint64_t)stats->bytes * 1000U) / window_ms) : 0U;
	uint32_t used_mask = 0U;
	uint32_t rank;
	const char *operation_name = memcpy_prof_operation_names[operation];

	if (stats->calls == 0U)
		return;

	rt_printf("[MEMPROF][%lu][%s][%s] calls=%lu bytes=%lu rate=%luB/s "
		  "len_avg/min/max=%lu/%lu/%lu cycles n/total/avg/max=%lu/%lu/%lu/%lu "
		  "cyc_per_KiB=%lu cpu=%lu.%02lu%%\r\n",
		  (unsigned long)sequence, name, operation_name,
		  (unsigned long)stats->calls,
		  (unsigned long)stats->bytes, (unsigned long)rate,
		  (unsigned long)avg, (unsigned long)stats->min_len,
		  (unsigned long)stats->max_len,
		  (unsigned long)stats->cycle_samples,
		  (unsigned long)stats->cycles,
		  (unsigned long)cycle_avg,
		  (unsigned long)stats->max_cycles,
		  (unsigned long)cycles_per_kib,
		  (unsigned long)(cpu_10000 / 100U),
		  (unsigned long)(cpu_10000 % 100U));
	if (operation == CARBOX_MEMOP_MEMSET) {
		rt_printf("[MEMPROF][%lu][%s][%s] align dst16/dst4/unaligned "
			  "calls=%lu/%lu/%lu bytes=%lu/%lu/%lu\r\n",
			  (unsigned long)sequence, name, operation_name,
			  (unsigned long)stats->align_calls[0],
			  (unsigned long)stats->align_calls[1],
			  (unsigned long)stats->align_calls[2],
			  (unsigned long)stats->align_bytes[0],
			  (unsigned long)stats->align_bytes[1],
			  (unsigned long)stats->align_bytes[2]);
	} else {
		rt_printf("[MEMPROF][%lu][%s][%s] align both16/both4/same_mod4/diff_mod4 "
			  "calls=%lu/%lu/%lu/%lu bytes=%lu/%lu/%lu/%lu\r\n",
			  (unsigned long)sequence, name, operation_name,
			  (unsigned long)stats->align_calls[0],
			  (unsigned long)stats->align_calls[1],
			  (unsigned long)stats->align_calls[2],
			  (unsigned long)stats->align_calls[3],
			  (unsigned long)stats->align_bytes[0],
			  (unsigned long)stats->align_bytes[1],
			  (unsigned long)stats->align_bytes[2],
			  (unsigned long)stats->align_bytes[3]);
	}
	rt_printf("[MEMPROF][%lu][%s][%s] regions dst itcm/sram/lpddr/xip/other="
		  "%lu/%lu/%lu/%lu/%luB src=%lu/%lu/%lu/%lu/%luB\r\n",
		  (unsigned long)sequence, name, operation_name,
		  (unsigned long)stats->dst_region_bytes[0],
		  (unsigned long)stats->dst_region_bytes[1],
		  (unsigned long)stats->dst_region_bytes[2],
		  (unsigned long)stats->dst_region_bytes[3],
		  (unsigned long)stats->dst_region_bytes[4],
		  (unsigned long)stats->src_region_bytes[0],
		  (unsigned long)stats->src_region_bytes[1],
		  (unsigned long)stats->src_region_bytes[2],
		  (unsigned long)stats->src_region_bytes[3],
		  (unsigned long)stats->src_region_bytes[4]);
	rt_printf("[MEMPROF][%lu][%s][%s] bins <=64/<=256/<=1K/<=4K/<=16K/>16K "
		  "calls=%lu/%lu/%lu/%lu/%lu/%lu bytes=%lu/%lu/%lu/%lu/%lu/%lu\r\n",
		  (unsigned long)sequence, name, operation_name,
		  (unsigned long)stats->bin_calls[0],
		  (unsigned long)stats->bin_calls[1],
		  (unsigned long)stats->bin_calls[2],
		  (unsigned long)stats->bin_calls[3],
		  (unsigned long)stats->bin_calls[4],
		  (unsigned long)stats->bin_calls[5],
		  (unsigned long)stats->bin_bytes[0],
		  (unsigned long)stats->bin_bytes[1],
		  (unsigned long)stats->bin_bytes[2],
		  (unsigned long)stats->bin_bytes[3],
		  (unsigned long)stats->bin_bytes[4],
		  (unsigned long)stats->bin_bytes[5]);

	for (rank = 0U; rank < MEMCPY_PROF_REPORT_CALLERS; ++rank) {
		uint32_t i;
		uint32_t best = MEMCPY_PROF_CALLERS;
		uint32_t best_cost = 0U;

		for (i = 0U; i < MEMCPY_PROF_CALLERS; ++i) {
			uint32_t cost = stats->callers[i].cycles != 0U ?
				stats->callers[i].cycles : stats->callers[i].bytes;

			if ((used_mask & (1UL << i)) == 0U &&
			    cost > best_cost) {
				best = i;
				best_cost = cost;
			}
		}
		if (best == MEMCPY_PROF_CALLERS)
			break;
		used_mask |= 1UL << best;
		avg = stats->callers[best].calls != 0U ?
			stats->callers[best].bytes / stats->callers[best].calls : 0U;
		rt_printf("[MEMPROF][%lu][%s][%s][CALLER%lu] pc=0x%08lx "
			  "calls=%lu bytes=%lu cycles=%lu len_avg/min/max=%lu/%lu/%lu\r\n",
			  (unsigned long)sequence, name, operation_name,
			  (unsigned long)(rank + 1U),
			  (unsigned long)stats->callers[best].caller,
			  (unsigned long)stats->callers[best].calls,
			  (unsigned long)stats->callers[best].bytes,
			  (unsigned long)stats->callers[best].cycles,
			  (unsigned long)avg,
			  (unsigned long)stats->callers[best].min_len,
			  (unsigned long)stats->callers[best].max_len);
	}
	if (stats->caller_overflow_calls != 0U) {
		rt_printf("[MEMPROF][%lu][%s][%s] caller_overflow=%lu/%luB\r\n",
			  (unsigned long)sequence, name, operation_name,
			  (unsigned long)stats->caller_overflow_calls,
			  (unsigned long)stats->caller_overflow_bytes);
	}
}

void carbox_memcpy_task_profiler_report(uint32_t sequence,
					uint32_t window_ms)
{
	uint32_t old_buffer;
	uint32_t target;
	uint32_t operation;
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	old_buffer = memcpy_prof_active_buffer;
	memcpy_prof_active_buffer ^= 1U;
	if (primask == 0U)
		__enable_irq();

	for (target = 0U; target < CARBOX_MEMCPY_PROFILE_TARGETS; ++target) {
		const char *name = target == 0U ?
			"ScreenThread" : "AirPlayScreenReceiver";
		for (operation = 0U; operation < CARBOX_MEMOP_COUNT; ++operation) {
			memcpy_prof_report_target(sequence, name, operation,
				&memcpy_prof_stats[old_buffer][target][operation],
				window_ms);
		}
	}

	/* This buffer remains inactive until the next report, so it is safe to
	 * clear without extending the IRQ-off section. */
	memset(&memcpy_prof_stats[old_buffer][0], 0,
	       sizeof(memcpy_prof_stats[old_buffer]));
}

#else

void * volatile
	carbox_memcpy_profile_targets[CARBOX_MEMCPY_PROFILE_TARGETS];

void carbox_memcpy_task_profiler_record(void *task, uintptr_t caller,
					const void *dst, const void *src,
					size_t len, unsigned int operation,
					uint32_t cycles, int cycles_valid)
{
	(void)task; (void)caller; (void)dst; (void)src; (void)len;
	(void)operation; (void)cycles; (void)cycles_valid;
}

void carbox_memcpy_task_profiler_set_targets(TaskHandle_t screen,
					     TaskHandle_t receiver)
{
	(void)screen; (void)receiver;
}

void carbox_memcpy_task_profiler_report(uint32_t sequence, uint32_t window_ms)
{
	(void)sequence; (void)window_ms;
}

#endif
