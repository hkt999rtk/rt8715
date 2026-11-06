/* Recovery-only stubs for the Smart SDK OTA HTTP server.
 *
 * The full lib_usbsmart.a is excluded from the recovery image, but the OTA
 * page (ota_httpd.c / httpd_core.c) references a handful of Smart
 * compatibility hooks that normally live in the USB backend.  Recovery has no
 * USB accessory and does not program efuse, so these are no-op / placeholder
 * implementations.
 */
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "led_rgb.h"
#include "rt_printf.h"

/* Smart OTA status callback —— recovery 场景通过 RGB LED 指示升级进度。
 * 1 升级中：闪红灯；2 升级成功：红绿蓝来回闪烁；其他：灭灯。 */
static volatile int recovery_led_status = 0;
/* OTA一旦开始，本次启动不再接受MP切换。成功后等待重启也属于忙碌；
 * 失败后FW1签名也可能已变化，不能再依据开机时的fw1_valid快照切换。 */
/* 由recovery_ota_guard.S在OTA初始化入口置位，必须在第一笔写入之前。 */
volatile int recovery_ota_started;
int recovery_ota_is_busy(void) { return recovery_ota_started; }
static TaskHandle_t recovery_led_task_handle = NULL;

/* 调用 led_rgb 并打印状态变化，便于串口确认是参数错误还是 ICC 链路错误。
 * 0 表示 HP→LP 已 ACK；-1 参数非法；-4 超时（LP 未 ACK）。 */
static void recovery_led_apply(float r, float g, float b)
{
	static int last_status = LED_RGB_NOT_READY;
	int led_status;

	led_rgb(r, g, b);
	led_status = led_rgb_get_status();
	if (led_status != last_status) {
		rt_printf("[LED_RGB] led_rgb status=%d (0=OK, -1=invalid-arg, -2=busy, "
		          "-3=not-ready, -4=timeout, -5=transport)\r\n", led_status);
		last_status = led_status;
	}
}

static void recovery_led_task(void *param)
{
	(void)param;
	int color_index = 0;

	for (;;) {
		int status = recovery_led_status;

		if (status == 1) {
			/* 升级中：红灯闪烁（亮/灭交替）。
			 * 本板 RGB LED 反相驱动：led_rgb 写入 0=开、1=关
			 * （协议打包后 0=开、255=关）。 */
			recovery_led_apply(0.0f, 1.0f, 1.0f);   /* 红开、绿关、蓝关 → 红 */
			vTaskDelay(pdMS_TO_TICKS(50));
			recovery_led_apply(1.0f, 1.0f, 1.0f);   /* 全关 → 灭灯 */
			vTaskDelay(pdMS_TO_TICKS(50));
		} else if (status == 2) {
			/* 升级成功：红 → 绿 → 蓝 循环（反相：0=开、1=关） */
			static const float colors[3][3] = {
				{0.0f, 1.0f, 1.0f},  /* R */
				{1.0f, 0.0f, 1.0f},  /* G */
				{1.0f, 1.0f, 0.0f},  /* B */
			};
			recovery_led_apply(colors[color_index][0], colors[color_index][1], colors[color_index][2]);
			color_index = (color_index + 1) % 3;
			vTaskDelay(pdMS_TO_TICKS(500));
		} else {
			/* 失败等：灭灯（全关） */
			recovery_led_apply(1.0f, 1.0f, 1.0f);
			vTaskDelay(pdMS_TO_TICKS(500));
		}
	}
}

/* recovery 场景的 OTA 状态上报入口：用 RGB LED 指示升级进度。
 * normal 场景走 usb_common.c 的 usb_ota_update_status（AAC 回调上报）；
 * recovery 场景由 sta_ota.c 通过弱引用直接调用本函数（LED）。
 * 这里不再定义 usb_ota_update_status，避免与 usb_common.o 强定义重复。 */
void recovery_ota_update_status(int status, int percent)
{
	(void)percent;

	if (status != 0) recovery_ota_started = 1;
	recovery_led_status = status;

	if (recovery_led_task_handle == NULL) {
		if (xTaskCreate(recovery_led_task, "recovery_led",
		                512, NULL, tskIDLE_PRIORITY + 1,
		                &recovery_led_task_handle) != pdPASS) {
			recovery_led_task_handle = NULL;
		}
	}
}

/* wifi_driver.o 引用 usb_os_sema_give（-Wl,-wrap），httpd_msg.o 引用
 * rtos_critical_enter/exit 与 rtos_time_get_current_system_time_ms。
 * 这些符号现在由 libusb_ref_compat_os.c 提供（recovery 已编译 compat，
 * 为 MP 命令的 USB host 按需提取提供 ISR 安全实现），这里不再重复定义。 */

/* Smart OTP logical-map API — recovery does not program efuse. */
uint32_t OTP_LogicalMap_Read(uint8_t *pbuf, uint32_t addr, uint32_t len)
{
	(void)pbuf;
	(void)addr;
	(void)len;
	return 1U; /* != _SUCCESS */
}

uint32_t OTP_LogicalMap_Write(uint32_t addr, uint32_t cnts, uint8_t *data)
{
	(void)addr;
	(void)cnts;
	(void)data;
	return 1U; /* != _SUCCESS */
}

/* Smart diagnostic print used by the OTA/HTTPD path. */
int DiagSnPrintf(char *buf, size_t size, const char *fmt, ...)
{
	int ret;
	va_list ap;

	if (buf == NULL || size == 0U) {
		return 0;
	}
	va_start(ap, fmt);
	ret = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return ret;
}

/* CarPlay accessory getters referenced by the OTA page callbacks. */
char *bt_get_local_name(void)
{
	return (char *)"bt_recovery";
}

char *acc_carplay_get_manufacturer(void)
{
	return (char *)"manufacturer_recovery";
}

char *acc_carplay_get_model(void)
{
	return (char *)"model_recovery";
}

int CSystemSetup_GetAccessoryType(void)
{
	return 5;
}
