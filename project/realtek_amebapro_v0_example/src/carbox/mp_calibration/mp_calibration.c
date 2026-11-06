/*
 * RTL8715 MP 校准接入代码。
 * 基于 SDK 的 wifi_on/off、log_service.c 的 iwpriv 路由组合；
 * 不是反编译恢复的原厂完整源码。RF/MP 算法仍由 lib_wlan_mp.a 实现。
 * 不重复初始化 DRAM：调用点位于现有 firmware 的 CINIT 和 RTOS 启动之后。
 */
#include <autoconf.h>
#include <wifi_conf.h>
#include <wifi_util.h>
#include <wlan_intf.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "mp_calibration.h"

#ifndef CONFIG_MP_INCLUDED
#error "MP integration requires CONFIG_MP_INCLUDED and matching lib_wlan_mp.a"
#endif

/* 单任务契约避免在耗时 ioctl 外层持有全局 WLAN mutex。
 * 固定缓冲区避免占用约 2KB 的调用任务栈；因此接口不可重入。
 */
static char command_buffer[MP_CALIBRATION_COMMAND_MAX + 1];
static int owns_wifi;
static int commands_ready;
extern rtw_mode_t wifi_mode;
extern int is_promisc_enabled(void);

/* 原厂cab792c3...库的只读布局，由rtw_mp_set/mp_join/recv_func_prehandle核对。
 * Makefile校验完整库SHA256后才允许编译；换库必须重新核对，不能只改宏。
 * 不在这里写MP位：必须让原厂mp_start完成HAL、MP adapter和join初始化。
 * 生命周期由启动任务交接给console任务，禁止并行wifi_off/释放adapter。 */
#ifndef CARBOX_MP_DRIVER_LAYOUT_20261106
#error "Verify the vendor MP library layout before enabling MP state reads"
#endif
struct mp_wlan_entry {
    void *netdev;
    uint32_t reserved[3];
    unsigned char flag;
    unsigned char reserved2[11];
};
extern struct mp_wlan_entry rltk_wlan_info[2];
#ifndef MP_CALIBRATION_HOST_TEST
typedef char mp_layout_pointer_size[(sizeof(void *) == 4) ? 1 : -1];
typedef char mp_layout_entry_size[(sizeof(struct mp_wlan_entry) == 28) ? 1 : -1];
typedef char mp_layout_flag_offset[(offsetof(struct mp_wlan_entry, flag) == 16) ? 1 : -1];
#endif

int mp_calibration_get_driver_state(struct mp_calibration_driver_state *state)
{
    unsigned char *dev, *priv, *adapter;
    if (!state) return MP_CALIBRATION_BAD_COMMAND;
    memset(state, 0, sizeof(*state));
    if (!owns_wifi || !rltk_wlan_running(0) || !rltk_wlan_info[0].flag)
        return MP_CALIBRATION_NOT_READY;
    dev = rltk_wlan_info[0].netdev;
    if (!dev) return MP_CALIBRATION_DRIVER_STATE;
    memcpy(&priv, dev + 16, sizeof(priv));
    if (!priv) return MP_CALIBRATION_DRIVER_STATE;
    memcpy(&adapter, priv, sizeof(adapter));
    if (!adapter) return MP_CALIBRATION_DRIVER_STATE;
    state->mode = *(volatile unsigned char *)(adapter + 0x18a7);
    state->fw_state = *(volatile uint32_t *)(adapter + 0x10);
    state->test_state = *(volatile uint32_t *)(adapter + 0x31bc);
    return 0;
}

static int mp_environment_ready(void)
{
    return commands_ready && owns_wifi && rltk_wlan_running(0) &&
           !rltk_wlan_running(1) && wifi_mode == RTW_MODE_STA &&
           !is_promisc_enabled();
}

int mp_calibration_is_ready(void)
{
    struct mp_calibration_driver_state state;
    return mp_environment_ready() && mp_calibration_get_driver_state(&state) == 0 &&
           state.mode == 1 && (state.fw_state & 0x10000U) != 0 && state.test_state != 0;
}

int mp_calibration_start(void)
{
    struct mp_calibration_driver_state state;
    char cmd[] = "mp_start";
    int ret;
    if (!mp_environment_ready()) return MP_CALIBRATION_NOT_READY;
    /* 仪器可能重复发送mp_start。已有完整状态时不重新初始化、不清RX计数。 */
    if (mp_calibration_is_ready()) return 0;
    ret = wext_private_command("wlan0", cmd, 1);
    mp_calibration_get_driver_state(&state);
    printf("[MP_INIT] mp_start_ret=%d mp_mode=%u fw_state=0x%08x test_state=%u ready=%d\r\n",
           ret, state.mode, state.fw_state, state.test_state, mp_calibration_is_ready());
    /* 原厂部分失败分支仍返回0，必须以实际MP状态复核，不能仅检查ioctl返回值。 */
    if (ret < 0) return ret;
    return mp_calibration_is_ready() ? 0 : MP_CALIBRATION_DRIVER_STATE;
}

int mp_calibration_shutdown(void)
{
    int ret;
    commands_ready = 0;
    if (!owns_wifi)
        return 0;
    ret = wifi_off();
    if (ret < 0)
        return ret;
    /* 本 SDK 的 wifi_off 在等待超时后也可能返回 0，必须核对实际状态。 */
    if (rltk_wlan_running(0) || rltk_wlan_running(1))
        return MP_CALIBRATION_WIFI_STATE;
    owns_wifi = 0;
    return 0;
}

int mp_calibration_prepare(void)
{
    int ret;
    /* 不接管已运行的 Wi-Fi，避免 STA_AP 被 wifi_on 的提前返回保留下来。 */
    if (owns_wifi || rltk_wlan_running(0) || rltk_wlan_running(1))
        return MP_CALIBRATION_BUSY;
    commands_ready = 0;
    owns_wifi = 1; /* 初始化中途失败也需要清理；清理失败时保留所有权供重试。 */
    ret = wifi_on(RTW_MODE_STA);
    if (ret != 0 || !rltk_wlan_running(0) || rltk_wlan_running(1)) {
        int cleanup = mp_calibration_shutdown();
        if (cleanup < 0)
            return cleanup;
        return ret < 0 ? ret : MP_CALIBRATION_WIFI_STATE;
    }
    commands_ready = 1;
    return 0;
}

static int line_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int mp_calibration_command(const char *line, int show_response)
{
    size_t n = 0, first, last, i;
    char *cmd;
    if (!commands_ready)
        return MP_CALIBRATION_NOT_READY;
    if (!rltk_wlan_running(0) || rltk_wlan_running(1) || wifi_mode != RTW_MODE_STA || is_promisc_enabled()) {
        commands_ready = 0;
        return MP_CALIBRATION_NOT_READY;
    }
    if (!line)
        return MP_CALIBRATION_BAD_COMMAND;
    while (n <= MP_CALIBRATION_COMMAND_MAX && line[n])
        ++n;
    if (n > MP_CALIBRATION_COMMAND_MAX)
        return MP_CALIBRATION_BAD_COMMAND;
    first = 0;
    last = n;
    while (first < last && line_space((unsigned char)line[first]))
        ++first;
    while (last > first && line_space((unsigned char)line[last - 1]))
        --last;
    if (first == last)
        return MP_CALIBRATION_BAD_COMMAND;
    /* 禁止把多行合并为一次 ioctl；参数中的空格、逗号、等号保持不变。 */
    for (i = first; i < last; ++i) {
        unsigned char c = (unsigned char)line[i];
        if ((c < 0x20 && c != '\t') || c > 0x7e)
            return MP_CALIBRATION_BAD_COMMAND;
    }
    memcpy(command_buffer, line + first, last - first);
    command_buffer[last - first] = '\0';
    cmd = command_buffer;
    if (strncmp(cmd, "iwpriv", 6) == 0) {
        if (cmd[6] != ' ' && cmd[6] != '\t')
            return MP_CALIBRATION_BAD_COMMAND;
        cmd += 6;
        while (*cmd == ' ' || *cmd == '\t')
            ++cmd;
        if (!*cmd)
            return MP_CALIBRATION_BAD_COMMAND;
    }
    if (strcmp(cmd, "mp_start") == 0)
        return mp_calibration_start();
    /* mp_stop后禁止继续MP射频操作；保留原厂efuse/config等非MP命令的收尾流程。 */
    if (strncmp(cmd, "mp_", 3) == 0 && !mp_calibration_is_ready() && strcmp(cmd, "mp_query") != 0 &&
        strcmp(cmd, "mp_stop") != 0 && strcmp(cmd, "mp_ctx stop") != 0 &&
        strcmp(cmd, "mp_arx stop") != 0)
        return MP_CALIBRATION_NOT_READY;
    /* 与 SDK log_service.c 相同：所有 MP 命令固定交给 wlan0。 */
    return wext_private_command("wlan0", cmd, show_response != 0);
}
