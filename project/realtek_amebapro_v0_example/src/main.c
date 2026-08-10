#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "main.h"
#include "build_info.h"
#include "ff.h"
#include "vfs.h"
#include "carbox_vfs_compat.h"

#include "carbox/carbox_diag.h"
#include "carbox/aes_backend_select.h"
#include "carbox/aes_ctr_periodic_selftest.h"
#include "carbox/large_memcpy_gdma.h"
#include "carbox/pc_profiler.h"
#include "carbox/system_overclock.h"
#if defined(CONFIG_MEMCHECK)
#include "carbox/memcheck.h"
#include "shell.h"
#endif
#if defined(CONFIG_UART2_TEST)
extern void uart2_test(void);
#endif
#if !defined(CARBOX_EXPERIMENTAL_SMART_A_LINK)
#include <example_entry.h>
#endif
#include "platform_autoconf.h"
#include "hal_flash_boot.h"
#include "hal_uart.h"
#if defined(CONFIG_NET_GDMA_BENCH) && CONFIG_NET_GDMA_BENCH
#include "lwip_intf.h"
#include "osdep_service.h"
#endif

extern void console_init(void);
extern hal_uart_adapter_t log_uart;

#define CARBOX_LOGUART_BAUD  1500000//3000000

#if defined(CONFIG_NET_GDMA_BENCH) && CONFIG_NET_GDMA_BENCH
#define CARBOX_GDMA_BENCH_INTERVAL_MS 5000U
#define CARBOX_GDMA_BENCH_ITERATIONS  64U
#define CARBOX_GDMA_BENCH_WARMUP       4U
#define CARBOX_GDMA_BENCH_MAX_LEN    1500U
#define CARBOX_GDMA_BENCH_ALIGN        32U
#define CARBOX_GDMA_BENCH_ALLOC \
	(CARBOX_GDMA_BENCH_MAX_LEN + (3U * CARBOX_GDMA_BENCH_ALIGN))
#define CARBOX_GDMA_BENCH_STACK_BYTES 4096U

typedef struct carbox_gdma_bench_totals_s {
	uint64_t cpu_cycles;
	uint64_t gdma_cycles;
	uint64_t submit_cycles;
	uint64_t poll_cycles;
	uint64_t finish_cycles;
	uint64_t dma_total_cycles;
	uint32_t polls;
	uint32_t yields;
	uint32_t cpu_min_cycles;
	uint32_t cpu_max_cycles;
	uint32_t gdma_min_cycles;
	uint32_t gdma_max_cycles;
	uint32_t dma_bytes;
	uint32_t fallbacks;
	uint32_t errors;
	uint32_t cpu_mismatches;
	uint32_t gdma_mismatches;
	uint32_t first_bad_iter;
	uint32_t first_bad_index;
	uint8_t first_bad_expected;
	uint8_t first_bad_actual;
	uint8_t first_bad_is_gdma;
	uint8_t first_bad_valid;
} carbox_gdma_bench_totals_t;

static void carbox_gdma_record_mismatch(carbox_gdma_bench_totals_t *totals,
					const uint8_t *dst, const uint8_t *src,
					uint32_t len, uint32_t iteration,
					int is_gdma)
{
	uint32_t i;

	if (is_gdma) totals->gdma_mismatches++;
	else totals->cpu_mismatches++;
	if (totals->first_bad_valid) return;
	for (i = 0; i < len; ++i) {
		if (dst[i] != src[i]) {
			totals->first_bad_valid = 1U;
			totals->first_bad_is_gdma = is_gdma ? 1U : 0U;
			totals->first_bad_iter = iteration;
			totals->first_bad_index = i;
			totals->first_bad_expected = src[i];
			totals->first_bad_actual = dst[i];
			break;
		}
	}
}

static uint32_t carbox_gdma_cycles_to_ns(uint64_t cycles, uint32_t samples)
{
	uint64_t divisor = (uint64_t)SystemCoreClock * samples;

	if (divisor == 0U) {
		return 0U;
	}
	return (uint32_t)((cycles * 1000000000ULL + (divisor / 2U)) / divisor);
}

static void carbox_gdma_bench_case(uint32_t sequence, uint32_t len,
				   uint32_t offset, uint8_t *src_base,
				   uint8_t *dst_base, const void *allocation_end)
{
	carbox_gdma_bench_totals_t totals = { 0 };
	rltk_network_gdma_bench_sample_t sample;
	uint8_t *src = src_base + offset;
	uint8_t *dst = dst_base + offset;
	uint32_t start;
	uint32_t elapsed;
	uint32_t i;
	int result;

	for (i = 0; i < len; ++i) {
		src[i] = (uint8_t)(i * 29U + len + offset);
	}
	for (i = 0; i < CARBOX_GDMA_BENCH_WARMUP; ++i) {
		rtw_memset(dst, 0xA5, len);
		(void)rltk_network_gdma_benchmark_copy(dst, src, len,
						  allocation_end, &sample);
	}

	totals.cpu_min_cycles = UINT32_MAX;
	totals.gdma_min_cycles = UINT32_MAX;
	for (i = 0; i < CARBOX_GDMA_BENCH_ITERATIONS; ++i) {
		rtw_memset(dst, 0x5A, len);
		start = DWT->CYCCNT;
		rtw_memcpy(dst, src, len);
		elapsed = DWT->CYCCNT - start;
		totals.cpu_cycles += elapsed;
		if (elapsed < totals.cpu_min_cycles) totals.cpu_min_cycles = elapsed;
		if (elapsed > totals.cpu_max_cycles) totals.cpu_max_cycles = elapsed;
		if (rtw_memcmp(dst, src, len) != _TRUE) {
			carbox_gdma_record_mismatch(&totals, dst, src, len, i, 0);
		}

		rtw_memset(dst, 0xC3, len);
		start = DWT->CYCCNT;
		result = rltk_network_gdma_benchmark_copy(dst, src, len,
							 allocation_end, &sample);
		elapsed = DWT->CYCCNT - start;
		totals.gdma_cycles += elapsed;
		if (elapsed < totals.gdma_min_cycles) totals.gdma_min_cycles = elapsed;
		if (elapsed > totals.gdma_max_cycles) totals.gdma_max_cycles = elapsed;
		totals.submit_cycles += sample.submit_cycles;
		totals.poll_cycles += sample.poll_cycles;
		totals.finish_cycles += sample.finish_cycles;
		totals.dma_total_cycles += sample.dma_total_cycles;
		totals.polls += sample.poll_count;
		totals.yields += sample.yield_count;
		totals.dma_bytes += sample.dma_bytes;
		if (sample.dma_bytes == 0U) totals.fallbacks++;
		if (result != 0) totals.errors++;
		if (rtw_memcmp(dst, src, len) != _TRUE) {
			carbox_gdma_record_mismatch(&totals, dst, src, len, i, 1);
		}
	}

	rt_printf("[GDMA_BENCH][%lu] len=%lu off=%lu iter=%u "
		  "cpu_ns avg/min/max=%lu/%lu/%lu "
		  "gdma_ns avg/min/max=%lu/%lu/%lu "
		  "phase_ns submit/poll/finish/total=%lu/%lu/%lu/%lu "
		  "poll/yield=%lu/%lu "
		  "dma_avg=%luB fallback=%lu error=%lu mismatch_cpu/gdma=%lu/%lu\r\n",
		  (unsigned long)sequence, (unsigned long)len,
		  (unsigned long)offset, CARBOX_GDMA_BENCH_ITERATIONS,
		  (unsigned long)carbox_gdma_cycles_to_ns(totals.cpu_cycles,
							 CARBOX_GDMA_BENCH_ITERATIONS),
		  (unsigned long)carbox_gdma_cycles_to_ns(totals.cpu_min_cycles, 1U),
		  (unsigned long)carbox_gdma_cycles_to_ns(totals.cpu_max_cycles, 1U),
		  (unsigned long)carbox_gdma_cycles_to_ns(totals.gdma_cycles,
							 CARBOX_GDMA_BENCH_ITERATIONS),
		  (unsigned long)carbox_gdma_cycles_to_ns(totals.gdma_min_cycles, 1U),
		  (unsigned long)carbox_gdma_cycles_to_ns(totals.gdma_max_cycles, 1U),
		  (unsigned long)carbox_gdma_cycles_to_ns(totals.submit_cycles,
							 CARBOX_GDMA_BENCH_ITERATIONS),
		  (unsigned long)carbox_gdma_cycles_to_ns(totals.poll_cycles,
							 CARBOX_GDMA_BENCH_ITERATIONS),
		  (unsigned long)carbox_gdma_cycles_to_ns(totals.finish_cycles,
							 CARBOX_GDMA_BENCH_ITERATIONS),
		  (unsigned long)carbox_gdma_cycles_to_ns(totals.dma_total_cycles,
							 CARBOX_GDMA_BENCH_ITERATIONS),
		  (unsigned long)totals.polls, (unsigned long)totals.yields,
		  (unsigned long)(totals.dma_bytes / CARBOX_GDMA_BENCH_ITERATIONS),
		  (unsigned long)totals.fallbacks, (unsigned long)totals.errors,
		  (unsigned long)totals.cpu_mismatches,
		  (unsigned long)totals.gdma_mismatches);
	if (totals.first_bad_valid) {
		rt_printf("[GDMA_BENCH][%lu][MISMATCH] len=%lu off=%lu path=%s "
			  "iter=%lu index=%lu expected=%02x actual=%02x\r\n",
			  (unsigned long)sequence, (unsigned long)len,
			  (unsigned long)offset,
			  totals.first_bad_is_gdma ? "gdma" : "cpu",
			  (unsigned long)totals.first_bad_iter,
			  (unsigned long)totals.first_bad_index,
			  totals.first_bad_expected, totals.first_bad_actual);
	}
}

static void carbox_gdma_bench_task(void *param)
{
	static const uint16_t lengths[] = { 1024U, 1152U, 1280U, 1408U, 1500U };
	uint8_t *src_raw;
	uint8_t *dst_raw;
	uint8_t *src;
	uint8_t *dst;
	TickType_t last_wake;
	uint32_t sequence = 0;
	uint32_t round_start;
	uint32_t i;

	(void)param;
	src_raw = (uint8_t *)rtw_malloc(CARBOX_GDMA_BENCH_ALLOC);
	dst_raw = (uint8_t *)rtw_malloc(CARBOX_GDMA_BENCH_ALLOC);
	if (src_raw == NULL || dst_raw == NULL) {
		rt_printf("[GDMA_BENCH] buffer allocation failed\r\n");
		if (src_raw != NULL) rtw_mfree(src_raw, CARBOX_GDMA_BENCH_ALLOC);
		if (dst_raw != NULL) rtw_mfree(dst_raw, CARBOX_GDMA_BENCH_ALLOC);
		vTaskDelete(NULL);
		return;
	}
	src = (uint8_t *)(((uintptr_t)src_raw + CARBOX_GDMA_BENCH_ALIGN - 1U) &
			  ~(uintptr_t)(CARBOX_GDMA_BENCH_ALIGN - 1U));
	dst = (uint8_t *)(((uintptr_t)dst_raw + CARBOX_GDMA_BENCH_ALIGN - 1U) &
			  ~(uintptr_t)(CARBOX_GDMA_BENCH_ALIGN - 1U));

	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	rt_printf("[GDMA_BENCH] enabled interval=%ums iterations=%u core=%luHz\r\n",
		  CARBOX_GDMA_BENCH_INTERVAL_MS, CARBOX_GDMA_BENCH_ITERATIONS,
		  (unsigned long)SystemCoreClock);
	last_wake = xTaskGetTickCount();

	for (;;) {
		vTaskDelayUntil(&last_wake,
				pdMS_TO_TICKS(CARBOX_GDMA_BENCH_INTERVAL_MS));
		sequence++;
		round_start = DWT->CYCCNT;
		for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
			carbox_gdma_bench_case(sequence, lengths[i], 0U, src, dst,
					       dst_raw + CARBOX_GDMA_BENCH_ALLOC);
			carbox_gdma_bench_case(sequence, lengths[i], 2U, src, dst,
					       dst_raw + CARBOX_GDMA_BENCH_ALLOC);
		}
		rltk_network_gdma_benchmark_print_status(sequence);
		rt_printf("[GDMA_BENCH][%lu] round_us=%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)carbox_gdma_cycles_to_ns(
				  DWT->CYCCNT - round_start, 1U) / 1000U);
	}
}

static void carbox_gdma_bench_start(void)
{
	if (rltk_network_gdma_benchmark_init() != 0) {
		rt_printf("[GDMA_BENCH] dedicated channel reservation failed\r\n");
		return;
	}
	if (xTaskCreate(carbox_gdma_bench_task, "gdma_bench",
			CARBOX_GDMA_BENCH_STACK_BYTES / sizeof(StackType_t), NULL,
			tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		rt_printf("[GDMA_BENCH] task create failed\r\n");
	}
}
#endif

/*
 * Clock characterization aid.
 *
 * The supported system-clock API exposes only fixed 200/300 MHz PLL sources,
 * while the underlying analog PLL has additional SDM/fractional controls.  Log
 * the post-ROM register state before changing any clock code so that known-good
 * 200 MHz and 300 MHz boots can be compared and the PLL encoding derived.
 * These are read-only diagnostic accesses; do not write these registers here.
 */
#define CARBOX_SYSON_REG32(offset) \
	(*(volatile const uint32_t *)(0x40000000UL + (offset)))

static void carbox_clock_register_dump(void)
{
	uint32_t clk_ctrl1 = CARBOX_SYSON_REG32(0x14U);
	uint32_t pll_ctrl0 = CARBOX_SYSON_REG32(0x50U);
	uint32_t pll_ctrl1 = CARBOX_SYSON_REG32(0x54U);
	uint32_t pll_ctrl2 = CARBOX_SYSON_REG32(0x58U);
	uint32_t pll_ctrl3 = CARBOX_SYSON_REG32(0x5CU);
	uint32_t pll_test = CARBOX_SYSON_REG32(0xA0U);

	rt_printf("[CLOCK] SystemCoreClock=%lu Hz\r\n",
		  (unsigned long)SystemCoreClock);
	rt_printf("[CLOCK] SYSON 014=%08lx 050=%08lx 054=%08lx "
		  "058=%08lx 05c=%08lx 0a0=%08lx\r\n",
		  (unsigned long)clk_ctrl1,
		  (unsigned long)pll_ctrl0,
		  (unsigned long)pll_ctrl1,
		  (unsigned long)pll_ctrl2,
		  (unsigned long)pll_ctrl3,
		  (unsigned long)pll_test);
	rt_printf("[CLOCK] source=%s pll_source=%luMHz divider_en=%lu "
		  "divider_sel=0x%lx pll_ready=%lu\r\n",
		  (clk_ctrl1 & (1UL << 8)) ? "PLL" : "ANA-4M",
		  (clk_ctrl1 & 1UL) ? 200UL : 300UL,
		  (clk_ctrl1 >> 9) & 1UL,
		  (clk_ctrl1 >> 4) & 0xFUL,
		  (pll_test >> 26) & 1UL);
}

#if defined(CARBOX_EXPERIMENTAL_USB)


#define CARBOX_USB_START_DELAY_MS 8000U
#define CARBOX_USB_WRAPPER_STACK_BYTES 8192U

#define CARBOX_FATFS_ROOT_DUMP_MAX 64U
#define CARBOX_FATFS_DUMP_DELAY_MS 3000U
#define CARBOX_FATFS_DUMP_PERIOD_MS 5000U
#define CARBOX_FATFS_DUMP_REPEAT 0U
#define CARBOX_FATFS_DUMP_TASK_STACK_BYTES 8192U
static void carbox_usb_init_task(void *param)
{
	(void)param;
	carbox_diag_task_entry("usb_init_wrapper");
	rt_printf("[carbox_diag] usb init wrapper started, delay=%lu ms\r\n",
		  (unsigned long)CARBOX_USB_START_DELAY_MS);
	carbox_diag_stack_report(50);
	vTaskDelay((TickType_t)((CARBOX_USB_START_DELAY_MS + portTICK_PERIOD_MS - 1U) /
				portTICK_PERIOD_MS));
	rt_printf("[carbox_diag] usb init wrapper starting\r\n");
	// usb_ref_host_acm_start();
	usb_dev_init();
	carbox_diag_stack_report(50);
	usb_load_dev();
	carbox_diag_stack_report(50);
	rt_printf("[carbox_diag] usb init wrapper done\r\n");
	vTaskDelete(NULL);
}
#endif

char *get_build_version(void)
{
    return BOX_APP_VERSION;
}

#ifdef CONFIG_FATFS_TEST
static void carbox_fatfs_dump_root(void)
{
	DIR dir;
	FILINFO info;
	FRESULT res;
	const char *drive;
	char line[128];
	unsigned int count = 0;
	int ret;

	rt_printf("[carbox_fatfs] root dump start\r\n");


	vfs_init();
	ret = vfs_user_register(VFS_PREFIX, VFS_FATFS, VFS_INF_FLASH, VFS_REGION_1, VFS_RO);
	if (ret != 0) {
		rt_printf("[carbox_fatfs] VFS FatFS register failed: %d\r\n", ret);

		return;
	}

	drive = carbox_vfs_get_drive();
	if (drive == NULL) {
		rt_printf("[carbox_fatfs] no FatFS drive after register\r\n");

		return;
	}

	rt_printf("[carbox_fatfs] root path: %s\r\n", drive);

	res = f_opendir(&dir, drive);
	if (res != FR_OK) {
		rt_printf("[carbox_fatfs] f_opendir(%s) failed: %d\r\n", drive, res);

		return;
	}

	for (;;) {
		res = f_readdir(&dir, &info);
		if (res != FR_OK) {
			rt_printf("[carbox_fatfs] f_readdir failed: %d\r\n", res);
			break;
		}

		if (info.fname[0] == '\0') {
			break;
		}

		rt_printf("[carbox_fatfs] root[%02u] %c %-13s size=%lu attr=0x%02x\r\n",
			  count,
			  (info.fattrib & AM_DIR) ? 'D' : 'F',
			  info.fname,
			  (unsigned long)info.fsize,
			  (unsigned int)info.fattrib);


		count++;
		if (count >= CARBOX_FATFS_ROOT_DUMP_MAX) {
			rt_printf("[carbox_fatfs] root dump truncated at %u entries\r\n", count);
			break;
		}
	}

	f_closedir(&dir);
	rt_printf("[carbox_fatfs] root dump end, entries=%u\r\n", count);

}

static void carbox_fatfs_dump_task(void *param)
{
	unsigned int i;

	(void)param;
	vTaskDelay((TickType_t)((CARBOX_FATFS_DUMP_DELAY_MS + portTICK_PERIOD_MS - 1U) /
				portTICK_PERIOD_MS));


	raw_test();

	for (i = 0; CARBOX_FATFS_DUMP_REPEAT == 0U || i < CARBOX_FATFS_DUMP_REPEAT; i++) {
		if (CARBOX_FATFS_DUMP_REPEAT == 0U) {
			printf("[carbox_fatfs] dump task pass %u\n\r", i + 1U);
		} else {
			printf("[carbox_fatfs] dump task pass %u/%u\n\r",
			       i + 1U, CARBOX_FATFS_DUMP_REPEAT);
		}

		carbox_fatfs_dump_root();
		vTaskDelay((TickType_t)((CARBOX_FATFS_DUMP_PERIOD_MS + portTICK_PERIOD_MS - 1U) /
					portTICK_PERIOD_MS));
	}

	printf("[carbox_fatfs] dump task done\n\r");
	vTaskDelete(NULL);
}

static void carbox_fatfs_dump_start(void)
{
	if (xTaskCreate(carbox_fatfs_dump_task, "fatfs_dump",
			CARBOX_FATFS_DUMP_TASK_STACK_BYTES / sizeof(StackType_t), NULL,
			tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
		printf("[carbox_fatfs] dump task create failed\n\r");
	}
}
#endif


static void car_app_start_task(void *param)
{
	(void)param;
	do_initcalls();
	rt_printf("do_initcalls is OK\r\n");
#if defined(CARBOX_EXPERIMENTAL_SMART_A_LINK)
	void CarApp_Start(void);
	CarApp_Start();	
#endif

	vTaskDelete(NULL);
}
/**
  * @brief  Main program.
  * @param  None
  * @retval None
  */
void main(void)
{
	int ret = 0;
	int clock_status = carbox_system_overclock_early();

	/* Initialize log uart and at command service */
#if WINBOND_FLASH_UNPROTECT
        extern void flash_unprotect_winbond(void);
        flash_unprotect_winbond();
#endif
        console_init();
    hal_uart_set_baudrate(&log_uart, CARBOX_LOGUART_BAUD);
    rt_printf("main build_version %s\r\n",BOX_APP_VERSION);
	rt_printf("[CLOCK] overclock status=%d requested=%lu Hz\r\n",
		  clock_status, (unsigned long)CONFIG_SYS_PLL_TARGET_HZ);
	carbox_clock_register_dump();
	
#ifdef CONFIG_FATFS

	vfs_init();
	ret = vfs_user_register(VFS_FAT_PREFIX, VFS_FATFS, VFS_INF_FLASH, VFS_REGION_1, VFS_RO);
	if (ret != 0) {
		rt_printf("[vfs] FatFS register failed: %d\r\n", ret);
	}
	ret = vfs_user_register(VFS_PREFIX, VFS_LITTLEFS, VFS_INF_FLASH, VFS_REGION_2, VFS_RW);
	if (ret != 0) {
		rt_printf("[vfs] LittleFS register failed: %d\r\n", ret);
	}
	rt_printf("[vfs] fat:/ read-only, vfs:/ read-write, 0:/ aliases fat:/\r\n");

#endif


#if defined(CONFIG_FATFS_TEST)
	// carbox_fatfs_dump_start();
#endif
	/* dump FW slot info for OTA diagnostics */
	{
		uint8_t *info = (uint8_t *)get_fw_img_info_tbl();
		if (info != NULL) {
			/* fw_img_export_info_type_t layout:
			 * +0x04 fw1_start_offset, +0x08 fw2_start_offset,
			 * +0x0C fw1_sn, +0x10 fw2_sn,
			 * +0x14 fw1_valid, +0x15 fw2_valid, +0x16 loaded_fw_idx */
			uint32_t fw1_start = *(uint32_t *)(info + 0x04);
			uint32_t fw2_start = *(uint32_t *)(info + 0x08);
			uint32_t fw1_sn    = *(uint32_t *)(info + 0x0C);
			uint32_t fw2_sn    = *(uint32_t *)(info + 0x10);
			uint8_t  fw1_valid = info[0x14];
			uint8_t  fw2_valid = info[0x15];
			uint8_t  loaded    = info[0x16];
			rt_printf("fw_slot: loaded=%d "
				  "fw1(0x%08x sn=%u valid=%d) "
				  "fw2(0x%08x sn=%u valid=%d)\r\n",
				  loaded,
				  (unsigned int)fw1_start, (unsigned int)fw1_sn, fw1_valid,
				  (unsigned int)fw2_start, (unsigned int)fw2_sn, fw2_valid);
		}
	}
	/* init diagnostic tracing before any task creation */
	carbox_diag_init();
	carbox_large_memcpy_gdma_init();
	carbox_aes_backend_select();
	carbox_aes_ctr_periodic_selftest_start();
	carbox_diag_trace_enter("main");
	carbox_pc_profiler_start();

#if defined(CONFIG_NET_GDMA_BENCH) && CONFIG_NET_GDMA_BENCH
	carbox_gdma_bench_start();
#endif

#if defined(CONFIG_MEMCHECK)

	if (!shell_register(memcheck_main, "checkmem", "memcheck tool")) {
		rt_printf("[carbox] memcheck cmd register failed\r\n");
	}
#endif

#if ISP_BOOT_MODE_ENABLE == 0
#if !defined(CARBOX_EXPERIMENTAL_SMART_A_LINK)
	/* pre-processor of application example */
	pre_example_entry();
#endif

	/* wlan intialization */
#if defined(CONFIG_WIFI_NORMAL) && defined(CONFIG_NETWORK)
	wlan_network();
#endif
#endif

#if defined(CONFIG_UART2_TEST)
	uart2_test();
#endif

#if defined(CARBOX_EXPERIMENTAL_USB)
// 	/* Keep the customer boot flow: start USB host ACM automatically. */

	// if (xTaskCreate(carbox_usb_init_task, "usb_wrapper",
	// 		CARBOX_USB_WRAPPER_STACK_BYTES / sizeof(StackType_t), NULL,
	// 		tskIDLE_PRIORITY + 4, NULL) != pdPASS) {
	// 	rt_printf("[carbox_diag] usb wrapper task create failed\r\n");
	// }
#endif

	
#if !defined(CARBOX_EXPERIMENTAL_SMART_A_LINK)
	/* Execute application example */
	example_entry();
#endif

	if (xTaskCreate(car_app_start_task, "car_app_start",
			0x8000, NULL,
			tskIDLE_PRIORITY + 4, NULL) != pdPASS) {
		rt_printf("[carbox_diag] car_app_start_task wrapper task create failed\r\n");
	}


    	/*Enable Schedule, Start Kernel*/
#if defined(CONFIG_KERNEL) && !TASK_SCHEDULER_DISABLED
	#ifdef PLATFORM_FREERTOS
	vTaskStartScheduler();
	#endif
#endif

	carbox_diag_trace_exit("main");
	
	while(1);
}
