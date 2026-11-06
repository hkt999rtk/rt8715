#ifndef RECOVERY_MP_H
#define RECOVERY_MP_H
/* FW2 启动时、任何 Wi-Fi/AP/OTA 任务建立之前调用。0=OTA，1=MP，负数=故障。 */
int recovery_mp_boot_select(void);
/* 只在上面选择MP后调用；完成单STA、时钟、promisc和原厂mp_start状态检查。 */
int recovery_mp_start(void);
/* MP=4：写入并核对一次性标记，重启同一FW2。不得在中断或其他任务调用。 */
int recovery_mp_request(void);
int recovery_mp_is_active(void);
/* 只有OTA分支允许建立网络业务；MP、切换及故障状态均禁止AP/STA_OTA。 */
int recovery_mp_ota_allowed(void);
/* 只读诊断：ready验证厂商MP状态，不等于射频校准成功。 */
const char *recovery_mp_phase_name(void);
void recovery_mp_print_status(void);
/* MP/故障模式禁止普通联网命令重新启动 AP；由 log_service 单任务调用。 */
int recovery_mp_allow_console(const char *cmd);
int recovery_ota_is_busy(void);
#endif
