#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "main.h"
#include "build_info.h"
#include "ff.h"
#include "vfs.h"
#include "carbox_vfs_compat.h"

#include "carbox/carbox_diag.h"
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

extern void console_init(void);
extern hal_uart_adapter_t log_uart;

#define CARBOX_LOGUART_BAUD  1500000//3000000

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
	/* Initialize log uart and at command service */
#if WINBOND_FLASH_UNPROTECT
        extern void flash_unprotect_winbond(void);
        flash_unprotect_winbond();
#endif
        console_init();
    hal_uart_set_baudrate(&log_uart, CARBOX_LOGUART_BAUD);
    rt_printf("main build_version %s\r\n",BOX_APP_VERSION);
	
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
	carbox_diag_trace_enter("main");

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
