/* Recovery 内的互斥启动分支。
 * 闭源 AP/OTA 库没有完整的停止/等待接口，不能靠 wifi_off+delay 热切换。
 * MP=4 通过一次性文件重启同一FW2，确保本次MP启动没有AP/OTA任务、回调或旧adapter。
 * 标记在启动Wi-Fi前删除；MP=0/掉电/复位后回到默认OTA，不永久改变启动模式。
 * 本文件不改FW签名、不写efuse。MP启动时完成原厂mp_start；标记只占LittleFS文件。
 */
#include <stdio.h>
#include <string.h>
#ifndef RECOVERY_MP_HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
#include "device_lock.h"
#include "carbox_littlefs.h"
#include "lfs.h"
#include "diag.h"
#include "sys_api.h"
#include "fw_img.h"
#include "hal_flash_boot.h"
#include "wifi_conf.h"
#include "wifi_util.h"
#include "wlan_intf.h"
#include "lwip_netconf.h"
#include "mp_calibration.h"
#include "recovery_mp.h"
/* 复位前等待在途flash操作，再暂停任务调度，关闭OTA检查与复位之间的竞争窗口。
 * 保护区内不得打印、操作文件或等待；目标sys_reset关闭中断并立即触发复位。 */
static void reset_guard_enter(void)
{
    device_mutex_lock(RT_DEV_LOCK_FLASH);
    vTaskSuspendAll();
}
static void reset_guard_exit(void)
{
    xTaskResumeAll();
    device_mutex_unlock(RT_DEV_LOCK_FLASH);
}
static int marker_exists(void)
{
    uint32_t size;
    int is_dir;
    int ret = carbox_littlefs_stat("/recovery_mp_once", &size, &is_dir);
    return ret == 0 ? 1 : ret == LFS_ERR_NOENT ? 0 : -1;
}
extern rtw_mode_t wifi_mode;
extern int is_promisc_enabled(void);
static int recovery_mp_current_slot(void)
{
    fw_img_export_info_type_t *i = get_fw_img_info_tbl();
    return i ? i->loaded_fw_idx : -1;
}
static int recovery_mp_fw1_valid(void)
{
    fw_img_export_info_type_t *i = get_fw_img_info_tbl();
    return i ? i->fw1_valid : -1;
}
#endif
#ifndef MP_ONCE_PATH
#define MP_ONCE_PATH "vfs:/recovery_mp_once"
#endif
static const unsigned char marker[16] = "RMP_ONCE_V1_8715";
enum recovery_phase { RECOVERY_BOOT, RECOVERY_OTA, RECOVERY_MP_START, RECOVERY_MP, RECOVERY_ERROR, RECOVERY_RESET };
static enum recovery_phase phase = RECOVERY_BOOT;

static int marker_read(void)
{
    unsigned char buf[sizeof(marker)+1];
    FILE *f = fopen(MP_ONCE_PATH, "rb");
    size_t n;
    int close_ret, read_error;
    /* fopen失败不等于文件不存在：内存或I/O异常时禁止猜测启动模式。 */
    if (!f) return marker_exists() == 0 ? 0 : -2;
    n = fread(buf, 1, sizeof(buf), f);
    read_error = ferror(f);
    close_ret = fclose(f);
    if (read_error || close_ret != 0) return -2;
    return close_ret == 0 && n == sizeof(marker) && memcmp(buf, marker, sizeof(marker)) == 0 ? 1 : -1;
}
int recovery_mp_boot_select(void)
{
    int flag = marker_read();
    if (phase != RECOVERY_BOOT) return -1;
    phase = RECOVERY_ERROR;
    if (flag == -2) {
        rt_printf("[REC_MP] marker I/O failed; Wi-Fi startup blocked\r\n");
        return -1;
    }
    if (flag != 0 && remove(MP_ONCE_PATH) != 0) {
        rt_printf("[REC_MP] marker consume failed; Wi-Fi startup blocked\r\n");
        return -1;
    }
    if (flag == 1) {
        if (recovery_mp_current_slot() != 2 || recovery_mp_fw1_valid() != 0) {
            rt_printf("[REC_MP] unexpected slot/signature; MP blocked\r\n");
            return -1;
        }
        phase = RECOVERY_MP_START;
        return 1;
    }
    if (flag < 0) rt_printf("[REC_MP] invalid marker removed; default OTA\r\n");
    phase = RECOVERY_OTA;
    return 0;
}
int recovery_mp_start(void)
{
    int ret;
    if (phase != RECOVERY_MP_START) return -1;
    LwIP_Init();
    ret = mp_calibration_prepare();
    rt_printf("[REC_MP] prepare=%d slot=%d cpu=%lu mode=%d driver=%d/%d promisc=%d ap_ota_started=0\r\n",
              ret, recovery_mp_current_slot(), (unsigned long)SystemCoreClock, (int)wifi_mode,
              rltk_wlan_running(0), rltk_wlan_running(1), is_promisc_enabled());
    if (ret != 0 || SystemCoreClock != 300000000U || wifi_mode != RTW_MODE_STA ||
        !rltk_wlan_running(0) || rltk_wlan_running(1) || is_promisc_enabled()) {
        phase = RECOVERY_ERROR;
        mp_calibration_shutdown();
        rt_printf("[REC_MP] INIT FAILED; commands blocked; MP=0 to reboot\r\n");
        return -1;
    }
    ret = mp_calibration_start();
    if (ret != 0 || !mp_calibration_is_ready()) {
        phase = RECOVERY_ERROR;
        mp_calibration_shutdown();
        rt_printf("[REC_MP] MP START FAILED ret=%d; commands blocked; MP=0 to reboot\r\n", ret);
        return -1;
    }
    phase = RECOVERY_MP;
    return 0;
}
int recovery_mp_request(void)
{
    FILE *f;
    int ok;
    if (phase == RECOVERY_MP) {
        rt_printf("[REC_MP] already MP; test state preserved; query MP=STATUS\r\n");
        return 0;
    }
    if (phase != RECOVERY_OTA || recovery_mp_current_slot() != 2 || recovery_mp_fw1_valid() != 0) {
        rt_printf("[REC_MP] require idle recovery FW2 entered via OTARECOVER\r\n");
        return -1;
    }
    if (recovery_ota_is_busy()) {
        rt_printf("[REC_MP] OTA busy; MP=4 rejected\r\n");
        return -1;
    }
    f = fopen(MP_ONCE_PATH, "wb");
    if (!f) return -1;
    ok = fwrite(marker, 1, sizeof(marker), f) == sizeof(marker);
    if (fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || marker_read() != 1 || recovery_ota_is_busy()) {
        /* 失败时不得留下有效的下一次启动请求。若清理也失败，封锁命令并报错。 */
        if (remove(MP_ONCE_PATH) != 0) phase = RECOVERY_ERROR;
        rt_printf("[REC_MP] marker write/verify failed or OTA became busy; no reboot\r\n");
        return -1;
    }
    rt_printf("[REC_MP] MP=4 verified; checking OTA before reboot; wait for MP READY\r\n");
    reset_guard_enter();
    if (recovery_ota_is_busy()) {
        reset_guard_exit();
        if (remove(MP_ONCE_PATH) != 0) phase = RECOVERY_ERROR;
        rt_printf("[REC_MP] OTA became busy; no reboot\r\n");
        return -1;
    }
    phase = RECOVERY_RESET;
    sys_reset();
    reset_guard_exit(); /* 仅供主机测试，目标复位不返回。 */
    return 0; /* 仅主机测试会返回；目标sys_reset不返回。 */
}
int recovery_mp_is_active(void) { return phase == RECOVERY_MP; }
int recovery_mp_ota_allowed(void) { return phase == RECOVERY_OTA; }
int recovery_mp_allow_console(const char *cmd)
{
    static const char *const allowed[] = {"MP", "OTACLEAR", "OTARECOVER", "ATS?", "ATSS", "help", "efuse", "otpmap", "otp", "set_tx_power"};
    size_t i, n;
    if (phase == RECOVERY_OTA) return 1;
    while (*cmd == ' ' || *cmd == '\t') ++cmd;
    n = strcspn(cmd, "= \t\r\n");
    if (phase == RECOVERY_MP && n == 6 && (memcmp(cmd, "iwpriv", 6) == 0 || memcmp(cmd, "mptest", 6) == 0)) return 1;
    for (i=0;i<sizeof(allowed)/sizeof(allowed[0]);++i)
        if (strlen(allowed[i]) == n && memcmp(cmd, allowed[i], n) == 0) return 1;
    return 0;
}

/* 只从console任务读取；查询不初始化Wi-Fi、不改变MP会话、不触发射频操作。 */
const char *recovery_mp_phase_name(void)
{
    switch (phase) {
    case RECOVERY_BOOT: return "BOOT";
    case RECOVERY_OTA: return "OTA";
    case RECOVERY_MP_START: return "MP_START";
    case RECOVERY_MP: return "MP";
    case RECOVERY_ERROR: return "ERROR";
    case RECOVERY_RESET: return "RESET";
    default: return "UNKNOWN";
    }
}
void recovery_mp_print_status(void)
{
    struct mp_calibration_driver_state state;
    int slot = recovery_mp_current_slot();
    int fw1_valid = recovery_mp_fw1_valid();
    int driver0 = rltk_wlan_running(0), driver1 = rltk_wlan_running(1);
    int promisc = is_promisc_enabled(), busy = recovery_ota_is_busy();
    int ready = phase == RECOVERY_MP && slot == 2 && fw1_valid == 0 &&
                SystemCoreClock == 300000000U && wifi_mode == RTW_MODE_STA &&
                driver0 && !driver1 && !promisc && !busy && mp_calibration_is_ready();
    mp_calibration_get_driver_state(&state);
    /* ready同时验证单STA环境和原厂mp_start状态，不表示校准通过。
     * 上位机必须查询本次启动的新回复，不能把旧READY/#缓存当作当前状态。 */
    rt_printf("[REC_MP_STATUS] phase=%s ready=%d slot=%d fw1_valid=%d cpu_hz=%lu mode=%d driver=%d/%d promisc=%d ota_busy=%d mp_mode=%u fw_state=0x%08x test_state=%u\r\n",
              recovery_mp_phase_name(), ready, slot, fw1_valid,
              (unsigned long)SystemCoreClock, (int)wifi_mode, driver0, driver1, promisc, busy,
              state.mode, state.fw_state, state.test_state);
}
