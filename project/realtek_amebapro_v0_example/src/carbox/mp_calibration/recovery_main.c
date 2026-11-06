/* 客户已测通的 Recovery MP 启动入口。仅 application.recovery.mk 编译。 */
#include <platform_opts.h>
#include <platform_stdlib.h>
#include <FreeRTOS.h>
#include <task.h>
#include "main.h"
#include "build_info.h"
#include "vfs.h"
#include "diag.h"
#include "carbox_vfs_compat.h"
#include "carbox_diag.h"
#include "fault_dump.h"
#include "hal_uart.h"
extern hal_uart_adapter_t log_uart;
extern void console_init(void);
extern void wlan_network(void);
#define CARBOX_LOGUART_BAUD 1500000
#if CARBOX_RECOVERY
#include "mp_calibration.h"
#include "recovery_mp.h"
#include "lwip_netconf.h"
/* 在建立任何AP/OTA任务前选择启动分支。MP分支不执行wlan_network/car_app_start。 */
static void recovery_start(void *arg)
{
    int mode;
    (void)arg;
    mode = recovery_mp_boot_select();
    if (mode == 1) {
        recovery_mp_start();
    } else if (mode == 0) {
        extern void wifi_init(void);
        extern void test_wifi_dev(void);
        rt_printf("[REC_MP] default OTA; MP=4 reboots into MP\r\n");
        wlan_network();
        wifi_init();
        test_wifi_dev();
    }
    /* 先完成模式选择，再开放串口。失败时仍允许MP=0/OTACLEAR恢复。 */
    console_init();
    vTaskDelete(NULL);
}
static void recovery_main(void)
{
    int ro, rw;
    hal_uart_set_baudrate(&log_uart, CARBOX_LOGUART_BAUD);
    rt_printf("[REC_MP] build=%s cpu_hz=%lu fault=%d\r\n", BOX_APP_VERSION,
              (unsigned long)SystemCoreClock, carbox_fault_dump_init());
    /* 不调用400MHz/DDR调相入口，沿用已测通MP版本的SDK启动时钟。 */
    vfs_init();
    ro = vfs_user_register(VFS_FAT_PREFIX, VFS_FATFS, VFS_INF_FLASH, VFS_REGION_1, VFS_RO);
    rw = vfs_user_register(VFS_PREFIX, VFS_LITTLEFS, VFS_INF_FLASH, VFS_REGION_2, VFS_RW);
    rt_printf("[REC_MP] filesystem fat=%d lfs=%d\r\n", ro, rw);
    carbox_diag_init();
    if (ro != 0 || rw != 0 || xTaskCreate(recovery_start, "recovery_start", 4096, NULL,
                                        tskIDLE_PRIORITY + 4, NULL) != pdPASS) {
        rt_printf("[REC_MP] startup failed; reboot required\r\n");
        console_init();
    }
    vTaskStartScheduler();
    for (;;) {}
}
#endif

char *get_build_version(void) { return BOX_APP_VERSION; }
void main(void) { recovery_main(); }
