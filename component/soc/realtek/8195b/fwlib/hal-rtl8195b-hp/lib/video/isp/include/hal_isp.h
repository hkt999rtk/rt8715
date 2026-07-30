/**************************************************************************//**
 * @file      hal_isp.h
 * @brief
 * @version   V1.00
 * @date      2017-4-27 13:55:10
 *
 * @note
 *
 ******************************************************************************
 *
 * Copyright(c) 2007 - 2017 Realtek Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ******************************************************************************/

#ifndef _HAL_ISP_H_
#define _HAL_ISP_H_

#ifdef __cplusplus
extern "C"{
#endif

#include "cmsis.h"

/** 
 * @addtogroup hs_hal_isp ISP
 * @{
 * @brief The ISP HAL APIs.
 */


#ifdef __PXP_RUN__


#if 0
#ifdef _PRINT_OFF
static int off_printf(const char *fmt, ...)
{
  fmt =fmt;
  return 0;
}
#define DBG_ISP_INFO off_printf
#define DBG_ISP_ERR off_printf
#else
    #define DBG_ISP_INFO rtl_printf
    #define DBG_ISP_ERR rtl_printf
#endif
#endif
#endif

#define ISP_ISR_NUM 12
#define MCU_ISR_NUM 22
#define ISP_INT_START_BASE MCU_ISR_NUM

//#define SENSOR_W 1280    
//#define SENSOR_H 720

#if 0
#define SSOR_RAW_NUM 5
#define ISP_RUN_FRAME 1

#define TRANSFR_TYPE 0x4A  ///bit0: jpeg, bit1-4: isp0-3 bit6: ccs/8051 switch (u8)transfer_type;
#define SSOR_FORMAT 6 //yuv422: 3'b101, raw10: 3'b001, raw8: 3'b110
#endif

#define ISP_FORMAT_TYPE_YUV422_SEMIPLANAR 0x02
#define ISP_FORMAT_TYPE_YUV420_SEMIPLANAR 0x01

//DBG CMD
#define ISP_CMDCODE_XMEM_R		0x81
#define ISP_CMDCODE_XMEM_W		0x01
#define ISP_CMDCODE_I2C_R			0x82
#define ISP_CMDCODE_I2C_W			0x02
#define ISP_CMDCODE_SNR_R			0x83
#define ISP_CMDCODE_SNR_W			0x03

//GET DEVICE INFORMATION CMD
#define ISP_CMDCODE_GET_DEVICE_DES		((0x01 << 8) | 0x81)
#define ISP_CMDCODE_GET_FRAME_INTERVAL_DES	((0x01 << 8) | 0x82)
#define ISP_CMDCODE_GET_VS_FMT_DES		((0x01 << 8) | 0x83)
#define ISP_CMDCODE_GET_ISP_PROCESS_DES		((0x01 << 8) | 0x84)
#define ISP_CMDCODE_GET_CAMERA_DES		((0x01 << 8) | 0x85)
#define ISP_CMDCODE_GET_RTK_EXT_CTL_DES		((0x01 << 8) | 0x86)
#define ISP_CMDCODE_GET_HCLK_CFG_DES		((0x01 << 8) | 0x87)
#define ISP_CMDCODE_GET_FW_VERSION		((0x01 << 8) | 0x88)
#define ISP_CMDCODE_GET_SNR_POWER		((0x01 << 8) | 0x89)
#define ISP_CMDCODE_GET_API_VERSION		((0x01 << 8) | 0x8a)
#define ISP_CMDCODE_GET_SNR_POWER_SEQ	((0x01 << 8) | 0x8b)

//PREVIEW CMD
#define ISP_CMDCODE_SET_FPS			((0x02 << 8) | 0x01)
#define ISP_CMDCODE_START_PREVIEW	((0x02 << 8) | 0x02)
#define ISP_CMDCODE_STOP_PREVIEW		((0x02 << 8) | 0x03)
#define ISP_CMDCODE_OUTPUT_FMT		((0x02 << 8) | 0x04)
#define ISP_CMDCODE_PAUSE_VIDEO		((0x02 << 8) | 0x05)
#define ISP_CMDCODE_RESUME_VIDEO		((0x02 << 8) | 0x06)
#define ISP_CMDCODE_SET_FORMAT		((0x02 << 8) | 0x07)
#define ISP_CMDCODE_SNR_PWRON_BEF	((0x02 << 8) | 0x09)

//ISP CONTROL CMD
#define ISP_CMDCODE_CT_CMD_SET		((0x03 << 8) | 0x01)
#define ISP_CMDCODE_CT_CMD_GET		((0x03 << 8) | 0x81)
#define ISP_CMDCODE_CT_CMD_GET_MIN	((0x03 << 8) | 0x82)
#define ISP_CMDCODE_CT_CMD_GET_MAX	((0x03 << 8) | 0x83)
#define ISP_CMDCODE_CT_CMD_GET_RES	((0x03 << 8) | 0x84)
#define ISP_CMDCODE_CT_CMD_GET_LEN	((0x03 << 8) | 0x85)
#define ISP_CMDCODE_CT_CMD_GET_INFO	((0x03 << 8) | 0x86)
#define ISP_CMDCODE_CT_CMD_GET_DEF	((0x03 << 8) | 0x87)

//CAMERA CONTROL CMD
#define ISP_CMDCODE_CAMERA_CT_CMD_SET	((0x04 << 8) | 0x01)
#define ISP_CMDCODE_CAMERA_CT_CMD_GET	((0x04 << 8) | 0x81)
#define ISP_CMDCODE_CAMERA_CT_CMD_GET_MIN	((0x04 << 8) | 0x82)
#define ISP_CMDCODE_CAMERA_CT_CMD_GET_MAX	((0x04 << 8) | 0x83)
#define ISP_CMDCODE_CAMERA_CT_CMD_GET_RES	((0x04 << 8) | 0x84)
#define ISP_CMDCODE_CAMERA_CT_CMD_GET_LEN	((0x04 << 8) | 0x85)
#define ISP_CMDCODE_CAMERA_CT_CMD_GET_INFO	((0x04 << 8) | 0x86)
#define ISP_CMDCODE_CAMERA_CT_CMD_GET_DEF	((0x04 << 8) | 0x87)

//RTK EX CONTROL CMD
#define ISP_CMDCODE_RTK_EX_CT_CMD_SET	((0x05 << 8) | 0x01)
#define ISP_CMDCODE_RTK_EX_CT_CMD_GET	((0x05 << 8) | 0x81)
#define ISP_CMDCODE_RTK_EX_CT_CMD_GET_MIN	((0x05 << 8) | 0x82)
#define ISP_CMDCODE_RTK_EX_CT_CMD_GET_MAX	((0x05 << 8) | 0x83)
#define ISP_CMDCODE_RTK_EX_CT_CMD_GET_RES	((0x05 << 8) | 0x84)
#define ISP_CMDCODE_RTK_EX_CT_CMD_GET_LEN	((0x05 << 8) | 0x85)
#define ISP_CMDCODE_RTK_EX_CT_CMD_GET_INFO	((0x05 << 8) | 0x86)
#define ISP_CMDCODE_RTK_EX_CT_CMD_GET_DEF	((0x05 << 8) | 0x87)

//GET COORDINATE INFO CMD
#define ISP_CMDCODE_GET_SNR_FMT				((0x06 << 8) | 0x81)
#define ISP_CMDCODE_GET_SCALE_COEFF			((0x06 << 8) | 0x82)
#define ISP_CMDCODE_GET_ZOOM_START			((0x06 << 8) | 0x83)

//MTD CMD
#define ISP_CMDCODE_SET_MTD_EN				((0x07 << 8) | 0x81)
#define ISP_CMDCODE_GET_MTD_EN				((0x07 << 8) | 0x81)
#define ISP_CMDCODE_SET_MTD_DATA_FMT		((0x07 << 8) | 0x02)
#define ISP_CMDCODE_GET_MTD_DATA_FMT		((0x07 << 8) | 0x82)
#define ISP_CMDCODE_SET_MTD_GRID_MASK		((0x07 << 8) | 0x03)
#define ISP_CMDCODE_GET_MTD_GRID_MASK		((0x07 << 8) | 0x83)
#define ISP_CMDCODE_SET_MTD_GRID				((0x07 << 8) | 0x04)
#define ISP_CMDCODE_GET_MTD_GRID				((0x07 << 8) | 0x84)
#define ISP_CMDCODE_SET_MTD_RECT				((0x07 << 8) | 0x05)
#define ISP_CMDCODE_GET_MTD_RECT				((0x07 << 8) | 0x85)
#define ISP_CMDCODE_SET_MTD_GRID_ABSOLUTE	((0x07 << 8) | 0x06)
#define ISP_CMDCODE_GET_MTD_GRID_ABSOLUTE	((0x07 << 8) | 0x86)
#define ISP_CMDCODE_SET_MTD_RECT_ABSOLUTE	((0x07 << 8) | 0x07)
#define ISP_CMDCODE_GET_MTD_RECT_ABSOLUTE	((0x07 << 8) | 0x87)
#define ISP_CMDCODE_SET_MTD_SENSITIVITY		((0x07 << 8) | 0x08)
#define ISP_CMDCODE_GET_MTD_SENSITIVITY		((0x07 << 8) | 0x88)
#define ISP_CMDCODE_SET_MTD_PERCENTAGE		((0x07 << 8) | 0x09)
#define ISP_CMDCODE_GET_MTD_PERCENTAGE		((0x07 << 8) | 0x89)
#define ISP_CMDCODE_SET_MTD_FRM_INT			((0x07 << 8) | 0x0a)
#define ISP_CMDCODE_GET_MTD_FRM_INT			((0x07 << 8) | 0x8a)
#define ISP_CMDCODE_SET_MTD_GRID_RES			((0x07 << 8) | 0x0b)
#define ISP_CMDCODE_GET_MTD_GRID_RES			((0x07 << 8) | 0x8b)
#define ISP_CMDCODE_SET_MTD_RECT_RES			((0x07 << 8) | 0x0c)
#define ISP_CMDCODE_GET_MTD_RECT_RES			((0x07 << 8) | 0x8c)
#define ISP_CMDCODE_GET_MTD_RESULT			((0x07 << 8) | 0x8d)

//OSD CMD
#define ISP_CMDCODE_SET_OSD_LIB_CHAR_ATTR	((0x08 << 8) | 0x01)
#define ISP_CMDCODE_GET_OSD_LIB_CHAR_ATTR	((0x08 << 8) | 0x81)
#define ISP_CMDCODE_SET_OSD_CHINESE_ADDR	((0x08 << 8) | 0x02)
#define ISP_CMDCODE_GET_OSD_CHINESE_ADDR	((0x08 << 8) | 0x82)
#define ISP_CMDCODE_SET_OSD_TIME_DATE_EN	((0x08 << 8) | 0x03)
#define ISP_CMDCODE_GET_OSD_TIME_DATE_EN	((0x08 << 8) | 0x83)
#define ISP_CMDCODE_SET_OSD_TIME_DATE_DISP_CFG	((0x08 << 8) | 0x04)
#define ISP_CMDCODE_GET_OSD_TIME_DATE_DISP_CFG	((0x08 << 8) | 0x84)
#define ISP_CMDCODE_SET_OSD_TIME_FMT			((0x08 << 8) | 0x05)
#define ISP_CMDCODE_GET_OSD_TIME_FMT		((0x08 << 8) | 0x85)
#define ISP_CMDCODE_SET_OSD_CUR_TIME			((0x08 << 8) | 0x06)
#define ISP_CMDCODE_SET_OSD_CUR_DATE		((0x08 << 8) | 0x07)
#define ISP_CMDCODE_SET_OSD_NEXT_DATE		((0x08 << 8) | 0x08)
#define ISP_CMDCODE_SET_OSD_BLK_EN			((0x08 << 8) | 0x09)
#define ISP_CMDCODE_GET_OSD_BLK_EN			((0x08 << 8) | 0x89)
#define ISP_CMDCODE_SET_OSD_BLK_ATTR			((0x08 << 8) | 0x0a)
#define ISP_CMDCODE_GET_OSD_BLK_ATTR			((0x08 << 8) | 0x8a)
#define ISP_CMDCODE_WRITE_OSD_MEM			((0x08 << 8) | 0x0b)
#define ISP_CMDCODE_READ_OSD_MEM				((0x08 << 8) | 0x8b)
#define ISP_CMDCODE_GET_OSD_MEM_SIZE		((0x08 << 8) | 0x8c)

//PRIVATE MASK CMD
#define ISP_CMDCODE_SET_MASK_EN				((0x09 << 8) | 0x01)
#define ISP_CMDCODE_GET_MASK_EN				((0x09 << 8) | 0x81)
#define ISP_CMDCODE_SET_MASK_COLOR			((0x09 << 8) | 0x02)
#define ISP_CMDCODE_GET_MASK_COLOR			((0x09 << 8) | 0x82)
#define ISP_CMDCODE_SET_MASK_GRID_BMP		((0x09 << 8) | 0x03)
#define ISP_CMDCODE_GET_MASK_GRID_BMP		((0x09 << 8) | 0x83)
#define ISP_CMDCODE_SET_MASK_GRID			((0x09 << 8) | 0x04)
#define ISP_CMDCODE_GET_MASK_GRID			((0x09 << 8) | 0x84)
#define ISP_CMDCODE_SET_MASK_RECT			((0x09 << 8) | 0x05)
#define ISP_CMDCODE_GET_MASK_RECT			((0x09 << 8) | 0x85)
#define ISP_CMDCODE_SET_MASK_GRID_ABSOLUTE	((0x09 << 8) | 0x06)
#define ISP_CMDCODE_GET_MASK_GRID_ABSOLUTE	((0x09 << 8) | 0x86)
#define ISP_CMDCODE_SET_MASK_RECT_ABSOLUTE	((0x09 << 8) | 0x07)
#define ISP_CMDCODE_GET_MASK_RECT_ABSOLUTE	((0x09 << 8) | 0x87)
#define ISP_CMDCODE_SET_MASK_GRID_RES		((0x09 << 8) | 0x08)
#define ISP_CMDCODE_GET_MASK_GRID_RES		((0x09 << 8) | 0x88)
#define ISP_CMDCODE_SET_MASK_RECT_RES		((0x09 << 8) | 0x09)
#define ISP_CMDCODE_GET_MASK_RECT_RES		((0x09 << 8) | 0x89)

//OTHER ISP CMD
#define ISP_CMDCODE_SET_ISP_FUNC				((0x0a << 8) | 0x01)
#define ISP_CMDCODE_GET_ISP_FUNC				((0x0a << 8) | 0x81)
#define ISP_CMDCODE_SET_GRAY_MODE			((0x0a << 8) | 0x02)
#define ISP_CMDCODE_GET_GRAY_MODE			((0x0a << 8) | 0x82)
#define ISP_CMDCODE_GET_IQ_TBL_ATTR			((0x0a << 8) | 0x83)
#define ISP_CMDCODE_GET_3A_VER				((0x0a << 8) | 0x84)
#define ISP_CMDCODE_SET_WDR_MODE			((0x0a << 8) | 0x05)
#define ISP_CMDCODE_GET_WDR_MODE			((0x0a << 8) | 0x85)
#define ISP_CMDCODE_SET_WDR_LV				((0x0a << 8) | 0x06)
#define ISP_CMDCODE_GET_WDR_LV				((0x0a << 8) | 0x86)
#define ISP_CMDCODE_SET_IR_MODE				((0x0a << 8) | 0x08)
#define ISP_CMDCODE_GET_IR_MODE				((0x0a << 8) | 0x88)
#define ISP_CMDCODE_SET_AE_GAIN				((0x0a << 8) | 0x09)
#define ISP_CMDCODE_GET_AE_GAIN				((0x0a << 8) | 0x89)
#define ISP_CMDCODE_SET_AE_WEIGHT			((0x0a << 8) | 0x0a)
#define ISP_CMDCODE_GET_AE_WEIGHT			((0x0a << 8) | 0x8a)
#define ISP_CMDCODE_GET_AE_STATIS			((0x0a << 8) | 0x8b)
#define ISP_CMDCODE_GET_AWB_STATIS			((0x0a << 8) | 0x8c)
#define ISP_CMDCODE_GET_AWB_CT_GAIN			((0x0a << 8) | 0x8d)
#define ISP_CMDCODE_SET_AE_TARGET			((0x0a << 8) | 0x0e)
#define ISP_CMDCODE_GET_AE_TARGET			((0x0a << 8) | 0x8e)
#define ISP_CMDCODE_SET_ISP_FRR				((0x0a << 8) | 0x0f)
#define ISP_CMDCODE_GET_ISP_FRR				((0x0a << 8) | 0x8f)
#define ISP_CMDCODE_SET_AWB_INIT_VALUE		((0x0a << 8) | 0x10)
#define ISP_CMDCODE_SET_AE_INIT_VALUE	((0x0a << 8) | 0x11)
#define ISP_CMDCODE_SET_AE_STAT_WIN	  ((0x0a << 8) | 0x12)
#define ISP_CMDCODE_GET_AE_STAT_WIN	  ((0x0a << 8) | 0x92)
#define ISP_CMDCODE_SET_DYN_FPS	      ((0x0a << 8) | 0x15)
#define ISP_CMDCODE_GET_DYN_FPS	      ((0x0a << 8) | 0x95)
#define ISP_CMDCODE_SET_FMT_ZOOM	    ((0x0a << 8) | 0x2c)
#define ISP_CMDCODE_GET_FMT_ZOOM	    ((0x0a << 8) | 0xac)
#define ISP_CMDCODE_SET_AE_LIMIT	    ((0x0a << 8) | 0x2d)
#define ISP_CMDCODE_GET_AE_LIMIT	    ((0x0a << 8) | 0xad)
#define ISP_CMDCODE_SET_AE_FAST_STEP  ((0x0a << 8) | 0x2e)
#define ISP_CMDCODE_GET_AE_FAST_STEP  ((0x0a << 8) | 0xae)
#define ISP_CMDCODE_SET_AE_SLOW_STEP  ((0x0a << 8) | 0x2f)
#define ISP_CMDCODE_GET_AE_SLOW_STEP  ((0x0a << 8) | 0xaf)

//GPIO CMD
#define ISP_CMDCODE_GET_GPIO_USE_STATUS 		((0x0C << 8) | 0x81)
#define ISP_CMDCODE_SET_GPIO_DIR				((0x0C << 8) | 0x02)
#define ISP_CMDCODE_GET_GPIO_DIR				((0x0C << 8) | 0x82)
#define ISP_CMDCODE_SET_GPIO_VAL				((0x0C << 8) | 0x03)
#define ISP_CMDCODE_GET_GPIO_VAL				((0x0C << 8) | 0x83)

#define ISP_MAX_STREAM_NUM 3
#define ISP_MAX_SLOT_NUM 4
#define ISP_FRAME_HEADER_LENGTH 8

enum isp_interface {
    isp_interface_parallel = 0,
    isp_interface_mipi = 1
};

enum isp_snr_freq {
    isps_freq_37_125 = 1,
    isps_freq_27 = 3,
    isps_freq_24 = 4,
    isps_freq_12 = 5
};

enum isp_pin_sel {
    isp_pin_sel_s0 = 0,
    isp_pin_sel_s1 = 1
};

enum cpu_lock_mcu_mask {
    cpu_lock_mcu_request = 0,
    cpu_lock_mcu_done = 1,
    cpu_lock_mcu_release = 2,
    cpu_lock_mcu_cache_miss = 3,
};


struct isp_cmd_stru {
    u16 cmdcode;
    u8 index;
    u8 length;
    u16 param;
    u16 addr;
    u8 *buf;
    u32 error_type;
};

typedef void (*isp_callback_t)(void *);
typedef int (*isp_cmd_wait_done)(void);

typedef struct hal_isp_interrupt_s {

    isp_callback_t irq_callback[MCU_ISR_NUM+ISP_ISR_NUM];
    void *irq_cb_para[MCU_ISR_NUM+ISP_ISR_NUM];

} hal_isp_interrupt_t;


hal_status_t hal_isp_pin_ctrl(uint8_t isp_idx, BOOL en);
hal_status_t hal_isp_interface_ctrl(u32 interface, BOOL en);

#ifdef __PXP_RUN__
void hal_isp_snr_cfg(u16 sensor_w, u16 sensor_h, u16 raw_num, u16 run_frame, u32 snr_format);
void hal_isp_snr_start(void);
#endif

u32 hal_isp_videosys_setting(BOOL en, u32 interface, u32 isp_snr_freq);

u32 hal_isp_get_cpu_lock_mcu(u32 selector);

void hal_isp_en_isp_interrupt(BOOL en, int id, isp_callback_t callback, void *para);
void hal_isp_en_mcu_interrupt(BOOL en, int id, isp_callback_t callback, void *para);
void hal_isp_irq_reg(void);
u32 hal_isp_en_mcu(int en, u32 mcu_fw_addr, isp_cmd_wait_done fun_wait_cmd_done, isp_cmd_wait_done fun_wait_cpu_lock_mcu);

int hal_isp_config_isp_header(u32 isp_header_addr);
int hal_isp_config_isp_buffer(u8 streamid);
u8 hal_isp_get_hw_slow_num(u8 streamid);
int hal_isp_config_hw_slot_num(u8 streamid, u8 slot_num);
void hal_isp_config_yuv_addr(u8 streamid, u8 slot_id, u32 y_addr, u32 uv_addr);
void hal_isp_release_slot(u8 streamid, u8 slot_id);
u32 hal_isp_get_yuv_frame_buf_status(u8 streamid);
void hal_isp_control(u8 streamid, BOOL en);
void hal_isp_reset_stream(u8 streamid);
u32 hal_isp_get_yuv_cur_frame_index_offset(u8 streamid);

u32 hal_isp_get_mcu_cmd_status(void);
int hal_isp_check_mcu_cmd_status(u32 status);
int hal_isp_check_mcu_cmd_result(u32 status);
u32 hal_isp_get_mcu_cmd_error(u32 status);

int hal_isp_exec_command(struct isp_cmd_stru *cmd, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_set_format_cmd(u8 streamid, u16 format, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_set_isp_output_format_cmd(u8 streamid, u16 format, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_set_fps_cmd(u8 streamid, u32 fps, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_start_preview_cmd(u8 streamid, u32 yuv_width, u32 yuv_height, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_stop_preview_cmd(u8 streamid, u8* buf, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_xmem_cmd_r(u16 addr, u8 length, u8* buf, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_xmem_cmd_w(u16 addr, u8 length, u8* buf, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_control_cmd_r(u32 cmdcode, u8 index, u8 length, u8* buf, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_control_cmd_w(u32 cmdcode, u8 index, u8 length, u8* buf, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_device_info_r(u32 cmdcode, u8 index, u8 length, u8* buf, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_other_cmd_w(u32 cmdcode, u8 length, u8* buf, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_other_cmd_r(u32 cmdcode, u8 index, u8 length, u8* buf, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_snr_cmd_r(u16 addr, u8 length, u8* buf, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_snr_cmd_w(u16 addr, u8 length, u8* buf, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
int hal_isp_gpio_status(u32 cmdcode, u8 index, u8 length, u8* buf, isp_cmd_wait_done fun_pre_wait, isp_cmd_wait_done fun_post_wait);
u32 hal_isp_check_reset_stream(u8 streamid);
u32 hal_isp_check_reset_stream_all(void);
void hal_isp_irq_disable(void);

hal_status_t hal_isp_enable_line_buffer_signal(u8 stream_id, u8 line_height, bool enable);



/** @} */ /* End of group hs_hal_isp */

#ifdef  __cplusplus
}
#endif

#endif    // end of #ifndef _HAL_ISP_H_


