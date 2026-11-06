#ifndef CUSTOMER_MP_CALIBRATION_H
#define CUSTOMER_MP_CALIBRATION_H

#ifdef __cplusplus
extern "C" {
#endif

/* 包含可选的 iwpriv 前缀及行尾；可容纳 512 字节校准表的十六进制命令。 */
#define MP_CALIBRATION_COMMAND_MAX 2048
#define MP_CALIBRATION_BUSY        (-1000)
#define MP_CALIBRATION_NOT_READY   (-1001)
#define MP_CALIBRATION_BAD_COMMAND (-1002)
#define MP_CALIBRATION_WIFI_STATE  (-1003)
#define MP_CALIBRATION_DRIVER_STATE (-1004)

struct mp_calibration_driver_state {
    unsigned int mode;
    unsigned int fw_state;
    unsigned int test_state;
};
/* 只读快照；布局绑定指定原厂MP库，不能用于其它版本的driver。 */
int mp_calibration_get_driver_state(struct mp_calibration_driver_state *state);

/*
 * 以下接口只能由同一个任务顺序调用，不能在 ISR 或 Wi-Fi 回调中调用。
 * 调用者必须独占 Wi-Fi 生命周期：停止 CarPlay/连接重试/AP/网络业务后，
 * 完成 wifi_off()，再调用 prepare。推荐在开机生产模式分支调用。
 * prepare 只启动单 STA，不自动发送 mp_start、不连接 AP、不启动 DHCP。
 */
int mp_calibration_prepare(void);

/* prepare之后调用：执行原厂mp_start并核对RX所需状态。已启动时不重置session。
 * 不启动TX/RX发包、不保存校准结果、不写efuse。 */
int mp_calibration_start(void);

/* 只读检查：单STA环境和原厂MP状态均有效；不表示射频测量通过。 */
int mp_calibration_is_ready(void);

/*
 * 接受 "iwpriv mp_start" 或 "mp_start" 等单行命令，允许末尾 CR/LF。
 * 原样保留参数及返回值；show_response=1 使用 SDK 原有输出。
 * 返回 >=0 仅表示 ioctl 未报错，不代表 RF 测量合格或校准数据已保存。
 * 某些驱动分支会吞掉内部失败，调用者必须继续核对原厂回复/测量结果。
 * 本接口不添加串口提示符，不接收 UART，不自动追加下一条校准命令。
 */
int mp_calibration_command(const char *line, int show_response);

/*
 * 关闭本模块启动的 Wi-Fi；失败后禁止继续提交命令，可再次调用 shutdown。
 * 不自动恢复 CarPlay、STA+AP，不替代上位机正常的停止 TX/RX/保存结果流程。
 * 生产测试结束后建议重启回正常固件入口。
 */
int mp_calibration_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
