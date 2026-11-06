#include <platform_stdlib.h>
#include <platform_opts.h>
#ifdef CONFIG_PLATFORM_8195A
#include <hal_adc.h>
#endif

#include <gpio_api.h>   // mbed
#include <sys_api.h>
#include <flash_api.h>
#include "analogin_api.h"

#if !defined(CONFIG_PLATFORM_8195BHP) && !defined(CONFIG_PLATFORM_8710C)
#include <rtl_lib.h>
#endif
#include <build_info.h>
#include "log_service.h"
#include "atcmd_sys.h"
#include "main.h"
#include "atcmd_wifi.h"
#include "device_lock.h"
#include "hal_flash_boot.h"
#include "osdep_service.h"
#include "fw_img.h"
#include "hal_timer.h"
#include "semphr.h"
#include "wifi_constants.h"
#if CARBOX_RECOVERY
#include "mp_calibration.h"
#include "recovery_mp.h"
#endif
#if defined(CONFIG_PLATFORM_8195BHP)
#include "nor_otp.h"
#endif

#if defined(configUSE_WAKELOCK_PMU) && (configUSE_WAKELOCK_PMU == 1)
#include "freertos_pmu.h"
#endif

#if !defined(CONFIG_PLATFORM_8195BHP) && !defined(CONFIG_PLATFORM_8710C)
extern u32 ConfigDebugErr;
extern u32 ConfigDebugInfo;
extern u32 ConfigDebugWarn;
#endif
extern u32 CmdDumpWord(IN u16 argc, IN u8 *argv[]);
extern u32 CmdWriteWord(IN u16 argc, IN u8 *argv[]);
extern int rltk_set_tx_power_percentage(rtw_tx_pwr_percentage_t power_percentage_idx);
extern int wext_private_command(const char *ifname, char *cmd, int show_msg);
extern void sys_reset(void);
extern int usbh_mp_init(void);
extern int usbh_mp_deinit(void);
typedef struct { int m_hI2C; int m_port; } CI2C;
extern CI2C *CI2C_new(void);
extern void CI2C_delete(CI2C *me);
extern int CI2C_Open(CI2C *me, char *device);
extern void CI2C_Close(CI2C *me);
extern int CI2C_Read(CI2C *me, unsigned char addr, unsigned char *reg, int reglen, unsigned char *data, int datalen);
#if defined(CONFIG_PLATFORM_8195BHP)
extern int efuse_logical_read(u16 laddr, u16 size, u8 *pbuf);
extern int efuse_logical_write(u16 addr, u16 cnts, u8 *data);
extern u32 OTP_LogicalMap_Read(u8 *pbuf, u32 addr, u32 len);
extern u32 OTP_LogicalMap_Write(u32 addr, u32 cnts, u8 *data);
#endif
#if CONFIG_UART_YMODEM
extern int uart_ymodem(void);
#endif

//#if ATCMD_VER == ATVER_1
#if (configGENERATE_RUN_TIME_STATS == 1)
static char cBuffer[512];

#define ATSS_SNAPSHOT_RETRIES	3U
#define ATSS_TASK_COUNT_MARGIN	2U
#define ATSS_MONITOR_STACK_SIZE	1024U
#define ATSS_MONITOR_PRIORITY	(tskIDLE_PRIORITY + 1U)

static TaskHandle_t atss_monitor_task;
static volatile BaseType_t atss_monitor_stop_requested;
static volatile BaseType_t atss_monitor_exiting;
static volatile BaseType_t atss_internal_enabled;
static volatile BaseType_t atss_cli_print_enabled;
static SemaphoreHandle_t atss_control_mutex;
static SemaphoreHandle_t atss_data_mutex;
static SemaphoreHandle_t atss_monitor_stopped;
static atss_task_stat_t *atss_latest_stats;
static size_t atss_latest_count;
static uint32_t atss_latest_sequence;
static int atss_latest_status = ATSS_NOT_RUNNING;

static SemaphoreHandle_t atss_install_semaphore(SemaphoreHandle_t candidate,
						SemaphoreHandle_t *target)
{
	SemaphoreHandle_t installed;

	taskENTER_CRITICAL();
	if (*target == NULL) {
		*target = candidate;
		candidate = NULL;
	}
	installed = *target;
	taskEXIT_CRITICAL();

	if (candidate != NULL) {
		vSemaphoreDelete(candidate);
	}
	return installed;
}

static int atss_sync_init(void)
{
	SemaphoreHandle_t candidate;

	if (atss_control_mutex == NULL) {
		candidate = xSemaphoreCreateMutex();
		if (candidate == NULL) {
			return ATSS_NO_MEMORY;
		}
		(void)atss_install_semaphore(candidate, &atss_control_mutex);
	}
	if (atss_data_mutex == NULL) {
		candidate = xSemaphoreCreateMutex();
		if (candidate == NULL) {
			return ATSS_NO_MEMORY;
		}
		(void)atss_install_semaphore(candidate, &atss_data_mutex);
	}
	if (atss_monitor_stopped == NULL) {
		candidate = xSemaphoreCreateBinary();
		if (candidate == NULL) {
			return ATSS_NO_MEMORY;
		}
		(void)atss_install_semaphore(candidate, &atss_monitor_stopped);
	}
	return ATSS_OK;
}

static void atss_copy_name(char dst[ATSS_TASK_NAME_LEN], const char *src)
{
	size_t index;

	if (src == NULL) {
		src = "<unknown>";
	}
	for (index = 0; index + 1U < ATSS_TASK_NAME_LEN && src[index] != '\0'; index++) {
		dst[index] = src[index];
	}
	dst[index] = '\0';
}

static TaskStatus_t *atss_take_snapshot(UBaseType_t *task_count, uint32_t *wall_time_us)
{
	TaskStatus_t *tasks;
	UBaseType_t capacity;
	UBaseType_t count;
	uint32_t snapshot_wall_us;
	unsigned int retry;

	for (retry = 0; retry < ATSS_SNAPSHOT_RETRIES; retry++) {
		capacity = uxTaskGetNumberOfTasks() + ATSS_TASK_COUNT_MARGIN;
		tasks = pvPortMalloc(capacity * sizeof(*tasks));
		if (tasks == NULL) {
			return NULL;
		}

		/*
		 * Keep the GTimer boundary and DWT task-counter snapshot in the same
		 * scheduler-suspended interval. uxTaskGetSystemState() uses a nested
		 * scheduler suspension, which is supported by FreeRTOS. Without this
		 * outer level, its final xTaskResumeAll() could preempt this monitor
		 * before the wall-clock read and make utilization appear too low.
		 * Interrupts remain enabled while the scheduler is suspended.
		 */
		vTaskSuspendAll();
		snapshot_wall_us = hal_read_curtime_us();
		count = uxTaskGetSystemState(tasks, capacity, NULL);
		(void)xTaskResumeAll();
		if (count != 0U) {
			*wall_time_us = snapshot_wall_us;
			*task_count = count;
			return tasks;
		}

		vPortFree(tasks);
	}

	return NULL;
}

static int atss_calculate_sample(const TaskStatus_t *start_tasks,
				 UBaseType_t start_count,
				 uint32_t start_wall_us,
				 const TaskStatus_t *end_tasks,
				 UBaseType_t end_count,
				 uint32_t end_wall_us,
				 atss_task_stat_t **out_stats,
				 size_t *out_count,
				 uint32_t *out_window_us)
{
	UBaseType_t start_index;
	UBaseType_t end_index;
	uint32_t wall_delta_us;
	uint32_t task_delta;
	uint32_t stack_free_min_bytes;
	uint64_t window_cycles;
	const char *task_name;
	TaskDebugInfo_t task_debug_info;
	atss_task_stat_t *stats;

	wall_delta_us = end_wall_us - start_wall_us;
	if (wall_delta_us == 0U || SystemCoreClock == 0U) {
		return ATSS_NOT_READY;
	}
	window_cycles = ((uint64_t)wall_delta_us * (uint64_t)SystemCoreClock) /
			1000000ULL;
	if (window_cycles == 0ULL) {
		return ATSS_NOT_READY;
	}

	stats = pvPortMalloc((size_t)end_count * sizeof(*stats));
	if (stats == NULL) {
		return ATSS_NO_MEMORY;
	}

	for (end_index = 0; end_index < end_count; end_index++) {
		task_delta = end_tasks[end_index].ulRunTimeCounter;

		for (start_index = 0; start_index < start_count; start_index++) {
			if ((end_tasks[end_index].xHandle == start_tasks[start_index].xHandle) &&
			    (end_tasks[end_index].xTaskNumber == start_tasks[start_index].xTaskNumber)) {
				task_delta -= start_tasks[start_index].ulRunTimeCounter;
				break;
			}
		}

		stats[end_index].priority =
			(uint32_t)end_tasks[end_index].uxCurrentPriority;
		stats[end_index].runtime_us =
			(uint32_t)(((uint64_t)task_delta * 1000000ULL +
				    ((uint64_t)SystemCoreClock / 2ULL)) /
				   (uint64_t)SystemCoreClock);
		stats[end_index].cpu_utilization_x10 =
			(uint32_t)(((uint64_t)task_delta * 1000ULL +
				    (window_cycles / 2ULL)) /
				   window_cycles);

		if (xTaskGetDebugInfo(end_tasks[end_index].xHandle, &task_debug_info) == pdPASS) {
			task_name = task_debug_info.pcTaskName;
			stats[end_index].stack_size_bytes =
				task_debug_info.ulStackSizeBytes;
			stats[end_index].stack_used_bytes =
				task_debug_info.ulStackUsedBytes;
		} else {
			task_name = end_tasks[end_index].pcTaskName;
			stats[end_index].stack_size_bytes = 0U;
			stats[end_index].stack_used_bytes = 0U;
		}
		atss_copy_name(stats[end_index].task_name, task_name);

		stack_free_min_bytes =
			(uint32_t)end_tasks[end_index].usStackHighWaterMark * (uint32_t)sizeof(StackType_t);
		if (stats[end_index].stack_size_bytes >= stack_free_min_bytes) {
			stats[end_index].stack_peak_bytes =
				stats[end_index].stack_size_bytes - stack_free_min_bytes;
		} else {
			stats[end_index].stack_peak_bytes = 0U;
		}
	}

	*out_stats = stats;
	*out_count = (size_t)end_count;
	*out_window_us = wall_delta_us;
	return ATSS_OK;
}

static void atss_print_sample(const atss_task_stat_t *stats, size_t count,
			      uint32_t window_us)
{
	size_t index;
	uint32_t utilization_x100;

	AT_PRINTK("[ATSS]: %-31s %5s %7s %10s %12s %12s %12s",
		 "Task", "Prio", "CPU", "Time(us)", "StackSize(B)", "StackUsed(B)", "StackPeak(B)");

	for (index = 0; index < count; index++) {
		utilization_x100 =
			(uint32_t)(((uint64_t)stats[index].runtime_us * 10000ULL +
				    ((uint64_t)window_us / 2ULL)) /
				   (uint64_t)window_us);
		AT_PRINTK("[ATSS]: %-31s %5u %4u.%02u%% %10u %12u %12u %12u",
			 stats[index].task_name,
			 (unsigned int)stats[index].priority,
			 (unsigned int)(utilization_x100 / 100U),
			 (unsigned int)(utilization_x100 % 100U),
			 (unsigned int)stats[index].runtime_us,
			 (unsigned int)stats[index].stack_size_bytes,
			 (unsigned int)stats[index].stack_used_bytes,
			 (unsigned int)stats[index].stack_peak_bytes);
	}
}

static void atss_set_latest_status(int status)
{
	if (atss_data_mutex == NULL) {
		return;
	}
	xSemaphoreTake(atss_data_mutex, portMAX_DELAY);
	if (atss_latest_stats != NULL) {
		vPortFree(atss_latest_stats);
		atss_latest_stats = NULL;
	}
	atss_latest_count = 0U;
	atss_latest_status = status;
	xSemaphoreGive(atss_data_mutex);
}

static void atss_publish_sample(atss_task_stat_t *stats, size_t count)
{
	xSemaphoreTake(atss_data_mutex, portMAX_DELAY);
	if (atss_latest_stats != NULL) {
		vPortFree(atss_latest_stats);
	}
	atss_latest_stats = stats;
	atss_latest_count = count;
	atss_latest_sequence++;
	atss_latest_status = ATSS_OK;
	xSemaphoreGive(atss_data_mutex);
}

static void atss_monitor(void *param)
{
	TaskStatus_t *start_tasks;
	TaskStatus_t *end_tasks;
	atss_task_stat_t *sample_stats;
	size_t sample_count;
	UBaseType_t start_count;
	UBaseType_t end_count;
	uint32_t start_wall_us;
	uint32_t end_wall_us;
	uint32_t sample_window_us;
	int status;
	BaseType_t normal_stop = pdFALSE;

	(void)param;

	start_tasks = atss_take_snapshot(&start_count, &start_wall_us);
	if (start_tasks == NULL) {
		atss_set_latest_status(ATSS_NO_MEMORY);
		if (atss_cli_print_enabled != pdFALSE) {
			AT_PRINTK("[ATSS]: ERROR: unable to allocate the initial snapshot");
		}
		goto exit;
	}
	if (atss_cli_print_enabled != pdFALSE) {
		AT_PRINTK("[ATSS]: runtime=DWT frequency=%luHz resolution=1cycle wallclock=GTIMER",
			 (unsigned long)SystemCoreClock);
	}

	while (1) {
		(void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ATSS_SAMPLE_PERIOD_MS));
		if (atss_monitor_stop_requested != pdFALSE) {
			normal_stop = pdTRUE;
			break;
		}

		end_tasks = atss_take_snapshot(&end_count, &end_wall_us);
		if (end_tasks == NULL) {
			atss_set_latest_status(ATSS_NO_MEMORY);
			if (atss_cli_print_enabled != pdFALSE) {
				AT_PRINTK("[ATSS]: ERROR: unable to allocate a snapshot");
			}
			vPortFree(start_tasks);
			start_tasks = atss_take_snapshot(&start_count, &start_wall_us);
			if (start_tasks == NULL) {
				goto exit;
			}
			continue;
		}

		sample_stats = NULL;
		sample_count = 0U;
		sample_window_us = 0U;
		status = atss_calculate_sample(start_tasks, start_count, start_wall_us,
					       end_tasks, end_count, end_wall_us,
					       &sample_stats, &sample_count,
					       &sample_window_us);

		vPortFree(start_tasks);
		start_tasks = end_tasks;
		start_count = end_count;
		start_wall_us = end_wall_us;

		if (atss_monitor_stop_requested != pdFALSE) {
			if (sample_stats != NULL) {
				vPortFree(sample_stats);
			}
			normal_stop = pdTRUE;
			break;
		}
		if (status == ATSS_OK) {
			atss_publish_sample(sample_stats, sample_count);
			if (atss_cli_print_enabled != pdFALSE) {
				atss_print_sample(sample_stats, sample_count,
						  sample_window_us);
			}
		} else {
			atss_set_latest_status(status);
			if (atss_cli_print_enabled != pdFALSE) {
				AT_PRINTK("[ATSS]: ERROR: unable to calculate a sample (%d)",
					 status);
			}
		}
	}

	vPortFree(start_tasks);

exit:
	if (normal_stop != pdFALSE) {
		atss_set_latest_status(ATSS_NOT_RUNNING);
	}
	taskENTER_CRITICAL();
	atss_monitor_exiting = pdTRUE;
	atss_monitor_task = NULL;
	atss_monitor_stop_requested = pdFALSE;
	taskEXIT_CRITICAL();
	xSemaphoreGive(atss_monitor_stopped);
	vTaskDelete(NULL);
}

static int atss_set_enabled(BaseType_t cli, BaseType_t enable)
{
	BaseType_t should_stop;
	TaskHandle_t task_to_stop;
	int status;

	status = atss_sync_init();
	if (status != ATSS_OK) {
		return status;
	}

	xSemaphoreTake(atss_control_mutex, portMAX_DELAY);
	if (cli != pdFALSE) {
		atss_cli_print_enabled = enable;
	} else {
		atss_internal_enabled = enable;
	}

	if (enable != pdFALSE) {
		if (atss_monitor_task == NULL) {
			if (atss_monitor_exiting != pdFALSE) {
				xSemaphoreTake(atss_monitor_stopped, portMAX_DELAY);
				atss_monitor_exiting = pdFALSE;
			}
			while (xSemaphoreTake(atss_monitor_stopped, 0) == pdTRUE) {
			}
			atss_monitor_stop_requested = pdFALSE;
			atss_set_latest_status(ATSS_NOT_READY);
			if (xTaskCreate(atss_monitor,
					(const char *)"atss_monitor",
					ATSS_MONITOR_STACK_SIZE,
					NULL,
					ATSS_MONITOR_PRIORITY,
					&atss_monitor_task) != pdPASS) {
				atss_monitor_task = NULL;
				if (cli != pdFALSE) {
					atss_cli_print_enabled = pdFALSE;
				} else {
					atss_internal_enabled = pdFALSE;
				}
				atss_set_latest_status(ATSS_NO_MEMORY);
				xSemaphoreGive(atss_control_mutex);
				return ATSS_NO_MEMORY;
			}
		}
		xSemaphoreGive(atss_control_mutex);
		return ATSS_OK;
	}

	should_stop = (atss_internal_enabled == pdFALSE) &&
		      (atss_cli_print_enabled == pdFALSE);
	task_to_stop = atss_monitor_task;
	if (should_stop != pdFALSE && task_to_stop != NULL) {
		atss_monitor_stop_requested = pdTRUE;
		xTaskNotifyGive(task_to_stop);
		xSemaphoreTake(atss_monitor_stopped, portMAX_DELAY);
		atss_monitor_exiting = pdFALSE;
	} else if (should_stop != pdFALSE &&
		   atss_monitor_exiting != pdFALSE) {
		xSemaphoreTake(atss_monitor_stopped, portMAX_DELAY);
		atss_monitor_exiting = pdFALSE;
		atss_set_latest_status(ATSS_NOT_RUNNING);
	} else if (should_stop != pdFALSE) {
		atss_set_latest_status(ATSS_NOT_RUNNING);
	}
	xSemaphoreGive(atss_control_mutex);
	return ATSS_OK;
}

int atss_stats_start(void)
{
	return atss_set_enabled(pdFALSE, pdTRUE);
}

int atss_stats_get(atss_task_stat_t *stats,
		   size_t capacity,
		   size_t *task_count,
		   uint32_t *sequence,
		   uint32_t *sample_period_ms)
{
	size_t count;
	int status;

	if (task_count == NULL || (stats == NULL && capacity != 0U)) {
		return ATSS_INVALID_ARGUMENT;
	}
	status = atss_sync_init();
	if (status != ATSS_OK) {
		return status;
	}

	xSemaphoreTake(atss_data_mutex, portMAX_DELAY);
	count = atss_latest_count;
	*task_count = count;
	if (sequence != NULL) {
		*sequence = atss_latest_sequence;
	}
	if (sample_period_ms != NULL) {
		*sample_period_ms = ATSS_SAMPLE_PERIOD_MS;
	}
	status = atss_latest_status;
	if (status == ATSS_OK && capacity < count) {
		status = ATSS_BUFFER_TOO_SMALL;
	} else if (status == ATSS_OK && count != 0U) {
		memcpy(stats, atss_latest_stats, count * sizeof(*stats));
	}
	xSemaphoreGive(atss_data_mutex);
	return status;
}

int atss_stats_stop(void)
{
	return atss_set_enabled(pdFALSE, pdFALSE);
}
#else
int atss_stats_start(void)
{
	return ATSS_UNAVAILABLE;
}

int atss_stats_get(atss_task_stat_t *stats,
		   size_t capacity,
		   size_t *task_count,
		   uint32_t *sequence,
		   uint32_t *sample_period_ms)
{
	(void)stats;
	(void)capacity;
	if (task_count != NULL) {
		*task_count = 0U;
	}
	if (sequence != NULL) {
		*sequence = 0U;
	}
	if (sample_period_ms != NULL) {
		*sample_period_ms = ATSS_SAMPLE_PERIOD_MS;
	}
	return (task_count == NULL) ? ATSS_INVALID_ARGUMENT : ATSS_UNAVAILABLE;
}

int atss_stats_stop(void)
{
	return ATSS_UNAVAILABLE;
}
#endif
//#endif

#include "module_audio.h"
#if CONFIG_MMF_AUDIO_DEBUG
#include <audio_debug.h>
void fATAD(void *arg)
{
	int argc = 0;
	char *argv[MAX_ARGC] = {0};
	argc = parse_param(arg, argv);

	if(strcmp(argv[1], "start")==0){
		int time = atoi(argv[2]);
		printf("start mic dump %d s \n\r", time);
		audio_rx_debug_init(time);
		if((argc == 3) || (strcmp(argv[3],"mic")==0)){
			audio_rx_debug_start(1);
		}
		/*else if(strcmp(argv[3],"aec")==0){
			audio_rx_debug_start(3);
		}*/

	}else if(strcmp(argv[1], "dump")==0){
		if(argc == 2){
		printf("dump buffered audio input\n\r");
		//audio_debug_buf_print(audio_rx_debug_buffer);
		audio_rx_debug_dump();
		}else if(argc==3){
			if(strcmp(argv[2], "aec")==0){
				audio_rx_aec_dump();
			}
		}
	}
}

void fATAS(void *arg)
{
	char *argv[MAX_ARGC] = {0};
	parse_param(arg, argv);

	if(strcmp(argv[1], "start")==0){
		printf("start sending buffer\n\r");
		audio_tx_debug_get();
		audio_debug_buf_print(audio_tx_debug_buffer);
		audio_tx_debug_start();

	}else if(strcmp(argv[1], "play")==0){
		if(strcmp(argv[2], "buf")==0){
			printf("play sending buffer\n\r");
			audio_tx_debug_start();
		}else if (strcmp(argv[2], "tone")==0){
			int freq = atoi(argv[3]);
			//audio_debug_gen_tone(audio_tx_debug_buf, freq);
		}
	}else if(strcmp(argv[1], "send")==0){
		int time = atoi(argv[2]);
		printf("start speaker dump \n\r");
		audio_tx_debug_init(time);
	}
}
#endif// CONFIG_MMF_AUDIO_DEBUG

#if CONFIG_MMF_AUDIO_ATAF
extern int module_audio_vad;
extern int module_audio_agc;
extern int module_audio_ns;
extern int module_audio_aec;
void fATAF(void *arg)
{
	//int argc = 0;
	char *argv[MAX_ARGC] = {0};
	parse_param(arg, argv);

	if(strcmp(argv[1], "agc")==0){
		module_audio_agc = argv[2][0]-'0';
	}else if(strcmp(argv[1], "ns")==0){
		module_audio_ns = argv[2][0]-'0';
	}else if(strcmp(argv[1], "vad")==0){
		module_audio_vad = argv[2][0]-'0';
	}else if(strcmp(argv[1], "aec")==0){
		module_audio_aec = argv[2][0]-'0';
	}
}
#endif // CONFIG_MMF_AUDIO_ATAF

//-------- AT SYS commands ---------------------------------------------------------------
#if ATCMD_VER == ATVER_1
void fATSD(void *arg)
{
#if !defined(CONFIG_PLATFORM_8710C)
	int argc = 0;
	char *argv[MAX_ARGC] = {0};

	AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSD]: _AT_SYSTEM_DUMP_REGISTER_");
	if(!arg){
		AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSD] Usage: ATSD=REGISTER");
		return;
	}
	argc = parse_param(arg, argv);
	if(argc == 2 || argc == 3)
		CmdDumpWord(argc-1, (unsigned char**)(argv+1));
#endif
}

void fATSE(void *arg)
{
#if !defined(CONFIG_PLATFORM_8710C)
	int argc = 0;
	char *argv[MAX_ARGC] = {0};

	AT_DBG_MSG(AT_FLAG_EDIT, AT_DBG_ALWAYS, "[ATSE]: _AT_SYSTEM_EDIT_REGISTER_");
	if(!arg){
		AT_DBG_MSG(AT_FLAG_EDIT, AT_DBG_ALWAYS, "[ATSE] Usage: ATSE=REGISTER[VALUE]");
		return;
	}
	argc = parse_param(arg, argv);
	if(argc == 3)
		CmdWriteWord(argc-1, (unsigned char**)(argv+1));
#endif
}

void fATSC(void *arg)
{
	AT_DBG_MSG(AT_FLAG_OTA, AT_DBG_ALWAYS, "[ATSC]: _AT_SYSTEM_CLEAR_OTA_SIGNATURE_");
	sys_clear_ota_signature();
}

void fATSR(void *arg)
{
	AT_DBG_MSG(AT_FLAG_OTA, AT_DBG_ALWAYS, "[ATSR]: _AT_SYSTEM_RECOVER_OTA_SIGNATURE_");
	sys_recover_ota_signature();
}

/* ---- 手动 OTA 槽切换（诊断用） ----
 * OTARECOVER：切到 OTA2（fw2），OTACLEAR：切到 OTA1（fw1）。
 * 通过翻转 fw1 首扇区 signature 首字节使 fw1 有效<->无效，重启后由 boot
 * 根据 fw1_valid 选择加载 fw1 还是 fw2。 */
#define CARBOX_OTA_SECTOR_SIZE 0x1000

static int carbox_ota_toggle_fw1_signature(uint32_t fw1_addr)
{
	flash_t flash;
	uint8_t *pbuf;

	pbuf = malloc(CARBOX_OTA_SECTOR_SIZE);
	if (!pbuf) {
		AT_PRINTK("[OTA] alloc sector buf fail");
		return -1;
	}

	/* 先取得互斥锁，再关中断；不能在临界区阻塞等待另一个flash任务。 */
	device_mutex_lock(RT_DEV_LOCK_FLASH);
	rtw_enter_critical(NULL, NULL);
	flash_stream_read(&flash, fw1_addr, CARBOX_OTA_SECTOR_SIZE, pbuf);
	pbuf[0] = ~(pbuf[0]); /* 翻转 signature 首字节：有效<->无效 */
	flash_erase_sector(&flash, fw1_addr);
	flash_burst_write(&flash, fw1_addr, CARBOX_OTA_SECTOR_SIZE, pbuf);
	rtw_exit_critical(NULL, NULL);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);

	free(pbuf);
	return 0;
}

/* OTARECOVER：切到 OTA2（fw2），重启生效 */
void fOTARECOVER(void *arg)
{
	fw_img_export_info_type_t *info = (fw_img_export_info_type_t *)get_fw_img_info_tbl();
	(void)arg;

	if (info == NULL) { AT_PRINTK("[OTARECOVER] slot info unavailable"); return; }
    if (info->loaded_fw_idx == 2 && info->fw1_valid == 0) {
        AT_PRINTK("[OTARECOVER] already in recovery"); return;
    }
    /* 主线默认镜像可能仅含FW1；没有有效FW2时不得清除当前启动签名。 */
    if (info->fw2_valid != 1) {
        AT_PRINTK("[OTARECOVER] no valid recovery FW2; switch rejected"); return;
    }
    AT_PRINTK("[OTARECOVER] fw1_valid=%d fw2_valid=%d loaded=%d",
		  info->fw1_valid, info->fw2_valid, info->loaded_fw_idx);

	if (info->fw1_valid == 1) {
		if (carbox_ota_toggle_fw1_signature(info->fw1_start_offset) != 0) return;
		AT_PRINTK("[OTARECOVER] fw1 signature cleared, switch to fw2");
	} else {
		AT_PRINTK("[OTARECOVER] fw1 already invalid");
	}
	sys_reset();
}

/* OTACLEAR：切到 OTA1（fw1），重启生效 */
void fOTACLEAR(void *arg)
{
	fw_img_export_info_type_t *info = (fw_img_export_info_type_t *)get_fw_img_info_tbl();
	(void)arg;

	if (info == NULL) { AT_PRINTK("[OTACLEAR] slot info unavailable"); return; }
    AT_PRINTK("[OTACLEAR] fw1_valid=%d fw2_valid=%d loaded=%d",
		  info->fw1_valid, info->fw2_valid, info->loaded_fw_idx);

	if (info->fw1_valid == 0) {
		if (carbox_ota_toggle_fw1_signature(info->fw1_start_offset) != 0) return;
		AT_PRINTK("[OTACLEAR] fw1 signature recovered, switch to fw1");
	} else {
		AT_PRINTK("[OTACLEAR] fw1 already valid");
	}
	sys_reset();
}

/* set_tx_power：设置 WLAN 发射功率百分比。
 * 参数 0..4 分别对应 100% / 75% / 50% / 25% / 12.5%。 */
void fSETTXPOWER(void *arg)
{
	int argc = 0;
	char *argv[MAX_ARGC] = {0};
	int idx;

	if (!arg) {
		AT_PRINTK("[set_tx_power] Usage: set_tx_power=<0..4> (0:100%%,1:75%%,2:50%%,3:25%%,4:12.5%%)");
		return;
	}

	argc = parse_param(arg, argv);
	if (argc != 2) {
		AT_PRINTK("[set_tx_power] Usage: set_tx_power=<0..4>");
		return;
	}

	idx = atoi(argv[1]);
	if (idx < (int)RTW_TX_PWR_PERCENTAGE_100 || idx > (int)RTW_TX_PWR_PERCENTAGE_12_5) {
		AT_PRINTK("[set_tx_power] invalid index %d (0..4)", idx);
		return;
	}

	if (rltk_set_tx_power_percentage((rtw_tx_pwr_percentage_t)idx) != 0) {
		AT_PRINTK("[set_tx_power] set index %d failed", idx);
	} else {
		AT_PRINTK("[set_tx_power] set index %d ok", idx);
	}
}

#if CARBOX_RECOVERY
/* MP 自动检测：依次下发整套 iwpriv mp_* 命令序列（走 SIOCDEVPRIVATE 私有 ioctl）。 */
void fMPTEST(void *arg)
{
	static const char *const mp_test_cmds[] = {
		"mp_start",
		"mp_ant_tx a",
		"mp_channel 36",
		"mp_bandwidth 40M=0,shortGI=0",
		"mp_rate 108",
		"mp_txpower patha=46,pathb=46",
		"mp_ctx background,pkt",
		"mp_phypara xcap=92",
		"mp_phypara xcap=93",
		"mp_ctx stop",
		"mp_channel 38",
		"mp_bandwidth 40M=1,shortGI=0",
		"mp_rate 135",
		"mp_txpower patha=44,pathb=44",
		"mp_ctx background,pkt",
		"mp_txpower patha=44,pathb=44",
		"mp_txpower patha=46,pathb=46",
		"mp_ctx stop",
		"mp_channel 46",
		"mp_txpower patha=44,pathb=44",
		"mp_ctx background,pkt",
		"mp_txpower patha=44,pathb=44",
		"mp_txpower patha=45,pathb=45",
		"mp_ctx stop",
		"mp_channel 54",
		"mp_txpower patha=42,pathb=42",
		"mp_ctx background,pkt",
		"mp_txpower patha=42,pathb=42",
		"mp_txpower patha=44,pathb=44",
		"mp_ctx stop",
		"mp_channel 62",
		"mp_txpower patha=41,pathb=41",
		"mp_ctx background,pkt",
		"mp_txpower patha=41,pathb=41",
		"mp_ctx stop",
		"mp_channel 102",
		"mp_txpower patha=35,pathb=35",
		"mp_ctx background,pkt",
		"mp_txpower patha=35,pathb=35",
		"mp_ctx stop",
		"mp_channel 110",
		"mp_txpower patha=35,pathb=35",
		"mp_ctx background,pkt",
		"mp_txpower patha=35,pathb=35",
		"mp_ctx stop",
		"mp_channel 118",
		"mp_txpower patha=34,pathb=34",
		"mp_ctx background,pkt",
		"mp_txpower patha=34,pathb=34",
		"mp_ctx stop",
		"mp_channel 126",
		"mp_txpower patha=36,pathb=36",
		"mp_ctx background,pkt",
		"mp_txpower patha=36,pathb=36",
		"mp_ctx stop",
		"mp_channel 134",
		"mp_txpower patha=34,pathb=34",
		"mp_ctx background,pkt",
		"mp_txpower patha=34,pathb=34",
		"mp_ctx stop",
		"mp_channel 142",
		"mp_txpower patha=30,pathb=30",
		"mp_ctx background,pkt",
		"mp_txpower patha=30,pathb=30",
		"mp_ctx stop",
		"mp_channel 151",
		"mp_txpower patha=35,pathb=35",
		"mp_ctx background,pkt",
		"mp_txpower patha=35,pathb=35",
		"mp_ctx stop",
		"mp_channel 159",
		"mp_txpower patha=35,pathb=35",
		"mp_ctx background,pkt",
		"mp_txpower patha=35,pathb=35",
		"mp_ctx stop",
		"mp_channel 167",
		"mp_txpower patha=35,pathb=35",
		"mp_ctx background,pkt",
		"mp_txpower patha=35,pathb=35",
		"mp_ctx stop",
		"mp_channel 175",
		"mp_txpower patha=37,pathb=37",
		"mp_ctx background,pkt",
		"mp_txpower patha=37,pathb=37",
		"mp_txpower patha=36,pathb=36",
		"mp_ctx stop",
		"mp_channel 36",
		"mp_bandwidth 40M=0,shortGI=0",
		"mp_rate 108",
		"mp_txpower patha=48,pathb=48",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 52",
		"mp_txpower patha=46,pathb=46",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 64",
		"mp_txpower patha=43,pathb=43",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 100",
		"mp_rate 135",
		"mp_txpower patha=35,pathb=35",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 120",
		"mp_txpower patha=34,pathb=34",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 140",
		"mp_txpower patha=30,pathb=30",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 140",
		"mp_txpower patha=30,pathb=30",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 140",
		"mp_txpower patha=30,pathb=30",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 151",
		"mp_bandwidth 40M=1,shortGI=0",
		"mp_txpower patha=35,pathb=35",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 159",
		"mp_txpower patha=35,pathb=35",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 36",
		"mp_bandwidth 40M=0,shortGI=0",
		"mp_rate 168",
		"mp_txpower patha=46,pathb=46",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 177",
		"mp_txpower patha=36,pathb=36",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 38",
		"mp_bandwidth 40M=1,shortGI=0",
		"mp_rate 169",
		"mp_txpower patha=46,pathb=46",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 175",
		"mp_txpower patha=36,pathb=36",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 175",
		"mp_txpower patha=36,pathb=36",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 155",
		"mp_bandwidth 40M=2,shortGI=0",
		"mp_rate 167",
		"mp_txpower patha=33,pathb=33",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 155",
		"mp_txpower patha=33,pathb=33",
		"mp_ctx background,pkt",
		"mp_ctx stop",
		"mp_channel 155",
		"mp_txpower patha=33,pathb=33",
		"mp_ctx background,pkt",
		"mp_ctx stop",
	};
	const int cmd_count = (int)(sizeof(mp_test_cmds) / sizeof(mp_test_cmds[0]));
	char cmd_buf[128];
	int i;

	(void)arg;
	if (!recovery_mp_is_active()) { AT_PRINTK("[MPTEST] send MP=4 and wait READY first"); return; }
	AT_PRINTK("[MPTEST] start, total=%d", cmd_count);
	for (i = 0; i < cmd_count; i++) {
		AT_PRINTK("[MPTEST] (%d/%d) %s", i + 1, cmd_count, mp_test_cmds[i]);
		snprintf(cmd_buf, sizeof(cmd_buf), "%s", mp_test_cmds[i]);
		if (mp_calibration_command(cmd_buf, 1) < 0) { AT_PRINTK("[MPTEST] failed at %d", i + 1); return; }
	}
	AT_PRINTK("[MPTEST] done");
}

/* MP 产测命令（参考 carbox at_mp）
 * MP=0: 重启
 * MP=1: USB host MP 加载（外接 USB 设备检测）
 * MP=2: USB host MP 卸载
 * MP=3: I2C 读数据测试（设备 7 位地址 0x11，即 8 位写地址 0x22，寄存器 0x00）
 */
void at_mp(void *arg)
{
	if (!arg) {
		AT_PRINTK("[AT+MP] Usage: MP=STATUS|0|1|2|3|4\r\n  0: reboot\r\n  1: USB MP load\r\n  2: USB MP unload\r\n  3: I2C read test\r\n  4: reboot recovery into MP (wait READY)\r\n");
		return;
	}

    /* STATUS是只读查询，不作为模式切换口令。只接受完整token和行尾。 */
    if (strncmp((const char *)arg, "STATUS", 6) == 0 &&
        strspn((const char *)arg + 6, "\r\n") == strlen((const char *)arg + 6)) {
        recovery_mp_print_status();
        return;
    }
    /* 功能号只允许0..4及行尾，拒绝负号、多位数字和CR后的额外命令。 */
    const char *token = (const char *)arg;
    if (token[0] < '0' || token[0] > '4' ||
        strspn(token + 1, "\r\n") != strlen(token + 1)) {
        AT_PRINTK("[MP] invalid argument; use STATUS or 0..4"); return;
    }
    unsigned int parm = (unsigned int)(token[0] - '0');
    if (parm == 4) {
        int ret = recovery_mp_request();
        AT_PRINTK("[REC_MP] request=%d", ret); return;
    }

	if (parm == 0) {
		AT_PRINTK("MP=0: reboot\r\n");
		sys_reset();
	} else if (parm == 1) {
		AT_PRINTK("MP=1: USB MP load\r\n");
		usbh_mp_init();
	} else if (parm == 2) {
		AT_PRINTK("MP=2: USB MP unload\r\n");
		usbh_mp_deinit();
	} else if (parm == 3) {
		CI2C *i2c;
		unsigned char reg = 0x00;
		unsigned char val = 0;
		char device[] = "/dev/twi2";

		AT_PRINTK("MP=3: I2C read test\r\n");
		i2c = CI2C_new();
		if (i2c == NULL) {
			AT_PRINTK("Get data Fail: CI2C_new fail\r\n");
			return;
		}
		if (CI2C_Open(i2c, device)) {
			/* CI2C_Read 的 addr 是 8 位写地址（内部 >>1），
			 * 参考 i2c_read_data(17, 0x00) 的 17 是 7 位地址，
			 * 故此处传 0x22 = 0x11 << 1。 */
			if (CI2C_Read(i2c, 0x22, &reg, 1, &val, 1) && val != 0xFF) {
				AT_PRINTK("Get data Pass: 0x%x\r\n", val);
			} else {
				AT_PRINTK("Get data Fail: 0x%x\r\n", val);
			}
			CI2C_Close(i2c);
		} else {
			AT_PRINTK("Get data Fail: i2c open fail\r\n");
		}
		CI2C_delete(i2c);
	}
}
#endif

#if CONFIG_UART_YMODEM
void fATSY(void *arg)
{
	uart_ymodem();
}
#endif

#if defined(CONFIG_PLATFORM_8711B)
void fATSK(void *arg)
{
	int argc = 0;
	char *argv[MAX_ARGC] = {0};
	u8 key[16];

	AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK]: _AT_SYSTEM_RDP/RSIP_CONFIGURE_");
	if(!arg){
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=RDP_EN");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=RDP_KEY[value(hex)]");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] 	Example: ATSK=RDP_KEY[345487bbaa435bfe382233445ba359aa]");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=RSIP_EN");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=RSIP_DIS");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=RSIP_KEY[value(hex)]");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=SB_EN");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=SB_PK_MD5[value(hex)]");
		return;
	}

	argc = parse_param(arg, argv);
	if(strcmp(argv[1], "RDP_EN") == 0){
		EFUSE_RDP_EN();
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] RDP is enable");
	}else if(strcmp(argv[1], "RDP_KEY") == 0){
		if(argc != 3){
			AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=RDP_KEY[value(hex)]");
			return;
		}

		if(strlen(argv[2]) != 32){
			AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Err: RDP key length should be 16 bytes");
			return;
		}

		sscanf(argv[2], "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
			&key[0], &key[1], &key[2], &key[3], &key[4], &key[5], &key[6], &key[7],
			&key[8], &key[9], &key[10], &key[11], &key[12], &key[13], &key[14], &key[15]);

		EFUSE_RDP_KEY(key);
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Set RDP key done");
	}else if(strcmp(argv[1], "RSIP_EN") == 0){
		efuse_otf_cmd(ENABLE);
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] RSIP is enable");
	}else if(strcmp(argv[1], "RSIP_DIS") == 0){
		efuse_otf_cmd(DISABLE);
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] RSIP is disable");
	}else if(strcmp(argv[1], "RSIP_KEY") == 0){
		if(argc != 3){
			AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=RSIP_KEY[value(hex)]");
			return;
		}

		if(strlen(argv[2]) != 32){
			AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Err: RSIP key length should be 16 bytes");
			return;
		}

		sscanf(argv[2], "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
			&key[0], &key[1], &key[2], &key[3], &key[4], &key[5], &key[6], &key[7],
			&key[8], &key[9], &key[10], &key[11], &key[12], &key[13], &key[14], &key[15]);

		EFUSE_OTF_KEY(key);
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Set RSIP key done");
	}else if(strcmp(argv[1], "SB_EN") == 0){
		u8 data = 0;
		u32 efuse_ctrl = HAL_READ32(SYSTEM_CTRL_BASE, REG_SYS_EFUSE_CTRL);

		EFUSERead8(efuse_ctrl, 0xD3, &data, L25EOUTVOLTAGE);
		if ((data & EFUSE_PHYSICAL_SBOOT_ON) != 0) {
			EFUSEWrite8(efuse_ctrl, 0xD3, data & (~EFUSE_PHYSICAL_SBOOT_ON), L25EOUTVOLTAGE);
			AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] : Security Boot is enable!");
		}else{
			AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] : Security Boot is already enabled!");
		}
	}else if(strcmp(argv[1], "SB_PK_MD5") == 0){
		u8 i = 0;

		if(argc != 3){
			AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=SB_PK_MD5[value(hex)]");
			return;
		}

		if(strlen(argv[2]) != 32){
			AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Err: MD5 value of public key should be 16 bytes");
			return;
		}

		sscanf(argv[2], "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
			&key[0], &key[1], &key[2], &key[3], &key[4], &key[5], &key[6], &key[7],
			&key[8], &key[9], &key[10], &key[11], &key[12], &key[13], &key[14], &key[15]);

		for(i = 0; i < 16; i++) {
			EFUSEWrite8(HAL_READ32(SYSTEM_CTRL_BASE, REG_SYS_EFUSE_CTRL), 0xC1 + i, key[i], L25EOUTVOLTAGE);
		}
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Set SB md5 value of public key done");

	}else{
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=RDP_EN");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=RDP_KEY[value(hex)]");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] 	Example: ATSK=RDP_KEY[345487bbaa435bfe382233445ba359aa]");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=RSIP_EN");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=RSIP_DIS");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=RSIP_KEY[value(hex)]");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=SB_EN");
		AT_DBG_MSG(AT_FLAG_RDP, AT_DBG_ALWAYS, "[ATSK] Usage: ATSK=SB_PK_MD5[value(hex)]");

	}

}
#endif

#if SUPPORT_MP_MODE
void fATSA(void *arg)
{
#if !defined(CONFIG_PLATFORM_8195BHP) && !defined(CONFIG_PLATFORM_8710C) && !defined(CONFIG_PLATFORM_8721D)
	u32 tConfigDebugInfo = ConfigDebugInfo;
	int argc = 0, channel;
	char *argv[MAX_ARGC] = {0}, *ptmp;
	u16 offset, gain;

	AT_DBG_MSG(AT_FLAG_ADC, AT_DBG_ALWAYS, "[ATSA]: _AT_SYSTEM_ADC_TEST_");
	if(!arg){
		AT_DBG_MSG(AT_FLAG_ADC, AT_DBG_ALWAYS, "[ATSA] Usage: ATSA=CHANNEL(1~3)");
		AT_DBG_MSG(AT_FLAG_ADC, AT_DBG_ALWAYS, "[ATSA] Usage: ATSA=k_get");
		AT_DBG_MSG(AT_FLAG_ADC, AT_DBG_ALWAYS, "[ATSA] Usage: ATSA=k_set[offet(hex),gain(hex)]");
		return;
	}

	argc = parse_param(arg, argv);
	if(strcmp(argv[1], "k_get") == 0){
		sys_adc_calibration(0, &offset, &gain);
//		AT_DBG_MSG(AT_FLAG_ADC, AT_DBG_ALWAYS, "[ATSA] offset = 0x%04X, gain = 0x%04X", offset, gain);
	}else if(strcmp(argv[1], "k_set") == 0){
		if(argc != 4){
			AT_DBG_MSG(AT_FLAG_ADC, AT_DBG_ALWAYS, "[ATSA] Usage: ATSA=k_set[offet(hex),gain(hex)]");
			return;
		}
		offset = strtoul(argv[2], &ptmp, 16);
		gain = strtoul(argv[3], &ptmp, 16);
		sys_adc_calibration(1, &offset, &gain);
//		AT_DBG_MSG(AT_FLAG_ADC, AT_DBG_ALWAYS, "[ATSA] offset = 0x%04X, gain = 0x%04X", offset, gain);
	}else{
		channel = atoi(argv[1]);
		if(channel < 1 || channel > 3){
			AT_DBG_MSG(AT_FLAG_ADC, AT_DBG_ALWAYS, "[ATSA] Usage: ATSA=CHANNEL(1~3)");
			return;
		}
		analogin_t   adc;
		u16 adcdat;

		// Remove debug info massage
		ConfigDebugInfo = 0;
		if(channel == 1)
			analogin_init(&adc, AD_1);
		else if(channel == 2)
			analogin_init(&adc, AD_2);
		else
			analogin_init(&adc, AD_3);
		adcdat = analogin_read_u16(&adc)>>4;
		analogin_deinit(&adc);
		// Recover debug info massage
		ConfigDebugInfo = tConfigDebugInfo;

		AT_DBG_MSG(AT_FLAG_ADC, AT_DBG_ALWAYS, "[ATSA] A%d = 0x%04X", channel, adcdat);
	}
#elif defined(CONFIG_PLATFORM_8721D)
	int argc = 0, channel;
	char *argv[MAX_ARGC] = {0};
	analogin_t   adc;
	u16 adcdat;
	u8 max_ch_num =   8;
	u32 ch_list[8] = {AD_0, AD_1, AD_2, AD_3, AD_4, AD_5, AD_6, AD_7};

	AT_DBG_MSG(AT_FLAG_ADC, AT_DBG_ALWAYS, "[ATSA]: _AT_SYSTEM_ADC_TEST_");
	if(!arg){
		AT_DBG_MSG(AT_FLAG_ADC, AT_DBG_ALWAYS, "[ATSA] Usage: ATSA=CHANNEL(0~7)");
		return;
	}

	argc = parse_param(arg, argv);

	channel = atoi(argv[1]);
	if(channel >= max_ch_num){
		AT_DBG_MSG(AT_FLAG_ADC, AT_DBG_ALWAYS, "[ATSA] Usage: ATSA=CHANNEL(0~7)");
		return;
	}

	analogin_init(&adc, ch_list[channel]);
	adcdat = analogin_read_u16(&adc);
	analogin_deinit(&adc);

	AT_DBG_MSG(AT_FLAG_ADC, AT_DBG_ALWAYS, "[ATSA] A%d = 0x%04X", channel, adcdat);
#endif
}

void fATSG(void *arg)
{
#if defined(CONFIG_PLATFORM_8195A)
    gpio_t gpio_test;
    int argc = 0, val;
	char *argv[MAX_ARGC] = {0}, port, num;
	PinName pin = NC;
	u32 tConfigDebugInfo = ConfigDebugInfo;

	AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG]: _AT_SYSTEM_GPIO_TEST_");
	if(!arg){
		AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG] Usage: ATSG=PINNAME(ex:A0)");
		return;
	}else{
		argc = parse_param(arg, argv);
		if(argc != 2){
			AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG] Usage: ATSG=PINNAME(ex:A0)");
			return;
		}
	}
	port = argv[1][0];
	num = argv[1][1];
	if(port >= 'a' && port <= 'z')
		port -= ('a' - 'A');
	if(num >= 'a' && num <= 'z')
		num -= ('a' - 'A');
	switch(port){
		case 'A':
			switch(num){
				case '0': pin = PA_0; break; case '1': pin = PA_1; break; case '2': pin = PA_2; break; case '3': pin = PA_3; break;
				case '4': pin = PA_4; break; case '5': pin = PA_5; break; case '6': pin = PA_6; break; case '7': pin = PA_7; break;
			}
			break;
		case 'B':
			switch(num){
				case '0': pin = PB_0; break; case '1': pin = PB_1; break; case '2': pin = PB_2; break; case '3': pin = PB_3; break;
				case '4': pin = PB_4; break; case '5': pin = PB_5; break; case '6': pin = PB_6; break; case '7': pin = PB_7; break;
			}
			break;
		case 'C':
			switch(num){
				case '0': pin = PC_0; break; case '1': pin = PC_1; break; case '2': pin = PC_2; break; case '3': pin = PC_3; break;
				case '4': pin = PC_4; break; case '5': pin = PC_5; break; case '6': pin = PC_6; break; case '7': pin = PC_7; break;
				case '8': pin = PC_8; break; case '9': pin = PC_9; break;
			}
			break;
		case 'D':
			switch(num){
				case '0': pin = PD_0; break; case '1': pin = PD_1; break; case '2': pin = PD_2; break; case '3': pin = PD_3; break;
				case '4': pin = PD_4; break; case '5': pin = PD_5; break; case '6': pin = PD_6; break; case '7': pin = PD_7; break;
				case '8': pin = PD_8; break; case '9': pin = PD_9; break;
			}
			break;
		case 'E':
			switch(num){
				case '0': pin = PE_0; break; case '1': pin = PE_1; break; case '2': pin = PE_2; break; case '3': pin = PE_3; break;
				case '4': pin = PE_4; break; case '5': pin = PE_5; break; case '6': pin = PE_6; break; case '7': pin = PE_7; break;
				case '8': pin = PE_8; break; case '9': pin = PE_9; break; case 'A': pin = PE_A; break;
			}
			break;
		case 'F':
			switch(num){
				case '0': pin = PF_0; break; case '1': pin = PF_1; break; case '2': pin = PF_2; break; case '3': pin = PF_3; break;
				case '4': pin = PF_4; break; case '5': pin = PF_5; break;
			}
			break;
		case 'G':
			switch(num){
				case '0': pin = PG_0; break; case '1': pin = PG_1; break; case '2': pin = PG_2; break; case '3': pin = PG_3; break;
				case '4': pin = PG_4; break; case '5': pin = PG_5; break; case '6': pin = PG_6; break; case '7': pin = PG_7; break;
			}
			break;
		case 'H':
			switch(num){
				case '0': pin = PH_0; break; case '1': pin = PH_1; break; case '2': pin = PH_2; break; case '3': pin = PH_3; break;
				case '4': pin = PH_4; break; case '5': pin = PH_5; break; case '6': pin = PH_6; break; case '7': pin = PH_7; break;
			}
			break;
		case 'I':
			switch(num){
				case '0': pin = PI_0; break; case '1': pin = PI_1; break; case '2': pin = PI_2; break; case '3': pin = PI_3; break;
				case '4': pin = PI_4; break; case '5': pin = PI_5; break; case '6': pin = PI_6; break; case '7': pin = PI_7; break;
			}
			break;
		case 'J':
			switch(num){
				case '0': pin = PJ_0; break; case '1': pin = PJ_1; break; case '2': pin = PJ_2; break; case '3': pin = PJ_3; break;
				case '4': pin = PJ_4; break; case '5': pin = PJ_5; break; case '6': pin = PJ_6; break;
			}
			break;
		case 'K':
			switch(num){
				case '0': pin = PK_0; break; case '1': pin = PK_1; break; case '2': pin = PK_2; break; case '3': pin = PK_3; break;
				case '4': pin = PK_4; break; case '5': pin = PK_5; break; case '6': pin = PK_6; break;
			}
			break;
	}
	if(pin == NC){
		AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG]: Invalid Pin Name");
		return;
	}
	// Remove debug info massage
	ConfigDebugInfo = 0;
	// Initial input control pin
	gpio_init(&gpio_test, pin);
	gpio_dir(&gpio_test, PIN_INPUT);     // Direction: Input
	gpio_mode(&gpio_test, PullUp);       // Pull-High
	val = gpio_read(&gpio_test);
	// Recover debug info massage
	ConfigDebugInfo = tConfigDebugInfo;
	AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG] %c%c = %d", port, num, val);

#elif defined(CONFIG_PLATFORM_8711B)
    gpio_t gpio_test;
    int argc = 0, val, num;
	char *argv[MAX_ARGC] = {0}, port;
	PinName pin = NC;
	u32 tConfigDebugInfo = ConfigDebugInfo;

	AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG]: _AT_SYSTEM_GPIO_TEST_");
	if(!arg){
		AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG] Usage: ATSG=PINNAME(ex:A0)");
		return;
	}else{
		argc = parse_param(arg, argv);
		if(argc != 2){
			AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG] Usage: ATSG=PINNAME(ex:A0)");
			return;
		}
	}
	port = argv[1][0];
	if(port >= 'a' && port <= 'z')
		port -= ('a' - 'A');
	num = atoi(argv[1] + 1);

	//PA_6~PA_11 are not allowed to be tested when code running on flash.
	//PA_16~PA_17 or PA_29~PA_30 should not be tested when they are used as log UART RX and TX.
	switch(port){
		case 'A':
			switch(num){
				case 0: pin = PA_0; break; case 1: pin = PA_1; break; case 2: pin = PA_2; break; case 3: pin = PA_3; break;
				case 4: pin = PA_4; break; case 5: pin = PA_5; break;  /*case 6:pin = PA_6; break; case 7: pin = PA_7; break;
				case 8: pin = PA_8; break; case 9: pin = PA_9; break; case 10: pin = PA_10; break; case 11: pin = PA_11; break;*/
				case 12: pin = PA_12; break; case 13: pin = PA_13; break; case 14: pin = PA_14; break; case 15: pin = PA_15; break;
				case 16: pin = PA_16; break; case 17: pin = PA_17; break; case 18: pin = PA_18; break; case 19: pin = PA_19; break;
				case 20: pin = PA_20; break; case 21: pin = PA_21; break; case 22: pin = PA_22; break; case 23: pin = PA_23; break;
				case 24: pin = PA_24; break; case 25: pin = PA_25; break; case 26: pin = PA_26; break; case 27: pin = PA_27; break;
				case 28: pin = PA_28; break; case 29: pin = PA_29; break; case 30: pin = PA_30; break; case 31: pin = PA_31; break;
			}
			break;
		case 'B':
			switch(num){
				case 0: pin = PB_0; break; case 1: pin = PB_1; break; case 2: pin = PB_2; break; case 3: pin = PB_3; break;
				case 4: pin = PB_4; break; case 5: pin = PB_5; break; case 6: pin = PB_6; break; case 7: pin = PB_7; break;
			}
			break;
	}
	if(pin == NC){
		AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG]: Invalid Pin Name");
		return;
	}
	// Remove debug info massage
	ConfigDebugInfo = 0;
	// Initial input control pin
	gpio_init(&gpio_test, pin);
	gpio_dir(&gpio_test, PIN_INPUT);     // Direction: Input
	gpio_mode(&gpio_test, PullUp);       // Pull-High
	val = gpio_read(&gpio_test);
	// Recover debug info massage
	ConfigDebugInfo = tConfigDebugInfo;
	AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG] %c%d = %d", port, num, val);

#elif defined(CONFIG_PLATFORM_8721D)
    gpio_t gpio_test;
    int argc = 0, val, num;
	char *argv[MAX_ARGC] = {0}, port;
	PinName pin = NC;

	AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG]: _AT_SYSTEM_GPIO_TEST_");
	if(!arg){
		AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG] Usage: ATSG=PINNAME(ex:A0)");
		return;
	}else{
		argc = parse_param(arg, argv);
		if(argc != 2){
			AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG] Usage: ATSG=PINNAME(ex:A0)");
			return;
		}
	}
	port = argv[1][0];
	if(port >= 'a' && port <= 'z')
		port -= ('a' - 'A');
	num = atoi(argv[1] + 1);

	//PB_12~PB_17 or PB_18~PB_23 are not allowed to be tested when code running on flash.
	//PA_7~PA_8 should not be tested when they are used as log UART RX and TX.
	switch(port){
		case 'A':
			switch(num){
				case 0: pin = PA_0; break; case 1: pin = PA_1; break; case 2: pin = PA_2; break; case 3: pin = PA_3; break;
				case 4: pin = PA_4; break; case 5: pin = PA_5; break;  case 6:pin = PA_6; break; case 7: pin = PA_7; break;
				case 8: pin = PA_8; break; case 9: pin = PA_9; break; case 10: pin = PA_10; break; case 11: pin = PA_11; break;
				case 12: pin = PA_12; break; case 13: pin = PA_13; break; case 14: pin = PA_14; break; case 15: pin = PA_15; break;
				case 16: pin = PA_16; break; case 17: pin = PA_17; break; case 18: pin = PA_18; break; case 19: pin = PA_19; break;
				case 20: pin = PA_20; break; case 21: pin = PA_21; break; case 22: pin = PA_22; break; case 23: pin = PA_23; break;
				case 24: pin = PA_24; break; case 25: pin = PA_25; break; case 26: pin = PA_26; break; case 27: pin = PA_27; break;
				case 28: pin = PA_28; break; case 29: pin = PA_29; break; case 30: pin = PA_30; break; case 31: pin = PA_31; break;
			}
			break;
		case 'B':
			switch(num){
				case 0: pin = PB_0; break; case 1: pin = PB_1; break; case 2: pin = PB_2; break; case 3: pin = PB_3; break;
				case 4: pin = PB_4; break; case 5: pin = PB_5; break; case 6: pin = PB_6; break; case 7: pin = PB_7; break;
				case 8: pin = PB_8; break; case 9: pin = PA_9; break; case 10: pin = PA_10; break; case 11: pin = PA_11; break;
				/*case 12: pin = PB_12; break; case 13: pin = PB_13; break; case 14: pin = PB_14; break; case 15: pin = PB_15; break;
				case 16: pin = PB_16; break; case 17: pin = PB_17; break; case 18: pin = PB_18; break; case 19: pin = PB_19; break;*/
				case 20: pin = PB_20; break; case 21: pin = PB_21; break; case 22: pin = PB_22; break; case 23: pin = PB_23; break;
				case 24: pin = PB_24; break; case 25: pin = PB_25; break; case 26: pin = PB_26; break; case 27: pin = PB_27; break;
				case 28: pin = PB_28; break; case 29: pin = PB_29; break; case 30: pin = PB_30; break; case 31: pin = PB_31; break;
			}
			break;
	}
	if(pin == NC){
		AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG]: Invalid Pin Name");
		return;
	}

	// Initial input control pin
	gpio_init(&gpio_test, pin);
	gpio_dir(&gpio_test, PIN_INPUT);     // Direction: Input
	gpio_mode(&gpio_test, PullUp);       // Pull-High
	val = gpio_read(&gpio_test);

	AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG] %c%d = %d", port, num, val);

#endif
}

void fATSP(void *arg)
{
#if defined(CONFIG_PLATFORM_8195A)
	int   argc           = 0;
	char *argv[MAX_ARGC] = {0};

	unsigned long timeout; // ms
	unsigned long time_begin, time_current;

	gpio_t gpiob_1;
	int val_old, val_new;

	int expected_zerocount, zerocount;
	int test_result;

	// parameter check
	AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSP]: _AT_SYSTEM_POWER_PIN_TEST_");
	if(!arg) {
		AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSP]: Usage: ATSP=gpiob1[timeout,zerocount]");
		return;
	} else {
		argc = parse_param(arg, argv);
		if (argc < 2) {
			AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSP]: Usage: ATSP=gpiob1[timeout,zerocount]");
			return;
		}
	}

	if ( strcmp(argv[1], "gpiob1" ) == 0 ) {
		if (argc < 4) {
			AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSP]: Usage: ATSP=gpiob1[timeout,zerocount]");
			return;
		}

		// init gpiob1 test
		test_result         = 0;
		timeout             = strtoul(argv[2], NULL, 10);
		expected_zerocount  = atoi(argv[3]);
		zerocount           = 0;
		val_old             = 1;

		sys_log_uart_off();

		gpio_init(&gpiob_1, PB_1);
		gpio_dir(&gpiob_1, PIN_INPUT);
		gpio_mode(&gpiob_1, PullDown);

		// gpiob1 test ++
		time_begin = time_current = xTaskGetTickCount();
		while (time_current < time_begin + timeout) {
			val_new = gpio_read(&gpiob_1);

			if (val_new != val_old && val_new == 0) {

				zerocount ++;
				if (zerocount == expected_zerocount) {
					test_result = 1;
					break;
				}
			}

			val_old = val_new;
			time_current = xTaskGetTickCount();
		}
		// gpio test --

		sys_log_uart_on();

		if (test_result == 1) {
			AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSP]: success");
		} else {
			AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSP]: fail, it only got %d zeros", zerocount);
		}
	}
#endif
}

#if !defined(CONFIG_PLATFORM_8195BHP) && !defined(CONFIG_PLATFORM_8710C)
int write_otu_to_system_data(flash_t *flash, uint32_t otu_addr)
{
	uint32_t data, i = 0;
	flash_read_word(flash, FLASH_SYSTEM_DATA_ADDR+0xc, &data);
	//printf("\n\r[%s] data 0x%x otu_addr 0x%x", __FUNCTION__, data, otu_addr);
	AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: data 0x%x otu_addr 0x%x", data, otu_addr);
	if(data == ~0x0){
		flash_write_word(flash, FLASH_SYSTEM_DATA_ADDR+0xc, otu_addr);
	}else{
		//erase backup sector
		flash_erase_sector(flash, FLASH_RESERVED_DATA_BASE);
		//backup system data to backup sector
		for(i = 0; i < 0x1000; i+= 4){
			flash_read_word(flash, FLASH_SYSTEM_DATA_ADDR + i, &data);
			if(i == 0xc)
				data = otu_addr;
			flash_write_word(flash, FLASH_RESERVED_DATA_BASE + i,data);
		}
		//erase system data
		flash_erase_sector(flash, FLASH_SYSTEM_DATA_ADDR);
		//write data back to system data
		for(i = 0; i < 0x1000; i+= 4){
			flash_read_word(flash, FLASH_RESERVED_DATA_BASE + i, &data);
			flash_write_word(flash, FLASH_SYSTEM_DATA_ADDR + i,data);
		}
		//erase backup sector
		flash_erase_sector(flash, FLASH_RESERVED_DATA_BASE);
	}
	return 0;
}
#endif

void fATSB(void *arg)
{
#if !defined(CONFIG_PLATFORM_8195BHP) && !defined(CONFIG_PLATFORM_8710C)
	int   argc           = 0;
	char *argv[MAX_ARGC] = {0};
	u32 boot_gpio, rb_boot_gpio;
	u8 gpio_pin;
	u8 uart_port, uart_index;
	u8 gpio_pin_bar;
	u8 uart_port_bar;
	flash_t flash;

	// parameter check
	AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: _AT_SYSTEM_BOOT_OTU_PIN_SET_");
	if(!arg) {
		AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: Usage: ATSB=[GPIO_PIN,TRIGER_MODE,UART]");
		AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: GPIO_PIN: PB_1, PC_4 ....");
		AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: TRIGER_MODE: low_trigger, high_trigger");
		AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: UART: UART0, UART2");
		AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: example: ATSB=[PC_2,low_trigger,UART2]");
		return;
	} else {
		argc = parse_param(arg, argv);
		if (argc != 4 ) {
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: Usage: ATSB=[GPIO_PIN,TRIGER_MODE,UART]");
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: GPIO_PIN: PB_1, PC_4 ....");
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: TRIGER_MODE: low_trigger, high_trigger");
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: UART: UART0, UART2");
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: example: ATSB=[PC_2,low_trigger,UART2]");
			return;
		}
	}

	if ( strncmp(argv[1], "P", 1) == 0 && strlen(argv[1]) == 4
		&& (strcmp(argv[2], "low_trigger") == 0 || strcmp(argv[2], "high_trigger") == 0)
		&& strncmp(argv[3], "UART", 4) == 0 && strlen(argv[3]) == 5) {
		if((argv[1][1] >= 0x41 && argv[1][1] <= 0x45) && (argv[1][3] >= 0x30 && argv[1][3] <= 0x39) &&(argv[3][4] >= 0x30 && argv[3][4] <= 0x32)){
			if(strcmp(argv[2], "high_trigger") == 0)
				gpio_pin = 1<< 7 | ((argv[1][1]-0x41)<<4) | (argv[1][3] - 0x30);
			else
				gpio_pin = ((argv[1][1]-0x41)<<4) | (argv[1][3] - 0x30);
			gpio_pin_bar = ~gpio_pin;
			uart_index = argv[3][4] - 0x30;
			if(uart_index == 0)
				uart_port = (uart_index<<4)|2;
			else if(uart_index == 2)
				uart_port = (uart_index<<4)|0;
			else{
				AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: Input UART index error. Please choose UART0 or UART2.");
				AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: example: ATSB=[PC_2,low_trigger,UART2]");
				return;
			}
			uart_port_bar = ~uart_port;
			boot_gpio = uart_port_bar<<24 | uart_port<<16 | gpio_pin_bar<<8 | gpio_pin;
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]:gpio_pin 0x%x", gpio_pin);
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]:gpio_pin_bar 0x%x", gpio_pin_bar);
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]:uart_port 0x%x", uart_port);
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]:uart_port_bar 0x%x", uart_port_bar);
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]:boot_gpio 0x%x", boot_gpio);
			write_otu_to_system_data(&flash, boot_gpio);
			flash_read_word(&flash, FLASH_SYSTEM_DATA_ADDR+0x0c, &rb_boot_gpio);
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]:Read 0x900c 0x%x", rb_boot_gpio);
		}else{
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: Usage: ATSB=[GPIO_PIN,TRIGER_MODE,UART]");
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: GPIO_PIN: PB_1, PC_4 ....");
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: TRIGER_MODE: low_trigger, high_trigger");
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: UART: UART0, UART2");
			AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: example: ATSB=[PC_2,low_trigger,UART2]");
		}
	}else{
		AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: Usage: ATSB=[GPIO_PIN,TRIGER_MODE,UART]");
		AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: GPIO_PIN: PB_1, PC_4 ....");
		AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: TRIGER_MODE: low_trigger, high_trigger");
		AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: UART: UART0, UART2");
		AT_DBG_MSG(AT_FLAG_DUMP, AT_DBG_ALWAYS, "[ATSB]: example: ATSB=[PC_2,low_trigger,UART2]");
		return;
	}
#endif
}
#endif

#if (configGENERATE_RUN_TIME_STATS == 1)
void fATSS(void *arg)	// Show CPU stats
{
	int argc;
	int status;
	char *argv[MAX_ARGC] = {0};
	BaseType_t stop_monitor = pdFALSE;
	BaseType_t monitor_was_running;

	if (arg != NULL) {
		argc = parse_param(arg, argv);
		if (argc != 2) {
			AT_PRINTK("[ATSS]: Usage: ATSS, ATSS=start, or ATSS=stop");
			return;
		}

		if ((strcmp(argv[1], "stop") == 0) || (strcmp(argv[1], "0") == 0)) {
			stop_monitor = pdTRUE;
		} else if ((strcmp(argv[1], "start") != 0) && (strcmp(argv[1], "1") != 0)) {
			AT_PRINTK("[ATSS]: Usage: ATSS, ATSS=start, or ATSS=stop");
			return;
		}
	}

	if (stop_monitor != pdFALSE) {
		if (atss_cli_print_enabled == pdFALSE) {
			AT_PRINTK("[ATSS]: CPU monitor is not running");
		} else {
			status = atss_set_enabled(pdTRUE, pdFALSE);
			if (status == ATSS_OK) {
				if (atss_internal_enabled != pdFALSE) {
					AT_PRINTK("[ATSS]: CPU monitor printing stopped; internal statistics remain active");
				} else {
					AT_PRINTK("[ATSS]: CPU monitor stopped");
				}
			} else {
				AT_PRINTK("[ATSS]: ERROR: unable to stop CPU monitor (%d)",
					 status);
			}
		}
		return;
	}

	if (atss_cli_print_enabled != pdFALSE) {
		AT_PRINTK("[ATSS]: CPU monitor is already running");
		return;
	}

	monitor_was_running = (atss_monitor_task != NULL);
	status = atss_set_enabled(pdTRUE, pdTRUE);
	if (status != ATSS_OK) {
		AT_PRINTK("[ATSS]: ERROR: unable to start CPU monitor (%d)", status);
		return;
	}

	if (monitor_was_running != pdFALSE) {
		AT_PRINTK("[ATSS]: CPU monitor printing enabled; reporting every %u ms",
			 (unsigned int)ATSS_SAMPLE_PERIOD_MS);
	} else {
		AT_PRINTK("[ATSS]: CPU monitor started; reporting every %u ms",
			 (unsigned int)ATSS_SAMPLE_PERIOD_MS);
	}
}
#endif

void fATSs(void *arg)
{
	int argc = 0;
	char *argv[MAX_ARGC] = {0};

	AT_PRINTK("[ATS@]: _AT_SYSTEM_DBG_SETTING_");
	if(!arg){
		AT_PRINTK("[ATS@] Usage: ATS@=[LEVLE,FLAG]");
	}else{
		argc = parse_param(arg, argv);
		if(argc == 3){
			char *ptmp;
			gDbgLevel = atoi(argv[1]);
			gDbgFlag = strtoul(argv[2], &ptmp, 16);
		}
	}
	AT_PRINTK("[ATS@] level = %d, flag = 0x%08X", gDbgLevel, gDbgFlag);
}

void fATSc(void *arg)
{
#if defined (CONFIG_PLATFORM_8721D)
	int argc = 0, config = 0;
	char *argv[MAX_ARGC] = {0};

	AT_PRINTK("[ATS!]: _AT_SYSTEM_CONFIG_SETTING_");
	if(!arg){
		AT_PRINTK("[ATS!] Usage: ATS!=[CONFIG(0,1,2),FLAG]");
	}else{
		argc = parse_param(arg, argv);
		if(argc == 3){
			char *ptmp;
			config = atoi(argv[1]);
			if(config == 0)
				ConfigDebug[LEVEL_ERROR] = strtoul(argv[2], &ptmp, 16);
			if(config == 1)
				ConfigDebug[LEVEL_INFO] = strtoul(argv[2], &ptmp, 16);
			if(config == 2)
				ConfigDebug[LEVEL_WARN] = strtoul(argv[2], &ptmp, 16);
		}
	}
	AT_PRINTK("[ATS!] ConfigDebugErr  = 0x%08X", ConfigDebug[LEVEL_ERROR]);
	AT_PRINTK("[ATS!] ConfigDebugInfo = 0x%08X", ConfigDebug[LEVEL_INFO]);
	AT_PRINTK("[ATS!] ConfigDebugWarn = 0x%08X", ConfigDebug[LEVEL_WARN]);
#else
	int argc = 0, config = 0;
	char *argv[MAX_ARGC] = {0};

	AT_PRINTK("[ATS!]: _AT_SYSTEM_CONFIG_SETTING_");
	if(!arg){
		AT_PRINTK("[ATS!] Usage: ATS!=[CONFIG(0,1,2),FLAG]");
	}else{
		argc = parse_param(arg, argv);
		if(argc == 3){
			char *ptmp;
			config = atoi(argv[1]);
			if(config == 0)
				ConfigDebugErr = strtoul(argv[2], &ptmp, 16);
			if(config == 1)
				ConfigDebugInfo = strtoul(argv[2], &ptmp, 16);
			if(config == 2)
				ConfigDebugWarn = strtoul(argv[2], &ptmp, 16);
		}
	}
	AT_PRINTK("[ATS!] ConfigDebugErr  = 0x%08X", ConfigDebugErr);
	AT_PRINTK("[ATS!] ConfigDebugInfo = 0x%08X", ConfigDebugInfo);
	AT_PRINTK("[ATS!] ConfigDebugWarn = 0x%08X", ConfigDebugWarn);
#endif
}

#define SUPPORT_CP_TEST 0
#if SUPPORT_CP_TEST
extern void MFi_auth_test(void);
void fATSM(void *arg)
{
	AT_PRINTK("[ATSM]: _AT_SYSTEM_CP_");
	MFi_auth_test();
}
#endif

void fATSt(void *arg)
{
	AT_PRINTK("[ATS#]: _AT_SYSTEM_TEST_");
}

#if defined(CONFIG_PLATFORM_8711B)
/*Function: Check if the input jtag key is matched with the jtag password derived from the SB key stored in EFUSE.
		    If the input jtag key is correct, it will be stored in system data area of the flash.
		    Otherwise, the last 1 of the error map will be written to 0, which is also stored in system data of the flash. */
static void sys_enable_jtag_by_password(char *keystring)
{
	flash_t flash;
	u8 key[8];
	u32 data, key32[8], i = 0, errmap = 0;
	int is_match = 0;

	if(strlen(keystring) < 16){
		AT_PRINTK("%s(): Key length should be 16 characters.", __func__);
		return;
	}
	AT_PRINTK("Enter JTAG Key: %s\n", keystring);
	sscanf((const char*)keystring, "%02x%02x%02x%02x%02x%02x%02x%02x",
		&key32[0], &key32[1], &key32[2], &key32[3], &key32[4], &key32[5], &key32[6], &key32[7]);
	for(i=0; i<8; i++){
		key[i] = key32[i] & 0xFF;
	}

	device_mutex_lock(RT_DEV_LOCK_FLASH);
	flash_read_word(&flash, FLASH_SYSTEM_DATA_ADDR + 0x40, &errmap);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
	AT_PRINTK("Error Map: 0x%x\n", (errmap & 0xFF));
	if((errmap & 0xFF) == 0){
		AT_PRINTK("You have tried too much times!Locked!\n", keystring);
		return;
	}

	// check if jtag key is correct
	is_match = boot_export_symbol.is_jtag_key_match(key);

	device_mutex_lock(RT_DEV_LOCK_FLASH);
	flash_read_word(&flash, FLASH_SYSTEM_DATA_ADDR + 0x44, &data);
	if(data != ~0x0){
		//erase backup sector
		flash_erase_sector(&flash, FLASH_RESERVED_DATA_BASE);
		//backup system data to backup sector
		for(i = 0; i < 0x1000; i+= 4){
			flash_read_word(&flash, FLASH_SYSTEM_DATA_ADDR + i, &data);
			//Erase Errmap and key
			if((i == 0x44) || (i == 0x48))
				data = 0xFFFFFFFF;
			if((i == 0x40) && is_match)
				data = 0xFFFFFFFF;
			flash_write_word(&flash, FLASH_RESERVED_DATA_BASE + i,data);
		}

		//erase system data
		flash_erase_sector(&flash, FLASH_SYSTEM_DATA_ADDR);
		//write data back to system data
		for(i = 0; i < 0x1000; i+= 4){
			flash_read_word(&flash, FLASH_RESERVED_DATA_BASE + i, &data);
			flash_write_word(&flash, FLASH_SYSTEM_DATA_ADDR + i,data);
		}
		//erase backup sector
		flash_erase_sector(&flash, FLASH_RESERVED_DATA_BASE);
	}
	//write jtag key
	flash_stream_write(&flash, FLASH_SYSTEM_DATA_ADDR + 0x44, 8, key);
	//update error map
	if(is_match == 0){
		flash_write_word(&flash, FLASH_SYSTEM_DATA_ADDR + 0x40, errmap<<1);
	}
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
}
#endif

void fATSJ(void *arg)
{
	volatile int argc = 0;
	char *argv[MAX_ARGC] = {0};

#if defined (CONFIG_PLATFORM_8721D)
	#ifdef AMEBAD_TODO
	AT_PRINTK("[ATSJ]: _AT_SYSTEM_JTAG_");
	if(!arg){
		AT_PRINTK("[ATSJ] Usage: ATSJ=off");
	}else{
		argc = parse_param(arg, argv);
		if (strcmp(argv[1], "off" ) == 0)
			sys_jtag_off();
#if defined(CONFIG_PLATFORM_8711B)
		else if (strcmp(argv[1], "key" ) == 0)
			sys_enable_jtag_by_password(argv[2]); //Enter "FFFFFFFFFFFFFFFF" to clear key in flash
#endif
		else
			AT_PRINTK("ATSJ=%s is not supported!", argv[1]);
	}
	#endif
#else

	AT_PRINTK("[ATSJ]: _AT_SYSTEM_JTAG_");
	if(!arg){
		AT_PRINTK("[ATSJ] Usage: ATSJ=off");
	}else{
		argc = parse_param(arg, argv);
		if (strcmp(argv[1], "off" ) == 0)
			sys_jtag_off();
#if defined(CONFIG_PLATFORM_8711B)
		else if (strcmp(argv[1], "key" ) == 0)
			sys_enable_jtag_by_password(argv[2]); //Enter "FFFFFFFFFFFFFFFF" to clear key in flash
#endif
		else
			AT_PRINTK("ATSJ=%s is not supported!", argv[1]);
	}
#endif
}

void fATSx(void *arg)
{
//	uint32_t ability = 0;
	char buf[64];

	AT_PRINTK("[ATS?]: _AT_SYSTEM_HELP_");
	AT_PRINTK("[ATS?]: COMPILE TIME: %s", RTL_FW_COMPILE_TIME);
//	wifi_get_drv_ability(&ability);
	strcpy(buf, "v");
//	if(ability & 0x1)
//		strcat(buf, "m");
#if defined(CONFIG_PLATFORM_8710C)
	strcat(buf, ".7.1." RTL_FW_COMPILE_DATE);
#elif defined(CONFIG_PLATFORM_8195BHP)
	strcat(buf, ".5.2." RTL_FW_COMPILE_DATE);
#else
	strcat(buf, ".4.0." RTL_FW_COMPILE_DATE);
#endif
	AT_PRINTK("[ATS?]: SW VERSION: %s", buf);
}
#elif ATCMD_VER == ATVER_2

#define ATCMD_VERSION		"v2" //ATCMD MAJOR VERSION, AT FORMAT CHANGED
#define ATCMD_SUBVERSION	"2" //ATCMD MINOR VERSION, NEW COMMAND ADDED
#define ATCMD_REVISION		"1" //ATCMD FIX BUG REVISION
#define SDK_VERSION		"v3.5" //SDK VERSION
extern void sys_reset(void);
void print_system_at(void *arg);
extern void print_wifi_at(void *arg);
extern void print_tcpip_at(void *arg);

// uart version 2 echo info
extern unsigned char gAT_Echo;


void fATS0(void *arg){
	at_printf("\r\n[AT] OK");
}

void fATSh(void *arg){
	// print common AT command
	at_printf("\r\n[ATS?] ");
	at_printf("\r\nCommon AT Command:");
	print_system_at(arg);
#if CONFIG_WLAN
	at_printf("\r\nWi-Fi AT Command:");
	print_wifi_at(arg);
#endif

#if CONFIG_TRANSPORT
	at_printf("\r\nTCP/IP AT Command:");
	print_tcpip_at(arg);
#endif

	at_printf("\r\n[ATS?] OK");
}

void fATSR(void *arg){
	at_printf("\r\n[ATSR] OK");
	sys_reset();
}

void fATSV(void *arg){
	char at_buf[32];
	char fw_buf[32];

	// get at version
	strcpy(at_buf, ATCMD_VERSION"."ATCMD_SUBVERSION"."ATCMD_REVISION);

	// get fw version
	strcpy(fw_buf, SDK_VERSION);
#if defined CONFIG_PLATFORM_8195A
	at_printf("\r\n[ATSV] OK:%s,%s(%s)",at_buf,fw_buf,RTL8195AFW_COMPILE_TIME);
#elif defined CONFIG_PLATFORM_8710C
	at_printf("\r\n[ATSV] OK:%s,%s(%s)",at_buf,fw_buf,RTL8710CFW_COMPILE_TIME);
#endif
}

#if defined(configUSE_WAKELOCK_PMU) && (configUSE_WAKELOCK_PMU == 1)
void fATSP(void *arg){
	int argc = 0;
	char *argv[MAX_ARGC] = {0};

	uint32_t lock_id;
	uint32_t bitmap;

	if (!arg) {
		AT_DBG_MSG(AT_FLAG_COMMON, AT_DBG_ERROR, "\r\n[ATSP] Usage: ATSP=<a/r/?>");
		at_printf("\r\n[ATSP] ERROR:1");
		return;
	} else {
		if((argc = parse_param(arg, argv)) != 2){
			AT_DBG_MSG(AT_FLAG_COMMON, AT_DBG_ERROR, "\r\n[ATSP] Usage: ATSP=<a/r/?>");
			at_printf("\r\n[ATSP] ERROR:1");
			return;
		}
	}

	switch(argv[1][0]) {
		case 'a': // acquire
		{
			pmu_acquire_wakelock(PMU_OS);
			//at_printf("\r\n[ATSP] wakelock:0x%08x", pmu_get_wakelock_status());
			break;
		}

		case 'r': // release
		{
			pmu_release_wakelock(PMU_OS);
			//at_printf("\r\n[ATSP] wakelock:0x%08x", pmu_get_wakelock_status());
			break;
		}

		case '?': // get status
			break;
		default:
			AT_DBG_MSG(AT_FLAG_COMMON, AT_DBG_ERROR, "\r\n[ATSP] Usage: ATSP=<a/r/?>");
			at_printf("\r\n[ATSP] ERROR:2");
			return;
	}
	bitmap = pmu_get_wakelock_status();
	at_printf("\r\n[ATSP] OK:%s", (bitmap&BIT(PMU_OS))?"1":"0");
}
#endif

void fATSE(void *arg){
	int argc = 0;
	int echo = 0, mask = gDbgFlag, dbg_lv = gDbgLevel;
	char *argv[MAX_ARGC] = {0};
	int err_no = 0;

	AT_DBG_MSG(AT_FLAG_COMMON, AT_DBG_ALWAYS, "[ATSE]: _AT_SYSTEM_ECHO_DBG_SETTING");
	if(!arg){
		AT_DBG_MSG(AT_FLAG_COMMON, AT_DBG_ERROR, "[ATSE] Usage: ATSE=<echo>,<dbg_msk>,<dbg_lv>");
		err_no = 1;
		goto exit;
	}

	argc = parse_param(arg, argv);

	if(argc < 2 || argc > 4){
		err_no = 2;
		goto exit;
	}

#if CONFIG_EXAMPLE_UART_ATCMD
	if(argv[1] != NULL){
		echo = atoi(argv[1]);
		if(echo>1 || echo <0){
			err_no = 3;
			goto exit;
		}
		gAT_Echo = echo?1:0;
	}
#endif

	if((argc > 2) && (argv[2] != NULL)){
		mask = strtoul(argv[2], NULL, 0);
		at_set_debug_mask(mask);
	}

	if((argc == 4) && (argv[3] != NULL)){
		dbg_lv = strtoul(argv[3], NULL, 0);
		at_set_debug_level(dbg_lv);
	}

exit:
	if(err_no)
		at_printf("\r\n[ATSE] ERROR:%d", err_no);
	else
		at_printf("\r\n[ATSE] OK");
	return;
}
#if CONFIG_WLAN
extern int EraseApinfo();
extern int Erase_Fastconnect_data();

void fATSY(void *arg){
#if CONFIG_EXAMPLE_WLAN_FAST_CONNECT
	Erase_Fastconnect_data();
#endif

#if CONFIG_EXAMPLE_UART_ATCMD
	reset_uart_atcmd_setting();
#endif

#if CONFIG_OTA_UPDATE
	// Reset ota image  signature
	cmd_ota_image(0);
#endif

	at_printf("\r\n[ATSY] OK");
	// reboot
	sys_reset();
}

#if CONFIG_OTA_UPDATE
extern int wifi_is_connected_to_ap( void );
void fATSO(void *arg){
	int argc = 0;
	char *argv[MAX_ARGC] = {0};

	if(!arg){
		AT_DBG_MSG(AT_FLAG_OTA, AT_DBG_ERROR, "\r\n[ATSO] Usage: ATSO=<ip>,<port>");
		at_printf("\r\n[ATSO] ERROR:1");
		return;
	}
	argv[0] = "update";
	if((argc = parse_param(arg, argv)) != 3){
		AT_DBG_MSG(AT_FLAG_OTA, AT_DBG_ERROR, "\r\n[ATSO] Usage: ATSO=<ip>,<port>");
		at_printf("\r\n[ATSO] ERROR:1");
		return;
	}

	// check wifi connect first
	if(wifi_is_connected_to_ap()==0){
		cmd_update(argc, argv);
		at_printf("\r\n[ATSO] OK");

	}else{
		at_printf("\r\n[ATSO] ERROR:3");
	}
}

void fATSC(void *arg){
	int argc = 0;
	char *argv[MAX_ARGC] = {0};
	int cmd = 0;

	if(!arg){
		AT_DBG_MSG(AT_FLAG_OTA, AT_DBG_ERROR, "\r\n[ATSC] Usage: ATSC=<0/1>");
		at_printf("\r\n[ATSC] ERROR:1");
		return;
	}
	if((argc = parse_param(arg, argv)) != 2){
		AT_DBG_MSG(AT_FLAG_OTA, AT_DBG_ERROR, "\r\n[ATSC] Usage: ATSC=<0/1>");
	  at_printf("\r\n[ATSC] ERROR:1");
	  return;
	}

	cmd = atoi(argv[1]);

	if((cmd!=0)&&(cmd!=1)){
		at_printf("\r\n[ATSC] ERROR:2");
		return;
	}

	at_printf("\r\n[ATSC] OK");

	if(cmd == 1){
		cmd_ota_image(1);
	}
	else{
		cmd_ota_image(0);
	}
	// reboot
	sys_reset();
}
#endif

#if CONFIG_EXAMPLE_UART_ATCMD
extern const u32 log_uart_support_rate[];

void fATSU(void *arg){
	int argc = 0;
	char *argv[MAX_ARGC] = {0};
	u32 baud = 0;
	u8 databits = 0;
	u8 stopbits = 0;
	u8 parity = 0;
	u8 flowcontrol = 0;
	u8 configmode = 0;
	int i;
	UART_LOG_CONF uartconf;

	if(!arg){
		AT_DBG_MSG(AT_FLAG_COMMON, AT_DBG_ERROR,
		"[ATSU] Usage: ATSU=<baud>,<databits>,<stopbits>,<parity>,<flowcontrol>,<configmode>");
		at_printf("\r\n[ATSU] ERROR:1");
		return;
	}
	if((argc = parse_param(arg, argv)) != 7){
		AT_DBG_MSG(AT_FLAG_COMMON, AT_DBG_ERROR,
		"[ATSU] Usage: ATSU=<baud>,<databits>,<stopbits>,<parity>,<flowcontrol>,<configmode>");
		at_printf("\r\n[ATSU] ERROR:1");
		return;
	}

	baud = atoi(argv[1]);
	databits = atoi(argv[2]);
	stopbits = atoi(argv[3]);
	parity = atoi(argv[4]);
	flowcontrol = atoi(argv[5]);
	configmode = atoi(argv[6]);
/*
    // Check Baud rate
    for (i=0; log_uart_support_rate[i]!=0xFFFFFF; i++) {
        if (log_uart_support_rate[i] == baud) {
            break;
        }
    }

    if (log_uart_support_rate[i]== 0xFFFFFF) {
		at_printf("\r\n[ATSU] ERROR:2");
        return;
    }
*/
	if(((databits < 5) || (databits > 8))||\
		((stopbits < 1) || (stopbits > 2))||\
		((parity < 0) || (parity > 2))||\
		((flowcontrol < 0) || (flowcontrol > 1))||\
		((configmode < 0) || (configmode > 3))\
		){
		at_printf("\r\n[ATSU] ERROR:2");
		return;
	}

	memset((void*)&uartconf, 0, sizeof(UART_LOG_CONF));
	uartconf.BaudRate = baud;
	uartconf.DataBits = databits;
	uartconf.StopBits = stopbits;
	uartconf.Parity = parity;
	uartconf.FlowControl = flowcontrol;
	AT_DBG_MSG(AT_FLAG_COMMON, AT_DBG_ALWAYS,
		"AT_UART_CONF: %d,%d,%d,%d,%d", uartconf.BaudRate, uartconf.DataBits,uartconf.StopBits,uartconf.Parity,uartconf.FlowControl);
	switch(configmode){
		case 0: // set current configuration, won't save
			uart_atcmd_reinit(&uartconf);
			break;
		case 1: // set current configuration, and save
			write_uart_atcmd_setting_to_system_data(&uartconf);
			uart_atcmd_reinit(&uartconf);
			break;
		case 2: // set configuration, reboot to take effect
			write_uart_atcmd_setting_to_system_data(&uartconf);
			break;
	}

	at_printf("\r\n[ATSU] OK");
}
#endif //#if CONFIG_EXAMPLE_UART_ATCMD
#endif //#if CONFIG_WLAN

void fATSG(void *arg)
{
	gpio_t gpio_ctrl;
	int argc = 0, val, error_no = 0;
	char *argv[MAX_ARGC] = {0}, port, num;
	PinName pin = NC;

	AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "[ATSG]: _AT_SYSTEM_GPIO_CTRL_");

	if(!arg){
		AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ERROR,
		"[ATSG] Usage: ATSG=<R/W>,<PORT>,<DATA>,<DIR>,<PULL>");
		error_no = 1;
		goto exit;
	}
	if((argc = parse_param(arg, argv)) < 3){
		AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ERROR,
			"[ATSG] Usage: ATSG=<R/W>,<PORT>,<DATA>,<DIR>,<PULL>");
		error_no = 2;
		goto exit;
	}

	port = argv[2][1];
	num = strtoul(&argv[2][3], NULL, 0);
	port -= 'A';
	pin = (port << 4 | num);
	AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "PORT: %s[%d]", argv[2], pin);

	if(gpio_set(pin) == 0xff)
	{
		AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ERROR, "[ATSG]: Invalid Pin Name [%d]", pin);
		error_no = 3;
		goto exit;
	}

	gpio_init(&gpio_ctrl, pin);
	if(argv[4]){
		int dir = atoi(argv[4]);
		AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "DIR: %s", argv[4]);
		gpio_dir(&gpio_ctrl, dir);
	}
	if(argv[5]){
		int pull = atoi(argv[5]);
		AT_DBG_MSG(AT_FLAG_GPIO, AT_DBG_ALWAYS, "PULL: %s", argv[5]);
		gpio_mode(&gpio_ctrl, pull);
	}
	if(argv[1][0] == 'R'){
		gpio_dir(&gpio_ctrl, PIN_INPUT);
		val = gpio_read(&gpio_ctrl);
	}
	else if(argv[1][0] == 'W'){
		val = atoi(argv[3]);
		gpio_dir(&gpio_ctrl, PIN_OUTPUT);
		gpio_write(&gpio_ctrl, val);
	}

exit:
	if(error_no){
		at_printf("\r\n[ATSG] ERROR:%d", error_no);
	}
	else{
		at_printf("\r\n[ATSG] OK:%d", val);
	}
}

#endif //#elif ATCMD_VER == ATVER_2
#if defined(configUSE_WAKELOCK_PMU) && (configUSE_WAKELOCK_PMU == 1)
void fATSL(void *arg)
{
	int argc = 0;
	char *argv[MAX_ARGC] = {0};
	int err_no = 0;
	uint32_t lock_id;

	AT_DBG_MSG(AT_FLAG_OS, AT_DBG_ALWAYS, "[ATSL]: _AT_SYS_WAKELOCK_TEST_");

	if (!arg) {
		AT_DBG_MSG(AT_FLAG_OS, AT_DBG_ALWAYS, "[ATSL] Usage ATSL=[a/r/?][bitmask]");
		err_no = 1;
		goto exit;
	} else {
		argc = parse_param(arg, argv);
		if (argc < 2) {
			AT_DBG_MSG(AT_FLAG_OS, AT_DBG_ALWAYS, "[ATSL] Usage ATSL=[a/r/?][bitmask]");
			err_no = 2;
			goto exit;
		}
	}

	switch(argv[1][0]) {
		case 'a': // acquire
		{
			if (argc == 3) {
				lock_id = strtoul(argv[2], NULL, 16);
				pmu_acquire_wakelock(lock_id);
			}
			AT_DBG_MSG(AT_FLAG_OS, AT_DBG_ALWAYS, "[ATSL] wakelock:0x%08x", pmu_get_wakelock_status());
			break;
		}

		case 'r': // release
		{
			if (argc == 3) {
				lock_id = strtoul(argv[2], NULL, 16);
				pmu_release_wakelock(lock_id);
			}
			AT_DBG_MSG(AT_FLAG_OS, AT_DBG_ALWAYS, "[ATSL] wakelock:0x%08x", pmu_get_wakelock_status());
			break;
		}

		case '?': // get status
			AT_DBG_MSG(AT_FLAG_OS, AT_DBG_ALWAYS, "[ATSL] wakelock:0x%08x", pmu_get_wakelock_status());
#if (configGENERATE_RUN_TIME_STATS == 1)
			//pmu_get_wakelock_hold_stats((char *)cBuffer);
			AT_DBG_MSG(AT_FLAG_OS, AT_DBG_ALWAYS, "%s", cBuffer);
#endif
			break;

#if (configGENERATE_RUN_TIME_STATS == 1)
		case 'c': // clean wakelock info (for recalculate wakelock hold time)
			AT_DBG_MSG(AT_FLAG_OS, AT_DBG_ALWAYS, "[ATSL] clean wakelock stat");
			//pmu_clean_wakelock_stat();
			break;
#endif
		default:
			AT_DBG_MSG(AT_FLAG_OS, AT_DBG_ALWAYS, "[ATSL] Usage ATSL=[a/r/?][bitmask]");
			err_no = 3;
			break;
	}
exit:
#if ATCMD_VER == ATVER_2
	if(err_no)
		at_printf("\r\n[ATSL] ERROR:%d", err_no);
	else
		at_printf("\r\n[ATSL] OK:0x%08x",pmu_get_wakelock_status());
#endif
	return;
}
#endif
#if defined(CONFIG_PLATFORM_8195BHP)
#include "flash_api.h"
void fATSF(void *arg)
{
	AT_PRINTK("[ATSF]: _AT_SYSTEM_FLASH_UNPROTECT");

	int argc = 0;
	char *argv[MAX_ARGC] = {0};
	flash_t         flash;

	if(!arg){
		printf("usage: \n\r");
		printf("\tATSF=0 to unlock flash\n\r");
		printf("\tATSF=1 to lock flash\n\r");
		printf("\tATSF=2 to clean the flash all status\n\r");
	}
	else{
		argc = parse_param(arg, argv);
		if(argc == 2){
			int cmd = atoi(argv[1]);
			if(cmd == 0){
				printf("Unlock flash\n\r");
				// reset BP0, BP1, BP2
				flash_set_status(&flash, (flash_get_status(&flash) & ~(0x4 | 0x8 | 0x10)));
				// reset CMP
				flash_set_status2(&flash, (flash_get_status2(&flash) & ~(0x40)));
				// reset WPS
				flash_set_status3(&flash, (flash_get_status3(&flash) & ~(0x4)));
			}
			else if(cmd == 1){
				printf("Lock flash\n\r");
				// set Winbond flash to protect status
				flash_set_status(&flash, 0x00);
				flash_set_status2(&flash, 0x42);
			}else if(cmd == 2){
				printf("Unlock flash\n\r");
				flash_set_status(&flash,0);
				flash_set_status2(&flash,0);
				flash_set_status3(&flash,0);
			}
			else
				printf("unknown cmd\n\r");
		}
	}
	printf("Status register: %x %x %x\n\r", flash_get_status(&flash), flash_get_status2(&flash), flash_get_status3(&flash));
        extern hal_spic_adaptor_t *pglob_spic_adaptor;
	if(pglob_spic_adaptor != NULL){
              printf("Flash ID = %x %x %x\r\n",pglob_spic_adaptor->flash_id[0],pglob_spic_adaptor->flash_id[1],pglob_spic_adaptor->flash_id[2]);
	}
}
#endif

#if defined(CONFIG_PLATFORM_8195BHP)
#define EFUSE_LMAP_SIZE 512

static int efuse_hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int efuse_hex_to_bytes(const char *hex, u8 *out, int max_len)
{
	int i, n;
	int len = (int)strlen(hex);

	if (len == 0 || (len & 1) || (len / 2) > max_len) {
		return -1;
	}
	n = len / 2;
	for (i = 0; i < n; i++) {
		int hi = efuse_hex_nibble(hex[i * 2]);
		int lo = efuse_hex_nibble(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0) {
			return -1;
		}
		out[i] = (u8)((hi << 4) | lo);
	}
	return n;
}

void fEFUSE(void *arg)
{
	char sub[8] = {0};
	unsigned int addr = 0;
	unsigned int len = 0;
	int n;

	AT_DBG_MSG(AT_FLAG_EDIT, AT_DBG_ALWAYS, "[EFUSE]: _AT_EFUSE_LOGICAL_MAP_");

	if (!arg) {
		AT_PRINTK("[EFUSE] Usage: efuse rmap [<addr_hex> <len>]   (no arg: full 512B)");
		AT_PRINTK("[EFUSE] Usage: efuse wmap <addr_hex> <len> <data_hex>");
		AT_PRINTK("[EFUSE] Example: efuse rmap");
		AT_PRINTK("[EFUSE] Example: efuse rmap 0x11a 6");
		AT_PRINTK("[EFUSE] Example: efuse wmap 0x11A 6 00e04c000001");
		return;
	}

	n = sscanf((char *)arg, "%7s %x %u", sub, &addr, &len);
	if (n < 1) {
		AT_PRINTK("[EFUSE] Usage: efuse rmap [<addr_hex> <len>] / efuse wmap <addr_hex> <len> <data_hex>");
		return;
	}

	if (strcmp(sub, "rmap") != 0 && strcmp(sub, "wmap") != 0) {
		AT_PRINTK("[EFUSE] unknown subcommand '%s' (use rmap/wmap)", sub);
		return;
	}

	if (strcmp(sub, "rmap") == 0) {
		/* rmap 可选参数：无参数时全量读取逻辑映射（参考 amebasmart monitor_ap.c） */
		if (n == 2) {
			AT_PRINTK("[EFUSE] rmap needs both addr and len (or none)");
			return;
		}
		if (n < 3) {
			addr = 0;
			len = EFUSE_LMAP_SIZE;
		}
	} else if (strcmp(sub, "wmap") == 0 && n < 3) {
		AT_PRINTK("[EFUSE] wmap needs addr len data: efuse wmap <addr_hex> <len> <data_hex>");
		return;
	}

	if (addr >= EFUSE_LMAP_SIZE || len == 0 || (addr + len) > EFUSE_LMAP_SIZE) {
		AT_PRINTK("[EFUSE] addr/len out of range (addr=0x%x len=%u)", addr, len);
		return;
	}

	if (strcmp(sub, "rmap") == 0) {
		u8 buf[EFUSE_LMAP_SIZE];
		int ret;
		unsigned int i;

		memset(buf, 0xFF, sizeof(buf));
		ret = efuse_logical_read((u16)addr, (u16)len, buf);
		if (ret < 0) {
			AT_PRINTK("[EFUSE] read logical map failed (ret=%d)", ret);
			return;
		}

		AT_PRINTK("[EFUSE] rmap 0x%X +%u:", addr, len);
		for (i = 0; i < len; i++) {
			if ((i & 0xF) == 0) {
				printf("  0x%03X: ", addr + i);
			}
			printf("%02X ", buf[i]);
			if ((i & 0xF) == 0xF) {
				printf("\r\n");
			}
		}
		if ((len & 0xF) != 0) {
			printf("\r\n");
		}
	} else if (strcmp(sub, "wmap") == 0) {
		char data_str[129] = {0};
		u8 data[EFUSE_LMAP_SIZE];
		int n;
		int ret;
		unsigned int i;

		if (sscanf((char *)arg, "%7s %x %u %128s", sub, &addr, &len, data_str) < 4) {
			AT_PRINTK("[EFUSE] wmap needs data: efuse wmap <addr_hex> <len> <data_hex>");
			return;
		}

		n = efuse_hex_to_bytes(data_str, data, sizeof(data));
		if (n < 0) {
			AT_PRINTK("[EFUSE] invalid data hex '%s'", data_str);
			return;
		}
		if ((unsigned int)n != len) {
			AT_PRINTK("[EFUSE] data length %d != len %u", n, len);
			return;
		}

		ret = efuse_logical_write((u16)addr, (u16)len, data);
		if (ret < 0) {
			AT_PRINTK("[EFUSE] write logical map failed (ret=%d)", ret);
			return;
		}

		{
			u8 rbuf[EFUSE_LMAP_SIZE];
			int ok = 1;

			memset(rbuf, 0xFF, sizeof(rbuf));
			if (efuse_logical_read((u16)addr, (u16)len, rbuf) < 0) {
				AT_PRINTK("[EFUSE] write done, but read-back failed");
				return;
			}
			for (i = 0; i < len; i++) {
				if (rbuf[i] != data[i]) {
					ok = 0;
					break;
				}
			}
			if (ok) {
				AT_PRINTK("[EFUSE] wmap 0x%X +%u OK", addr, len);
			} else {
				AT_PRINTK("[EFUSE] wmap verify FAIL at byte %u (wrote 0x%02X, read 0x%02X)", i, data[i], rbuf[i]);
			}
		}
	} else {
		AT_PRINTK("[EFUSE] unknown subcommand '%s' (use rmap/wmap)", sub);
	}
}

/* OTP 逻辑映射读写入口：调用 usbsmart_otp_compat.c 的 OTP_LogicalMap_Read/
 * OTP_LogicalMap_Write（与 fEFUSE 同一条 efuse_logical_* 底层），命令风格
 * 参考 fATWZ，hex dump 对齐 "ATWZ=dbg <addr> <len>" 的输出。 */
void fOTPMAP(void *arg)
{
	char sub[8] = {0};
	unsigned int addr = 0;
	unsigned int len = 0;
	int n;

	AT_DBG_MSG(AT_FLAG_EDIT, AT_DBG_ALWAYS, "[OTPMAP]: _AT_OTP_LOGICAL_MAP_");

	if (!arg) {
		AT_PRINTK("[OTPMAP] Usage: otpmap rd [<addr_hex> <len>]   (no arg: full 512B)");
		AT_PRINTK("[OTPMAP] Usage: otpmap wr <addr_hex> <len> <data_hex>");
		AT_PRINTK("[OTPMAP] Example: otpmap rd 0x11a 6");
		AT_PRINTK("[OTPMAP] Example: otpmap wr 0x11A 6 00e04c000001");
		return;
	}

	n = sscanf((char *)arg, "%7s %x %u", sub, &addr, &len);
	if (n < 1) {
		AT_PRINTK("[OTPMAP] Usage: otpmap rd [<addr_hex> <len>] / otpmap wr <addr_hex> <len> <data_hex>");
		return;
	}

	if (strcmp(sub, "rd") != 0 && strcmp(sub, "wr") != 0) {
		AT_PRINTK("[OTPMAP] unknown subcommand '%s' (use rd/wr)", sub);
		return;
	}

	if (strcmp(sub, "rd") == 0) {
		/* rd 可选参数：无参数时全量读取逻辑映射 */
		if (n == 2) {
			AT_PRINTK("[OTPMAP] rd needs both addr and len (or none)");
			return;
		}
		if (n < 3) {
			addr = 0;
			len = EFUSE_LMAP_SIZE;
		}
	} else if (n < 3) {
		AT_PRINTK("[OTPMAP] wr needs addr len data: otpmap wr <addr_hex> <len> <data_hex>");
		return;
	}

	if (addr >= EFUSE_LMAP_SIZE || len == 0 || (addr + len) > EFUSE_LMAP_SIZE) {
		AT_PRINTK("[OTPMAP] addr/len out of range (addr=0x%x len=%u)", addr, len);
		return;
	}

	if (strcmp(sub, "rd") == 0) {
		u8 buf[EFUSE_LMAP_SIZE];
		u32 ret;
		unsigned int i;

		memset(buf, 0xFF, sizeof(buf));
		/* 注意：OTP_LogicalMap_Read/Write 定义在 usb_lib 侧
		 * (usbsmart_otp_compat.c)，其 _SUCCESS==0/_FAIL==-1；
		 * 本文件属于 common/api 侧，_SUCCESS==1/_FAIL==0。
		 * 跨库调用不能拿本文件的 _SUCCESS 去比较，否则会把
		 * 底层成功返回的 0 误判为失败。按底层实际约定
		 * 0=成功、0xFFFFFFFF=失败 判断。 */
		ret = OTP_LogicalMap_Read(buf, addr, len);
		if (ret != 1) {
			AT_PRINTK("[OTPMAP] read logical map failed (ret=%u)", (unsigned int)ret);
			return;
		}

		AT_PRINTK("[OTPMAP] rd 0x%X +%u:", addr, len);
		for (i = 0; i < len; i++) {
			if ((i & 0xF) == 0) {
				printf("0x%03X   ", addr + i);
			}
			printf("%02X ", buf[i]);
			if ((i & 0xF) == 0xF) {
				printf("\r\n");
			}
		}
		if ((len & 0xF) != 0) {
			printf("\r\n");
		}
	} else if (strcmp(sub, "wr") == 0) {
		char data_str[129] = {0};
		u8 data[EFUSE_LMAP_SIZE];
		int m;
		u32 ret;
		unsigned int i;

		if (sscanf((char *)arg, "%7s %x %u %128s", sub, &addr, &len, data_str) < 4) {
			AT_PRINTK("[OTPMAP] wr needs data: otpmap wr <addr_hex> <len> <data_hex>");
			return;
		}

		m = efuse_hex_to_bytes(data_str, data, sizeof(data));
		if (m < 0) {
			AT_PRINTK("[OTPMAP] invalid data hex '%s'", data_str);
			return;
		}
		if ((unsigned int)m != len) {
			AT_PRINTK("[OTPMAP] data length %d != len %u", m, len);
			return;
		}

		ret = OTP_LogicalMap_Write(addr, len, data);
		if (ret != 1) {
			AT_PRINTK("[OTPMAP] write logical map failed (ret=%u)", (unsigned int)ret);
			return;
		}

		{
			u8 rbuf[EFUSE_LMAP_SIZE];
			int ok = 1;

			memset(rbuf, 0xFF, sizeof(rbuf));
			if (OTP_LogicalMap_Read(rbuf, addr, len) != 1) {
				AT_PRINTK("[OTPMAP] write done, but read-back failed");
				return;
			}
			for (i = 0; i < len; i++) {
				if (rbuf[i] != data[i]) {
					ok = 0;
					break;
				}
			}
			if (ok) {
				AT_PRINTK("[OTPMAP] wr 0x%X +%u OK", addr, len);
			} else {
				AT_PRINTK("[OTPMAP] wr verify FAIL at byte %u (wrote 0x%02X, read 0x%02X)", i, data[i], rbuf[i]);
			}
		}
	} else {
		AT_PRINTK("[OTPMAP] unknown subcommand '%s' (use rd/wr)", sub);
	}
}

/* EN25S64A user OTP read/write test. Writes are IRREVERSIBLE (OTP bits can
 * only be cleared once). read returns the boot-time cache; write/test program
 * the device and verify by reading the hardware back, so they need the flash
 * resource lock held around the transaction. */
/* 主线NOR OTP为只读缓存，不移入客户SDK的擦写/测试命令。 */

#endif

#ifndef CONFIG_TOUCH_AA_COMMAND
#define CONFIG_TOUCH_AA_COMMAND 0
#endif
#if CONFIG_TOUCH_AA_COMMAND
extern void carbox_touch_aa_command(void *arg);
#endif

log_item_t at_sys_items[] = {
#if CONFIG_TOUCH_AA_COMMAND
	{"aa", carbox_touch_aa_command,},
#endif
#if CONFIG_MMF_AUDIO_DEBUG
	{"ATAD", fATAD,},
	{"ATAS", fATAS,},
#endif
#if CONFIG_MMF_AUDIO_ATAF
	{"ATAF", fATAF,},
#endif
#ifndef CONFIG_INIC_NO_FLASH
#if ATCMD_VER == ATVER_1
	{"ATSD", fATSD,},	// Dump register
	{"ATSE", fATSE,},	// Edit register
#if defined(CONFIG_PLATFORM_8711B)
	{"ATSK", fATSK,},   // Set RDP/RSIP enable and key
#endif
#if CONFIG_OTA_UPDATE
	{"ATSC", fATSC,},	// Clear OTA signature
	{"ATSR", fATSR,},	// Recover OTA signature
#endif
#if CONFIG_UART_YMODEM
	{"ATSY", fATSY,},	// uart ymodem upgrade
#endif
#if SUPPORT_MP_MODE
	{"ATSA", fATSA,},	// MP ADC test
	{"ATSG", fATSG,},	// MP GPIO test
	{"ATSP", fATSP,},	// MP Power related test
	{"ATSB", fATSB,},	// OTU PIN setup
#endif
#if (configGENERATE_RUN_TIME_STATS == 1)
	{"ATSS", fATSS,},	// Show CPU stats
#endif
#if SUPPORT_CP_TEST
	{"ATSM", fATSM,},	// Apple CP test
#endif
#if !defined(CONFIG_PLATFORM_8195BHP) && !defined(CONFIG_PLATFORM_8710C)
	{"ATSJ", fATSJ,}, //trun off JTAG
#endif
	{"ATS@", fATSs,},	// Debug message setting
	{"ATS!", fATSc,},	// Debug config setting
	{"ATS#", fATSt,},	// test command
	{"ATS?", fATSx,},	// Help
#elif ATCMD_VER == ATVER_2 //#if ATCMD_VER == ATVER_1
	{"AT", 	 fATS0,},	// test AT command ready
	{"ATS?", fATSh,},	// list all AT command
	{"ATSR", fATSR,},	// system restart
	{"ATSV", fATSV,},	// show version info
#if defined(CONFIG_PLATFORM_8195A)
	{"ATSP", fATSP,},	// power saving mod
#endif
	{"ATSE", fATSE,},	// enable and disable echo
#if CONFIG_WLAN
	{"ATSY", fATSY,},	// factory reset
#if CONFIG_OTA_UPDATE
	{"ATSO", fATSO,},	// ota upgrate
	{"ATSC", fATSC,},	// chose the activited image
#endif
#if CONFIG_EXAMPLE_UART_ATCMD
	{"ATSU", fATSU,},	// AT uart configuration
#endif
#endif
	{"ATSG", fATSG,},	// GPIO control
#endif // end of #if ATCMD_VER == ATVER_1

// Following commands exist in two versions
#if defined(configUSE_WAKELOCK_PMU) && (configUSE_WAKELOCK_PMU == 1)
	{"ATSL", fATSL,},	 // wakelock test
#endif
#endif
// Unlock the flash status
#if defined(CONFIG_PLATFORM_8195BHP)
        {"ATSF", fATSF,},	 // wakelock test
	{"efuse", fEFUSE,},	 // efuse logical map read/write
	{"otpmap", fOTPMAP,},	 // OTP_LogicalMap_Read/Write (usbsmart_otp_compat.c)
	{"OTARECOVER", fOTARECOVER,},	 // 手动切到 OTA2(fw2)
	{"OTACLEAR", fOTACLEAR,},	 // 手动切到 OTA1(fw1)
	{"set_tx_power", fSETTXPOWER,},	 // 设置 WLAN 发射功率百分比 0..4
#if CARBOX_RECOVERY
	{"mptest", fMPTEST,},	 // MP 自动检测：依次执行 mp_start/channel/bandwidth/rate/txpower/ctx 序列
	{"MP", at_mp,},	 // MP 产测：MP=0 重启 / MP=1 USB 加载 / MP=2 USB 卸载 / MP=3 I2C 测试
#endif
#endif
};

#if ATCMD_VER == ATVER_2
void print_system_at(void *arg){
	int index;
	int cmd_len = 0;

	cmd_len = sizeof(at_sys_items)/sizeof(at_sys_items[0]);
	for(index = 0; index < cmd_len; index++)
		at_printf("\r\n%s", at_sys_items[index].log_cmd);
}
#endif
void at_sys_init(void)
{
	log_service_add_table(at_sys_items, sizeof(at_sys_items)/sizeof(at_sys_items[0]));
}

#if SUPPORT_LOG_SERVICE
log_module_init(at_sys_init);
#endif
