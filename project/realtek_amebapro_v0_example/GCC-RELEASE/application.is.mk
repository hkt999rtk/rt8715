
# Initialize tool chain
# -------------------------------------------------------------------
#ARM_GCC_TOOLCHAIN = ../../../tools/arm-none-eabi-gcc/gcc6/bin/

AMEBAPRO_TOOLDIR	= ../../../component/soc/realtek/8195b/misc/iar_utility
FLASH_TOOLDIR = ../../../component/soc/realtek/8195b/misc/gcc_utility
UTILITYDIR = gnu_utility
OS := $(shell uname)

ifeq ($(findstring CYGWIN, $(OS)), CYGWIN)
	ARM_GCC_TOOLCHAIN = ../../../tools/arm-none-eabi-gcc/asdk/cygwin/newlib/bin
	ELF2BIN = $(AMEBAPRO_TOOLDIR)/elf2bin.exe
	CHKSUM = $(AMEBAPRO_TOOLDIR)/checksum.exe
endif

ifeq ($(findstring MINGW32, $(OS)), MINGW32)
	ARM_GCC_TOOLCHAIN = ../../../tools/arm-none-eabi-gcc/asdk/mingw32/newlib/bin
	ELF2BIN = $(AMEBAPRO_TOOLDIR)/elf2bin.exe
	CHKSUM = $(AMEBAPRO_TOOLDIR)/checksum.exe
endif

ifeq ($(findstring Linux, $(OS)), Linux)
	ARM_GCC_TOOLCHAIN = ../../../tools/arm-none-eabi-gcc/asdk/linux/newlib/bin
	ELF2BIN = $(AMEBAPRO_TOOLDIR)/elf2bin.linux	
	CHKSUM = $(AMEBAPRO_TOOLDIR)/checksum.linux
endif

#CROSS_COMPILE = $(ARM_GCC_TOOLCHAIN)/arm-none-eabi-
CROSS_COMPILE = $(ARM_GCC_TOOLCHAIN)/arm-none-eabi-

# Compilation tools
AR = $(CROSS_COMPILE)ar
CC = $(CROSS_COMPILE)gcc
AS = $(CROSS_COMPILE)as
NM = $(CROSS_COMPILE)nm
LD = $(CROSS_COMPILE)gcc
GDB = $(CROSS_COMPILE)gdb
OBJCOPY = $(CROSS_COMPILE)objcopy
OBJDUMP = $(CROSS_COMPILE)objdump

OS := $(shell uname)

LDSCRIPT := ./rtl8195bhp_ram_is.ld
#LDSCRIPT := ./rtl8195bhp_ram_mps2.ld


# Initialize target name and target object files
# -------------------------------------------------------------------

ifeq ($(CARBOX_SKIP_IMAGE),1)
all: prebuild build_info application sensor
else
all: prebuild build_info application sensor manipulate_images
endif

mp:  prebuild build_info application manipulate_images

TARGET=application_is
VFSDIR = rootfs
OBJ_DIR=$(TARGET)/Debug/obj
BIN_DIR=$(TARGET)/Debug/bin
INFO_DIR=$(TARGET)/Debug/info
BOOT_BIN_DIR=bootloader/Debug/bin
VFSTOOL  = python3 $(UTILITYDIR)/vfs.py

FATFS_SECTORS = 2
ROMIMG = 

# Include folder list
# -------------------------------------------------------------------

INCLUDES =
INCLUDES += -I../inc
INCLUDES += -I../src/eval/sensor_board_v1
INCLUDES += -I../../../component/soc/realtek/8195b/cmsis/rtl8195b-hp/include
INCLUDES += -I../../../component/common/drivers/sdio/realtek/sdio_host/src
INCLUDES += -I../../../component/soc/realtek/8195b/cmsis/cmsis-core/include
INCLUDES += -I../../../component/soc/realtek/8195b/cmsis/rtl8195b-hp/lib/include
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/include
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/lib/include
INCLUDES += -I../../../component/soc/realtek/8195b/app/rtl_printf/include
INCLUDES += -I../../../component/soc/realtek/8195b/app/shell
INCLUDES += -I../../../component/soc/realtek/8195b/app/stdio_port
INCLUDES += -I../../../component/os/freertos
INCLUDES += -I../../../component/os/freertos/freertos_v10.0.0/include
INCLUDES += -I../../../component/os/freertos/freertos_v10.0.0/portable/GCC/ARM_RTL8195B
INCLUDES += -I../../../component/os/freertos/freertos_v10.0.0/secure
# FreeRTOS-Plus-POSIX (required by BoxApp / CarPlay modules)
INCLUDES += -I../../../component/os/freertos/freertos_v10.0.0/portable/posix/lib/FreeRTOS-Plus-POSIX/include
INCLUDES += -I../../../component/os/freertos/freertos_v10.0.0/portable/posix/lib/FreeRTOS-Plus-POSIX/include/portable/realtek/rtl8195
INCLUDES += -I../../../component/os/freertos/freertos_v10.0.0/portable/posix/lib/include
INCLUDES += -I../../../component/os/freertos/freertos_v10.0.0/portable/posix/lib/include/private
INCLUDES += -I../../../component/os/os_dep/include
INCLUDES += -I../../../component/soc/realtek/8195b/misc/utilities/include
INCLUDES += -I../../../component/soc/realtek/8195b/misc/platform
INCLUDES += -I../../../component/soc/realtek/8195b/misc/driver
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/include/usb_otg
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/lib/include/usb_otg
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/lib/video/video/include
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/lib/video/lcdc/include
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/lib/video/isp/include
INCLUDES += -I../../../component/common/api
INCLUDES += -I../../../component/common/api/at_cmd
INCLUDES += -I../../../component/common/api/platform
INCLUDES += -I../../../component/common/api/wifi
INCLUDES += -I../../../component/common/api/network/include
INCLUDES += -I../../../component/common/audio/haac
INCLUDES += -Ithird_party/fdk-aac/libAACdec/include
INCLUDES += -Ithird_party/fdk-aac/libSYS/include
INCLUDES += -I../../../component/common/audio/hmp3
INCLUDES += -I../../../component/common/audio/speex
INCLUDES += -I../../../component/common/network/lwip/lwip_v2.1.2/src/include
INCLUDES += -I../../../component/common/network/lwip/lwip_v2.1.2/src/include/lwip
INCLUDES += -I../../../component/common/network/lwip/lwip_v2.1.2/port/realtek
INCLUDES += -I../../../component/common/network/lwip/lwip_v2.1.2/port/realtek/freertos
INCLUDES += -I../../../component/common/drivers/wlan/realtek/include
INCLUDES += -I../../../component/common/drivers/wlan/realtek/src/osdep
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/video/lcdc/include
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/video/isp/include
INCLUDES += -I../../../component/common/mbed/hal
INCLUDES += -I../../../component/common/mbed/hal_ext
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/include/usb_otg
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/video/video/include
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/video/lcdc/include
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/video/isp/include
INCLUDES += -I../../../component/soc/realtek/8195b/mbed-drivers/include
INCLUDES += -I../../../component/os/freertos
INCLUDES += -I../../../component/os/os_dep/include
INCLUDES += -I../../../component/common/media/mmfv2
INCLUDES += -I../../../component/common/media/rtp_codec
INCLUDES += -I../../../component/common/media/samples
INCLUDES += -I../../../component/common/network
INCLUDES += -I../../../component/common/example
INCLUDES += -I../../../component/common/example/qr_code_scanner
INCLUDES += -I../../../component/common/drivers/video/realtek/common
INCLUDES += -I../../../component/common/drivers/video/realtek/common/h264enc/inc
INCLUDES += -I../../../component/common/drivers/video/realtek/common/h264enc/source/common
INCLUDES += -I../../../component/common/drivers/video/realtek/common/h264enc/source/camstab
INCLUDES += -I../../../component/common/drivers/video/realtek/common/h264enc/source/h264
INCLUDES += -I../../../component/common/drivers/video/realtek/common
INCLUDES += -I../../../component/common/drivers/video/realtek/common/include
INCLUDES += -I../../../component/soc/realtek/8195b/cmsis/cmsis-dsp/include
INCLUDES += -I../../../component/common/audio/faac/libfaac
INCLUDES += -I../../../component/common/audio/faac/include
INCLUDES += -I../../../component/common/file_system/fatfs
INCLUDES += -I../../../component/common/file_system/fatfs/r0.14
INCLUDES += -I../../../component/common/file_system/littlefs/r2.41
INCLUDES += -I../../../component/common/drivers/sdio/realtek/sdio_host/inc
INCLUDES += -I../../../component/soc/realtek/8195b/cmsis/cmsis-core/include
INCLUDES += -I../../../component/soc/realtek/8195a/fwlib/rtl8195a
INCLUDES += -I../../../component/soc/realtek/8195a/misc/os
INCLUDES += -I../../../component/soc/realtek/8195b/mbed-hal/targets/hal/rtl8195bh
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/lib/video/enc/include
INCLUDES += -I../../../component/common/api/wifi/rtw_wpa_supplicant/src
INCLUDES += -I../../../component/soc/realtek/8711b/swlib/std_lib/include
INCLUDES += -I../../../component/common/network/http2/nghttp2-1.31.0/includes
INCLUDES += -I../../../component/common/application/mqtt/MQTTClient
INCLUDES += -I../../../component/common/network/coap/include
INCLUDES += -I../../../component/common/network/ssl/polarssl-1.3.8/include
INCLUDES += -I../../../component/common/network/ssl/mbedtls-2.4.0/include
INCLUDES += -I../../../component/common/network/ssl/ssl_ram_map/rom
INCLUDES += -I../../../component/common/drivers/i2s
INCLUDES += -I../../../component/common/utilities
INCLUDES += -I../../../component/common/application
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/include
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/lib/include/usb_otg
INCLUDES += -I../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/include
INCLUDES += -I../../../component/common/video/v4l2/inc
INCLUDES += -I../../../component/common/drivers/usb_class/host/uvc/inc
INCLUDES += -I../../../component/common/drivers/usb_class/device
INCLUDES += -I../../../component/common/drivers/usb_class/device/class
INCLUDES += -I../../../component/common/drivers/modules
INCLUDES += -I../../../component/common/media/framework
INCLUDES += -I../../../component/common/media/codec/mp3
INCLUDES += -I../../../component/common/media/rtp_codec/mjpeg
INCLUDES += -I../../../component/common/media/muxer
INCLUDES += -I../../../component/common/media/mmfv2
INCLUDES += -I../../../component/common/audio
INCLUDES += -I../../../component/common/media/rtp_codec
INCLUDES += -I../../../component/soc/realtek/8195b/app/xmodem/rom
INCLUDES += -I../../../component/common/test
INCLUDES += -I../../../component/common/file_system
INCLUDES += -I../../../component/common/file_system/dct
INCLUDES += -I../../../component/common/drivers/video/realtek/common/include/feature
INCLUDES += -I../../../component/common/mbed/hal
INCLUDES += -I../../../component/common/mbed/hal_ext
INCLUDES += -I../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh
INCLUDES += -I../../../component/common/application/qr_code_scanner/inc
INCLUDES += -I../../../component/common/application/qr_code_scanner/src
INCLUDES += -I../../../component/common/application/qr_code_scanner/src/zbar
INCLUDES += -I../../../component/common/application/qr_code_scanner/src/zbar/decoder
INCLUDES += -I../../../component/common/application/qr_code_scanner/src/zbar/include
INCLUDES += -I../../../component/common/application/qr_code_scanner/src/zbar/qrcode
INCLUDES += -I../../../component/common/audio/AEC
#INCLUDES += -I../../../component/common/audio/AEC/WebrtcAEC
#INCLUDES += -I../../../component/common/audio/AEC/WebrtcAEC/utility
#INCLUDES += -I../../../component/common/audio/AEC/WebrtcAEC/include
INCLUDES += -I../../../component/common/audio/opus-1.3.1/include
INCLUDES += -I../../../component/common/audio/libopusenc-0.2.1/include
INCLUDES += -I../src/carbox
INCLUDES += -I../src/carbox/vfs_compat

CARBOX_EXPERIMENTAL_USB ?= 1
CARBOX_EXPERIMENTAL_SMART_A_LINK ?= 1

ifeq ($(CARBOX_EXPERIMENTAL_USB),1)
CARBOX_USB_LIB := 1
endif

ifeq ($(CARBOX_USB_LIB),1)
# Use the OTA-compatible archive.  The vendor archive still contains the old
# carplay_ota_compat.o compiled for 0x200000 firmware slots; linking it makes a
# 0x370000 A/B image fail at exactly 2 MiB with "erase range overflow".  The
# replacement object's source is kept in src/carbox/ota/carplay_ota_compat.c.
CARBOX_USB_ARCHIVE := usb_lib/build/lib_usbsmart_ota.a
endif

CARBOX_USB_BUILD ?= 1
# CARBOX_USB_ORGI ?= 1



# Source file list
# SRC_C -> to Flash
# SRAM_C -> to internal ram
# ERAM_C -> to external ram
# -------------------------------------------------------------------

SRC_ASM = 
SRC_C =
SRAM_C =
ERAM_C =
CINIT_C = 
ITCM_C = 

# -------------------------------------------------------------------
# FreeRTOS-Plus-POSIX
POSIX_SRC = ../../../component/os/freertos/freertos_v10.0.0/portable/posix/lib/FreeRTOS-Plus-POSIX/source
SRC_C += $(POSIX_SRC)/FreeRTOS_POSIX_pthread_mutex.c
SRC_C += $(POSIX_SRC)/FreeRTOS_POSIX_utils.c
SRC_C += $(POSIX_SRC)/FreeRTOS_POSIX_pthread.c
SRC_C += $(POSIX_SRC)/FreeRTOS_POSIX_pthread_cond.c
SRC_C += $(POSIX_SRC)/FreeRTOS_POSIX_pthread_barrier.c
SRC_C += $(POSIX_SRC)/FreeRTOS_POSIX_clock.c
SRC_C += $(POSIX_SRC)/FreeRTOS_POSIX_unistd.c
SRC_C += $(POSIX_SRC)/FreeRTOS_POSIX_semaphore.c
SRC_C += $(POSIX_SRC)/FreeRTOS_POSIX_mqueue.c
SRC_C += $(POSIX_SRC)/FreeRTOS_POSIX_sched.c
SRC_C += $(POSIX_SRC)/FreeRTOS_POSIX_timer.c

#cmsis
#dsp
SRC_C += ../../../component/soc/realtek/8195b/cmsis/cmsis-dsp/source/BasicMathFunctions/arm_add_f32.c
SRC_ASM += ../../../component/soc/realtek/8195b/cmsis/cmsis-dsp/source/TransformFunctions/arm_bitreversal2.S
SRC_C += ../../../component/soc/realtek/8195b/cmsis/cmsis-dsp/source/TransformFunctions/arm_cfft_f32.c
SRC_C += ../../../component/soc/realtek/8195b/cmsis/cmsis-dsp/source/TransformFunctions/arm_cfft_radix8_f32.c
SRC_C += ../../../component/soc/realtek/8195b/cmsis/cmsis-dsp/source/ComplexMathFunctions/arm_cmplx_mag_f32.c
SRC_C += ../../../component/soc/realtek/8195b/cmsis/cmsis-dsp/source/CommonTables/arm_common_tables.c
SRC_C += ../../../component/soc/realtek/8195b/cmsis/cmsis-dsp/source/CommonTables/arm_const_structs.c
SRC_C += ../../../component/soc/realtek/8195b/cmsis/cmsis-dsp/source/StatisticsFunctions/arm_max_f32.c
SRC_C += ../../../component/soc/realtek/8195b/cmsis/cmsis-dsp/source/BasicMathFunctions/arm_mult_f32.c
SRC_C += ../../../component/soc/realtek/8195b/cmsis/cmsis-dsp/source/TransformFunctions/arm_rfft_fast_f32.c
SRC_C += ../../../component/soc/realtek/8195b/cmsis/cmsis-dsp/source/TransformFunctions/arm_rfft_fast_init_f32.c
SRC_C += ../../../component/soc/realtek/8195b/cmsis/cmsis-dsp/source/BasicMathFunctions/arm_scale_f32.c

SRC_C += ../../../component/soc/realtek/8195b/cmsis/rtl8195b-hp/source/ram_s/app_start.c
SRC_C += ../../../component/soc/realtek/8195b/cmsis/rtl8195b-hp/source/ram/mpu_config.c

#console
SRC_C += ../../../component/common/api/at_cmd/atcmd_lwip.c
SRC_C += ../../../component/common/api/at_cmd/atcmd_sys.c
SRC_C += ../../../component/common/api/at_cmd/atcmd_wifi.c
SRC_C += ../../../component/common/api/at_cmd/atcmd_qr_code.c
SRC_C += ../../../component/soc/realtek/8195b/app/shell/cmd_shell.c
SRC_C += ../../../component/soc/realtek/8195b/misc/driver/log_service.c
SRC_C += ../../../component/soc/realtek/8195b/misc/driver/low_level_io.c
SRC_C += ../../../component/soc/realtek/8195b/misc/driver/rtl_console.c

#multimedia
#mmfv2
SRC_C += ../../../component/common/media/mmfv2/module_aac.c
SRC_C += ../../../component/common/media/mmfv2/module_aad.c
SRC_C += ../../../component/common/media/mmfv2/module_array.c
SRC_C += ../../../component/common/media/mmfv2/module_audio.c
SRC_C += ../../../component/common/media/mmfv2/module_g711.c
# H.264 encoding is intentionally disabled in this CarPlay-only image.
# Do not restore module_h264.c, -l_h264, or lib_rtk264 independently.
# The RTL8195B hardware encoder currently pulls the VOE/ISP video-subsystem
# initialization and HAL.  A future standalone encoder build must restore and
# validate its complete dependency set, even if the camera ISP path stays off.
# The separate software lib_x264 must remain for hard references in the closed
# Accessory library; it does not require the RTL8195B VOE/ISP hardware path.
SRC_C += ../../../component/common/media/mmfv2/module_i2s.c
# module_jpeg.c conflicts with Smart lib_jpeg.a; excluded unconditionally
# ifneq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
# SRC_C += ../../../component/common/media/mmfv2/module_jpeg.c
# endif
SRC_C += ../../../component/common/media/mmfv2/module_mp4.c
SRC_C += ../../../component/common/media/mmfv2/module_rtp.c
SRC_C += ../../../component/common/media/mmfv2/module_rtsp2.c
SRC_C += ../../../component/common/media/mmfv2/module_uvcd.c
SRC_C += ../../../component/common/media/mmfv2/module_httpfs.c
SRC_C += ../../../component/common/media/mmfv2/module_dup.c

#mmfv2_example
ifneq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
SRC_C += ../../../component/common/example/media_framework/mmf2_example_2way_audio_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_a_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_aac_array_rtsp_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_aacloop_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_audioloop_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_av21_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_av2_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_av_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_av_mp4_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_av_rtsp_mp4_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_av_mp4_httpfs_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_g711loop_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_h264_2way_audio_pcmu_doorbell_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_h264_2way_audio_pcmu_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_h264_array_rtsp_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_i2s_audio_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_joint_test_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_joint_test_rtsp_mp4_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_param.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_pcmu_array_rtsp_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_rtp_aad_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_simo_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_v1_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_v1_param_change_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_v2_init.c
ifneq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
SRC_C += ../../../component/common/example/media_framework/mmf2_example_v3_init.c
endif
SRC_C += ../../../component/common/example/media_framework/mmf2_example_av_dup_init.c
SRC_C += ../../../component/common/example/media_framework/mmf2_example_mp4_dual_init.c
SRC_C += ../../../component/common/example/media_framework/snapshot_setting.c
#SRC_C += ../../../component/common/example/media_framework/example_media_framework_sd_detect.c
SRC_C += ../../../component/common/example/media_uvcd/usbd_uvc_parm.c
SRC_C += ../../../component/common/example/media_uvcd/usbd_uvc_event.c
endif

#sensor

#network
#api
#wifi
#rtw_wpa_supplicant
SRC_C += ../../../component/common/api/wifi/rtw_wpa_supplicant/src/crypto/tls_polarssl.c
SRC_C += ../../../component/common/api/wifi/rtw_wpa_supplicant/wpa_supplicant/wifi_eap_config.c
SRC_C += ../../../component/common/api/wifi/rtw_wpa_supplicant/wpa_supplicant/wifi_p2p_config.c
SRC_C += ../../../component/common/api/wifi/rtw_wpa_supplicant/wpa_supplicant/wifi_wps_config.c
SRC_C += ../../../component/common/api/wifi/wifi_conf.c
SRC_C += ../../../component/common/api/wifi/wifi_ind.c
SRC_C += ../../../component/common/api/wifi/wifi_promisc.c
SRC_C += ../../../component/common/api/wifi/wifi_simple_config.c
SRC_C += ../../../component/common/api/wifi/wifi_util.c
SRC_C += ../../../component/common/api/lwip_netconf.c

#app
#mqtt
##Debug
##SRC_C += ../../../component/common/application/mqtt/MQTTClient/MQTTClient.c
##SRC_C += ../../../component/common/application/mqtt/MQTTPacket/MQTTConnectClient.c
##SRC_C += ../../../component/common/application/mqtt/MQTTPacket/MQTTConnectServer.c
##SRC_C += ../../../component/common/application/mqtt/MQTTPacket/MQTTDeserializePublish.c
##SRC_C += ../../../component/common/application/mqtt/MQTTPacket/MQTTFormat.c
##SRC_C += ../../../component/common/application/mqtt/MQTTClient/MQTTFreertos.c
##SRC_C += ../../../component/common/application/mqtt/MQTTPacket/MQTTPacket.c
##SRC_C += ../../../component/common/application/mqtt/MQTTPacket/MQTTSerializePublish.c
##SRC_C += ../../../component/common/application/mqtt/MQTTPacket/MQTTSubscribeClient.c
##SRC_C += ../../../component/common/application/mqtt/MQTTPacket/MQTTSubscribeServer.c
##SRC_C += ../../../component/common/application/mqtt/MQTTPacket/MQTTUnsubscribeClient.c
##SRC_C += ../../../component/common/application/mqtt/MQTTPacket/MQTTUnsubscribeServer.c

SRC_C += ../../../component/soc/realtek/8195b/misc/platform/ota_8195b.c
SRC_C += ../../../component/common/api/network/src/ping_test.c
SRC_C += ../../../component/common/utilities/ssl_client.c
SRC_C += ../../../component/common/utilities/ssl_client_ext.c
SRC_C += ../../../component/common/utilities/tcptest.c
SRC_C += ../../../component/common/api/network/src/wlan_network.c

#coap
SRC_C += ../../../component/common/network/coap/sn_coap_ameba_port.c
SRC_C += ../../../component/common/network/coap/sn_coap_builder.c
SRC_C += ../../../component/common/network/coap/sn_coap_header_check.c
SRC_C += ../../../component/common/network/coap/sn_coap_parser.c
SRC_C += ../../../component/common/network/coap/sn_coap_protocol.c

#googlenest
SRC_C += ../../../component/common/application/google/google_tls.c

#http
SRC_C += ../../../component/common/network/httpc/httpc_tls.c
ifneq ($(CARBOX_EXPERIMENTAL_USB),1)
SRC_C += ../../../component/common/network/httpd/httpd_tls.c
endif
#lwip
#api
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/api/if_api.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/api/api_lib.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/api/api_msg.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/api/err.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/api/netbuf.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/api/netdb.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/api/netifapi.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/api/sockets.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/api/tcpip.c

#core
#ipv4
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv4/autoip.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv4/dhcp.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv4/etharp.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv4/icmp.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv4/igmp.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv4/ip4.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv4/ip4_addr.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv4/ip4_frag.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv4/ip_nat/lwip_ip4nat.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/def.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/dns.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/inet_chksum.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/init.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ip.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/netif.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/pbuf.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/raw.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/stats.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/sys.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/tcp.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/tcp_in.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/tcp_out.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/timeouts.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/udp.c
##ITMC -> DTCM
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/mem.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/memp.c

#ipv6
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv6/ip6_addr.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv6/ip6.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv6/ip6_frag.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv6/icmp6.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv6/nd6.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv6/mld6.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv6/ethip6.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv6/inet6.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv6/dhcp6.c

#netif
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ethernet.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/auth.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/ccp.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/chap-md5.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/chap-new.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/chap_ms.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/demand.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/eap.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/eui64.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/fsm.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/ipcp.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/ipv6cp.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/lcp.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/lwip_ecp.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/magic.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/polarssl/md5.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/mppe.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/multilink.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/ppp.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/pppapi.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/pppcrypt.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/pppoe.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/pppol2tp.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/pppos.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/upap.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/utils.c
SRC_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ppp/vj.c

#port
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/port/realtek/freertos/ethernetif.c
ITCM_C += ../../../component/common/drivers/wlan/realtek/src/osdep/lwip_intf.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/port/realtek/freertos/sys_arch.c

ITCM_C += ../../../component/common/network/dhcp/dhcps.c
ITCM_C += ../../../component/common/network/sntp/sntp.c

# ITCM is substantially more valuable to the sustained audio codecs than to
# lwIP control/API code.  Keep the network stack in LPDDR (ERAM), rather than
# falling back to NOR XIP, and reserve ITCM for the selected WLAN RX,
# scheduler and codec hot paths in rtl8195bhp_ram_is.ld.
CODEC_ITCM_RECLAIM_NETWORK ?= 1
ifeq ($(CODEC_ITCM_RECLAIM_NETWORK),1)
LWIP_CHECKSUM_ITCM_C := ../../../component/common/network/lwip/lwip_v2.1.2/src/core/inet_chksum.c
# Keep the sustained checksum hot path in ITCM.  It accounts for a measurable
# part of TCP_IP CPU time, whereas the remainder of lwIP is deliberately kept
# in LPDDR to preserve ITCM for scheduler and codec hot paths.
NETWORK_STACK_ITCM_C := $(filter-out $(LWIP_CHECKSUM_ITCM_C),$(filter \
	../../../component/common/network/% \
	../../../component/common/drivers/wlan/realtek/src/osdep/lwip_intf.c,\
	$(ITCM_C)))
ITCM_C := $(filter-out $(NETWORK_STACK_ITCM_C),$(ITCM_C))
ERAM_C += $(NETWORK_STACK_ITCM_C)
endif

#mdns
SRC_C += ../../../component/common/network/mDNS/mDNSPlatform.c

#ssl
#mbedtls
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/aesni.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/blowfish.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/camellia.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/ccm.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/certs.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/cipher.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/cipher_wrap.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/cmac.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/debug.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/gcm.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/havege.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/md2.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/md4.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/memory_buffer_alloc.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/net_sockets.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/padlock.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/pkcs11.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/pkcs12.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/pkcs5.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/pkparse.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/platform.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/ripemd160.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/ssl_cache.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/ssl_ciphersuites.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/ssl_cli.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/ssl_cookie.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/ssl_srv.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/ssl_ticket.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/ssl_tls.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/threading.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/timing.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/version.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/version_features.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/x509.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/x509_create.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/x509_crl.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/x509_crt.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/x509_csr.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/x509write_crt.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/x509write_csr.c
SRC_C += ../../../component/common/network/ssl/mbedtls-2.4.0/library/xtea.c

#ssl_ram_map
SRC_C += ../../../component/common/network/ssl/ssl_ram_map/rom/rom_ssl_ram_map.c
SRC_C += ../../../component/common/network/ssl/ssl_func_stubs/ssl_func_stubs.c

#websocket
SRC_C += ../../../component/common/network/websocket/wsclient_tls.c
SRC_C += ../../../component/common/network/websocket/wsserver_tls.c

#os
#freertos
#portable
##SRC_C += ../../../component/os/freertos/freertos_v10.0.0/portable/MemMang/heap_4.c
##Debug
ITCM_C += ../../../component/os/freertos/freertos_v10.0.0/portable/GCC/ARM_RTL8195B/port.c
ITCM_C += ../../../component/os/freertos/freertos_v10.0.0/croutine.c
ITCM_C += ../../../component/os/freertos/freertos_v10.0.0/event_groups.c
ITCM_C += ../../../component/os/freertos/freertos_v10.0.0/list.c
ITCM_C += ../../../component/os/freertos/freertos_v10.0.0/queue.c
ITCM_C += ../../../component/os/freertos/freertos_v10.0.0/stream_buffer.c
ITCM_C += ../../../component/os/freertos/freertos_v10.0.0/tasks.c
ITCM_C += ../../../component/os/freertos/freertos_v10.0.0/timers.c
SRC_C += ../../../component/os/freertos/cmsis_os.c
SRC_C += ../../../component/os/os_dep/device_lock.c
SRC_C += ../../../component/os/freertos/freertos_cb.c
SRC_C += ../../../component/os/freertos/freertos_service.c
SRC_C += ../../../component/os/os_dep/osdep_service.c
#peripheral
#api
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/analogin_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/audio_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/crypto_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/dma_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/efuse_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/ethernet_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/gpio_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/gpio_irq_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/i2c_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/i2s_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/icc_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/pcm_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/pinmap.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/pinmap_common.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/power_mode_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/pwmout_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/serial_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/sgpio_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/spdio_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/spi_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/sys_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/timer_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/us_ticker.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/us_ticker_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/wait_api.c
SRC_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/wdt_api.c

#hal
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/hal_adc.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/hal_audio.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_s/hal_efuse.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/hal_eth.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram/hal_gdma.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram/hal_gpio.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/hal_i2c.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/hal_i2s.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_s/hal_icc.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram/hal_icc_app.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram/hal_misc.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/hal_pcm.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/hal_pwm.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/hal_sdio_host.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/hal_sgpio.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_s/hal_spic.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/hal_ssi.c
SRC_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram/hal_uart.c

#utilities
#example
ifneq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
SRC_C += ../../../component/common/example/bcast/example_bcast.c
SRC_C += ../../../component/common/example/cJSON/example_cJSON.c
SRC_C += ../../../component/common/example/coap/example_coap.c
SRC_C += ../../../component/common/example/dct/example_dct.c
SRC_C += ../../../component/common/example/eap/example_eap.c
SRC_C += ../../../component/common/example/example_entry.c
SRC_C += ../../../component/common/example/fatfs/example_fatfs.c
SRC_C += ../../../component/common/example/std_file/example_std_file.c
SRC_C += ../../../component/common/example/sd_hot_plug/example_sd_hotplug.c
SRC_C += ../../../component/common/example/media_mp4_demuxer/example_media_mp4_demuxer.c
SRC_C += ../../../component/common/example/get_beacon_frame/example_get_beacon_frame.c
SRC_C += ../../../component/common/example/high_load_memory_use/example_high_load_memory_use.c
SRC_C += ../../../component/common/example/http_client/example_http_client.c
SRC_C += ../../../component/common/example/http_download/example_http_download.c
SRC_C += ../../../component/common/example/httpc/example_httpc.c
SRC_C += ../../../component/common/example/httpd/example_httpd.c
SRC_C += ../../../component/common/example/mcast/example_mcast.c
SRC_C += ../../../component/common/example/mdns/example_mdns.c
SRC_C += ../../../component/common/example/media_framework/example_media_framework.c
SRC_C += ../../../component/common/example/media_h264_to_sdcard/example_media_h264_to_sdcard.c
SRC_C += ../../../component/common/example/media_uvcd/example_media_uvcd.c
SRC_C += ../../../component/common/example/mqtt/example_mqtt.c
SRC_C += ../../../component/common/example/nonblock_connect/example_nonblock_connect.c
SRC_C += ../../../component/common/example/ota_sdcard/example_ota_sdcard.c
SRC_C += ../../../component/common/example/ota_http/example_ota_http.c
SRC_C += ../../../component/common/example/usb_dfu_ota/example_usb_dfu_ota.c
SRC_C += ../../../component/common/example/qr_code_scanner/example_qr_code_scanner.c
SRC_C += ../../../component/common/example/rarp/example_rarp.c
SRC_C += ../../../component/common/example/sdcard_upload_httpd/example_sdcard_upload_httpd.c
SRC_C += ../../../component/common/example/sntp_showtime/example_sntp_showtime.c
SRC_C += ../../../component/common/example/socket_select/example_socket_select.c
SRC_C += ../../../component/common/example/socket_tcp_trx/example_socket_tcp_trx_1.c
SRC_C += ../../../component/common/example/socket_tcp_trx/example_socket_tcp_trx_2.c
SRC_C += ../../../component/common/example/tcp_keepalive/example_tcp_keepalive.c
SRC_C += ../../../component/common/example/usb_mass_storage/example_usb_msc.c
SRC_C += ../../../component/common/example/wifi_mac_monitor/example_wifi_mac_monitor.c
SRC_C += ../../../component/common/example/wifi_roaming/example_wifi_roaming.c
SRC_C += ../../../component/common/example/wlan_fast_connect/example_wlan_fast_connect.c
##SRC_C += ../../../component/common/example/wowlan/example_wowlan.c
SRC_C += ../../../component/common/example/websocket_server/example_ws_server.c
SRC_C += ../../../component/common/example/websocket_client/example_wsclient.c
SRC_C += ../../../component/common/example/xml/example_xml.c
##SRC_C += ../../../component/common/example/std_file/example_std_file.c
SRC_C += ../../../component/common/example/wlan_scenario/example_wlan_scenario.c
SRC_C += ../../../component/common/example/pppoe/example_pppoe.c
SRC_C += ../../../component/common/example/snapshot/example_snapshot.c
SRC_C += ../../../component/common/example/audio_opus_encode/example_audio_opus_encode.c
SRC_C += ../../../component/common/example/audio_helix_mp3/example_audio_helix_mp3.c
endif

#FatFs
#disk_if
SRC_C += ../../../component/common/file_system/fatfs/disk_if/src/sdcard.c
##SRC_C += ../../../component/common/drivers/sdio/realtek/sdio_host/src/sdio_combine.c
##Debug
SRC_C += ../../../component/common/file_system/fatfs/disk_if/src/usbdisk.c

#fatfs_ext
SRC_C += ../../../component/common/file_system/fatfs/fatfs_ext/src/ff_driver.c

#r0.10c
#option
#SRC_C += ../../../component/common/file_system/fatfs/r0.10c/src/option/ccsbcs.c
SRC_C += ../../../component/common/file_system/fatfs/r0.14/diskio.c
SRC_C += ../../../component/common/file_system/fatfs/r0.14/ff.c
SRC_C += ../../../component/common/file_system/fatfs/r0.14/ffsystem.c
SRC_C += ../../../component/common/file_system/fatfs/r0.14/ffunicode.c
SRC_C += ../../../component/common/file_system/fatfs/fatfs_reent.c
SRC_C += ../../../component/common/file_system/fatfs/fatfs_sdcard_api.c
SRC_C += ../../../component/common/file_system/fatfs/fatfs_ramdisk_api.c
SRC_C += ../../../component/common/utilities/cJSON.c
SRC_C += ../../../component/common/utilities/http_client.c
SRC_C += ../../../component/common/utilities/xml.c

#user
#evalutaion_board
#sensor_board_v1
#SRC_C += ../src/eval/sensor_board_v1/AL3042.c             /* disabled */
#SRC_C += ../src/eval/sensor_board_v1/ambient_light_sensor.c  /* disabled */
SRC_C += ../../../component/common/drivers/wlan/realtek/src/core/option/rtw_opt_skbuf.c

SRC_C += ../src/main.c
SRC_C += ../src/carbox/aes_backend_select.c
SRC_C += ../src/carbox/aes_ctr_periodic_selftest.c
SRC_C += ../src/carbox/carbox_diag.c
SRC_C += ../src/carbox/pc_profiler.c
SRC_C += ../src/carbox/ota_local_upload_page.c
SRC_C += ../src/carbox/irq_profiler.c
SRC_C += ../src/carbox/gcd_sync_profiler.c
SRC_C += ../src/carbox/screen_queue_profiler.c
SRC_C += ../src/carbox/screen_rx_rate_limit.c
SRC_C += ../src/carbox/audio_decode_profiler.c
SRC_C += ../src/carbox/memcheck.c
SRC_C += ../src/carbox/carbox_stubs.c
SRC_C += ../src/carbox/libusb_ref_compat/libusb_ref_compat_hal.c
SRC_C += ../src/carbox/libusb_ref_compat/libusb_ref_compat_os.c
SRC_C += ../src/carbox/libusb_ref_compat/usb_ref_smart_compat.c
SRC_C += ../src/carbox/libusb_ref_compat/carplay_smart_api_stubs.c
SRC_C += ../src/carbox/vfs_compat/carbox_vfs_compat.c
SRC_C += ../src/carbox/vfs_compat/vfs_wrap.c
SRC_C += ../src/carbox/vfs_compat/vfs_fatfs.c
SRC_C += ../src/carbox/vfs_compat/vfs_littlefs.c
SRC_C += ../../../component/common/file_system/fatfs/fatfs_flash_api.c
SRC_C += ../../../component/common/file_system/littlefs/r2.41/lfs.c
SRC_C += ../../../component/common/file_system/littlefs/r2.41/lfs_util.c
# SRC_C += ../src/carbox/test/test_serial.c

#CINIT
# -------------------------------------------------------------------
#@CINIT
CINIT_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram/hal_timer.c
CINIT_C += ../src/carbox/atss_runtime_counter.c
CINIT_C += ../../../component/os/freertos/freertos_v10.0.0/portable/MemMang/heap_4_2.c
CINIT_C += ../../../component/soc/realtek/8195b/misc/utilities/source/ram/libc_wrap.c

#SRAM
# -------------------------------------------------------------------
#@SRAM
SRAM_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/flash_api.c
SRAM_C += ../../../component/soc/realtek/8195b/misc/driver/flash_api_ext.c
SRAM_C += ../../../component/common/file_system/fatfs/disk_if/src/flash_fatfs.c
SRAM_C += ../src/carbox/vfs_compat/carbox_littlefs.c
SRAM_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_s/hal_flash.c
SRAM_C += ../src/carbox/system_overclock.c
# This wrapper intercepts every device lock, including FLASH locks used before
# external RAM is ready, so it must execute from internal SRAM.
SRAM_C += ../src/carbox/crypto_engine_profiler.c
# Scheduler-facing transaction API; keep it off XIP and available during
# crypto initialization. Only CarPlay AES/ChaCha archive members call it.
SRAM_C += ../src/carbox/crypto_priority_lock.c
SRAM_C += ../src/carbox/memcpy_task_profiler.c
SRAM_C += ../src/carbox/large_memcpy_gdma.c
SRAM_C += ../src/carbox/video_handover_zero_copy.c
SRAM_C += ../src/carbox/screen_tx_direct_crypto.c

#ERAM
# -------------------------------------------------------------------
#@ERAM
ERAM_C +=
ERAM_C += ../src/carbox/usb_hcd_profiler.c
# The RX hooks execute per packet; keep profiling out of XIP so the
# instrumentation does not add flash stalls to the path being measured.
ERAM_C += ../src/carbox/net_queue_profiler.c
ERAM_C += ../src/carbox/aac_decoder_router.c
# The repeated decoder benchmark and its task live in external DRAM. The AAC
# input and PCM scratch buffers are also allocated once from the DRAM heap.
ERAM_C += ../src/carbox/aac_decoder_benchmark.c




# Generate obj list
# -------------------------------------------------------------------

ASM_O = $(patsubst %.S,%.o,$(SRC_ASM))
SRC_O = $(patsubst %.c,%.o,$(SRC_C))
CPP_O = $(patsubst %.cpp,%.o,$(SRC_CPP))
ERAM_O = $(patsubst %.c,%.o,$(ERAM_C))
SRAM_O = $(patsubst %.c,%.o,$(SRAM_C))
CINIT_O = $(patsubst %.c,%.o,$(CINIT_C))
ITCM_O = $(patsubst %.c,%.o,$(ITCM_C))
DTCM_O = $(patsubst %.c,%.o,$(DTCM_C))

DEPENDENCY_LIST = $(patsubst %.S,%.d,$(SRC_ASM))
DEPENDENCY_LIST += $(patsubst %.c,%.d,$(SRC_C))
DEPENDENCY_LIST += $(patsubst %.cpp,%.d,$(SRC_CPP))
DEPENDENCY_LIST += $(patsubst %.c,%.d,$(ERAM_C))
DEPENDENCY_LIST += $(patsubst %.c,%.d,$(SRAM_C))
DEPENDENCY_LIST += $(patsubst %.c,%.d,$(CINIT_C))
DEPENDENCY_LIST += $(patsubst %.c,%.d,$(ITCM_C))

SRC_C_LIST = $(notdir $(SRC_C)) $(notdir $(ERAM_C)) $(notdir $(SRAM_C)) $(notdir $(CINIT_C)) $(notdir $(ITCM_C))
OBJ_LIST = $(addprefix $(OBJ_DIR)/,$(patsubst %.c,%.o,$(SRC_C_LIST))) 
OBJ_LIST += $(addprefix $(OBJ_DIR)/,$(patsubst %.S,%.o,$(notdir $(SRC_ASM))))
OBJ_LIST += $(addprefix $(OBJ_DIR)/,$(patsubst %.cpp,%.o,$(notdir $(SRC_CPP))))


# Compile options
# -------------------------------------------------------------------

GCCFLAGS =
GCCFLAGS += -march=armv8-m.main+dsp -mthumb -mcmse -mfloat-abi=softfp -mfpu=fpv5-sp-d16 -g -gdwarf-3 -Os -c -MMD --save-temps
GCCFLAGS += -nostartfiles -nodefaultlibs -nostdlib -fstack-usage -fdata-sections -ffunction-sections -fno-common

# Define Macro
GCCFLAGS += -D__thumb2__ -DCONFIG_PLATFORM_8195B -DCONFIG_PLATFORM_8195BHP -D__FPU_PRESENT -D__ARM_ARCH_7M__=0 -D__ARM_ARCH_7EM__=0 -D__ARM_ARCH_8M_MAIN__=1 -D__ARM_ARCH_8M_BASE__=0 
GCCFLAGS += -DCONFIG_BUILD_RAM=1 -DCONFIG_BUILD_LIB=1
GCCFLAGS += -DV8M_STKOVF -DARM_MATH_CM7 -DCONFIG_FATFS_WRAPPER=1
NET_GDMA_COPY ?= 0
NET_GDMA_BENCH ?= 0
NET_GDMA_STATS ?= 0
NET_GDMA_OWNER_BOOST_PRIORITY ?= 11
# Descriptor/skb validation and 10-second diagnostics for the closed-driver
# RX ring hook. WLAN_RX_RING_SWAP enables the validated spare-buffer rotation;
# disabling it leaves this as an observation-only legacy memcpy probe.
WLAN_RX_SWAP_BRINGUP_PROFILE ?= 0
WLAN_RX_RING_SWAP ?= 1
TCP_PHASE_PROFILE ?= 0
# Detailed 10-second breakdown of tcp_input() processing. This is independent
# of the compact 5-second TCP_PERF throughput/checksum summary above.
TCP_CORE_PHASE_PROFILE ?= 0
# Ten-second breakdown of tcp_output() and its synchronous WLAN transmit path.
# This distinguishes ACK/data traffic and separates lwIP preparation/checksum/IP
# time from skb allocation, scatter-gather copy, and closed-driver submission.
TCP_OUTPUT_PROFILE ?= 0
# Ten-second timing of the en3 CDC-NCM transmit path.  The closed NCM library
# waits synchronously for USB completion, so measure that wait separately from
# any lwIP pbuf-chain flattening done by ethernetif.c.
NCM_TX_PROFILE ?= 0
# Move the closed synchronous NCM send/wait out of TCP_IP.
NCM_TX_ASYNC ?= 1
# Compact 10-second batch health report used to validate aggregation limits.
NCM_TX_ASYNC_PROFILE ?= 0
# Seal a multi-datagram NTB when it reaches either limit, or 5 ms after the
# oldest retained Ethernet packet was queued.  The timeout is not sliding.
NCM_TX_BATCH ?= 1
NCM_TX_BATCH_MAX_PACKETS ?= 16
NCM_TX_BATCH_MAX_BYTES ?= 4096
NCM_TX_BATCH_TIMEOUT_MS ?= 1
# Gather all pbuf fragments of a sealed NTB into the closed driver's contiguous
# USB buffer with the linked-GDMA copyv path already validated by socket recv.
# Header/table/cache-line edges stay on M33 and any busy/error falls back to a
# complete CPU copy before the USB request is submitted.
NCM_TX_BATCH_GDMA ?= 1
PC_PROFILER ?= 1
# Optional PC-level reports. Keep task utilization sampling enabled while
# suppressing the verbose per-PC reports during IRQ-count investigation.
PC_PROFILER_PC_DETAIL ?= 0
PC_PROFILER_RTW_RECV_DETAIL ?= 0
PC_PROFILER_RTW_DUMP_PROFILE ?= 0
# Keep the IRQ hook/counter banks required by USB CH4 NAK coalescing, but stop
# periodic IRQPROF reports once validation is complete.
IRQ_PROFILE ?= 1
IRQ_PROFILE_REPORT ?= 0
# Diagnose the high-rate USB interrupt source by sampling GINTSTS & GINTMSK in
# the IRQ trampoline.  This is independent of the aggregate IRQ counters.
IRQ_PROFILE_USB_CAUSE ?= 0
# Low-overhead measurement at the existing ISR semaphore-give wrapper: count
# successful wakeups and ISR-return yield requests only.
IRQ_PROFILE_USB_HANDOFF ?= 0
# Attribute channel-4 NAK/CHHLTD bottom-half service time and halt calls.
# This is diagnostic only; the wrapper always calls the vendor implementation.
IRQ_PROFILE_USB_CH4_FLOW ?= 0
# Capture CDC-NCM NTB negotiation so channel-4's fixed 2 KiB receive submits
# can be attributed to the device parameters or the host-side size selection.
IRQ_PROFILE_USB_CH4_NCM ?= 0
# After a channel-4 NAK, identify whether the next accepted receive submit or
# CHHLTD arrives first. Diagnostic only; no HCD state or IRQ mask is changed.
IRQ_PROFILE_USB_CH4_SEQUENCE ?= 0
# Drop the stale NVIC latch just before the HCD task re-enables USB_IRQn, then
# re-read the controller and software-pend any cause which arrived in the
# clear/re-enable race window.
USB_IRQ_SAFE_DEDUP ?= 1
# Drop channel-3 ACK-only/non-PING causes in the IRQ top half.  The closed HCD
# otherwise wakes its priority-11 task even though its ordinary ACK path is a
# no-op.  PING ACK and every compound cause retain the vendor path.
USB_IRQ_CH3_ACK_FASTPATH ?= 1
# Defer a channel-4 NAK-only handoff until the hardware's following CHHLTD,
# allowing the closed HCD to consume both latched reasons in one task wake.
USB_IRQ_CH4_NAK_COALESCE ?= 1
USB_IRQ_CH4_NAK_COALESCE_TIMEOUT_US ?= 50000
# The closed HCD disables USB_IRQn in its top half and re-enables it only after
# the ISR task drains HCINT.  Retain the original unsafe single-check experiment
# as an off-by-default rollback reference; do not enable with SAFE_DEDUP.
USB_IRQ_CLEAR_STALE_PENDING ?= 0
MEMCPY_TASK_PROFILE ?= 0
LARGE_MEMCPY_GDMA ?= 1
LARGE_MEMCPY_GDMA_THRESHOLD ?= 4096
LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY ?= 11
# Temporary correctness check for the new fragmented socket-recv DMA path.
# Every completed DMA batch is compared by M33 before its pbufs are released;
# mismatch prints the exact byte and forces a full CPU recopy of that batch.
LARGE_MEMCPY_GDMA_COPYV_VERIFY ?= 0
SOCKET_RECV_PROFILE ?= 0
# Gather fragmented TCP pbuf payloads into one linked-GDMA submission when a
# recv() batch exceeds 4 KiB.  Sources may be non-contiguous; caller output is
# contiguous.  Cache-line edges remain on the M33 path.
SOCKET_RECV_GDMA ?= 1
SOCKET_RECV_GDMA_PROFILE ?= 0
SOCKET_RECV_GDMA_THRESHOLD ?= 4096
# TCP core does not copy payload into a second socket buffer.  This probe
# measures the pbuf-pointer handoff into recvmbox and the socket wakeup event.
TCP_SOCKET_HANDOFF_PROFILE ?= 0
# Channels 4 and 5 are the only linked-list-capable channels on each HP GDMA.
# Keep them out of the ordinary single-block allocator so a later socket recv
# linked-list implementation can claim one deterministically.
GDMA_RESERVE_MULTIBLOCK_CHANNELS ?= 1
GCD_SYNC_PROFILE ?= 0
# Diagnostic A/B switch.  The production default preserves DispatchLite's
# requested worker priority; set this to a non-negative value only for testing.
GCD_WORK_PRIORITY ?= -1
# The closed USB HCD bottom half is short and must run immediately after the
# top-half ISR masks USB_IRQn.  Keep it above all normal networking tasks.
USBH_ISR_TASK_PRIORITY ?= 11
# Keep the closed HCD main worker above the priority-4 audio and screen tasks so
# CPU-heavy decode cannot delay USB completion handling and the next transfer.
# It remains below TCP/IP and NCM (priority 10) and the USB ISR worker (11).
USBH_MAIN_TASK_PRIORITY ?= 6
SCREEN_QUEUE_PROFILE ?= 0
# Production switch for the temporary RX/TX investigation reports. Keep the
# limiter, pacer, handover gate, and pressure feedback active when this is off.
SCREEN_DATAPATH_PROFILE ?= 1
# Pace receive-window credit only for the iPhone screen TCP connection.  The
# closed receiver is identified once by task plus its validated 128-byte frame
# header; all other sockets retain the stock lwIP receive path.
SCREEN_RX_RATE_LIMIT ?= 1
SCREEN_RX_RATE_LIMIT_BPS ?= 14000000
SCREEN_RX_RATE_LIMIT_INTERVAL_MS ?= 10
SCREEN_RX_RATE_LIMIT_BUCKET_BYTES ?= 32768
SCREEN_RX_RATE_LIMIT_CONTROL_MS ?= 100
SCREEN_RX_RATE_LIMIT_DEADBAND_PERCENT ?= 10
SCREEN_RX_RATE_LIMIT_FILTER_SHIFT ?= 2
SCREEN_RX_RATE_LIMIT_VALVE_MIN_BPS ?= 1000000
SCREEN_RX_RATE_LIMIT_VALVE_MAX_BPS ?= 14000000
SCREEN_RX_RATE_LIMIT_OPEN_STEP_BPS ?= 125000
SCREEN_RX_RATE_LIMIT_CLOSE_STEP_BPS ?= 500000
SCREEN_RX_RATE_LIMIT_OPEN_HOLD_MS ?= 100
SCREEN_RX_RATE_LIMIT_CLOSE_HOLD_MS ?= 150
SCREEN_RX_RATE_LIMIT_TASK_PRIORITY ?= 5
SCREEN_RX_RATE_LIMIT_TASK_STACK ?= 512
# Smooth each ScreenThread TCP write without creating a second long-term
# controller. RX owns the long-term rate; TX has a slightly higher catch-up
# ceiling but can burst by no more than the peer's observed 23,040-byte window.
SCREEN_TX_PACER ?= 1
SCREEN_TX_PACER_BPS ?= 15000000
SCREEN_TX_PACER_BUCKET_BYTES ?= 23040
SCREEN_TX_PACER_CHUNK_BYTES ?= 4096
SCREEN_TX_PACER_WAIT_MS ?= 1
SCREEN_TX_PRESSURE_FEEDBACK ?= 1
SCREEN_TX_PRESSURE_TRIGGER_MS ?= 75
SCREEN_TX_PRESSURE_CLEAN_MS ?= 500
# Narrow backpressure diagnostic for the closed AirPlay sender's "screen block"
# retry.  It samples only failed ScreenThread writes and stays independent of
# the older, verbose frame/queue profiler.
SCREEN_BLOCK_PROFILE ?= 1
# Once a screen write identifies the video TCP PCB, summarize the returning
# ACK cadence and advertised-window behavior.  This distinguishes a slow peer
# reader from local USB/NCM queueing without restoring the verbose TCP profile.
SCREEN_TCP_ACK_PROFILE ?= 0
# Measure both the closed AirPlay converter call and the nested FDK AAC decode.
# Reporting is windowed so the probe adds no per-frame UART traffic.
AUDIO_DECODE_PROFILE ?= 1
AUDIO_DECODE_PROFILE_WINDOW_MS ?= 10000
AUDIO_DECODE_PROFILE_SLOW_US ?= 10000
# Only decoder calls are routed; all AAC encoder symbols remain on FDK.
# 0: FDK only. 1: Helix for supported raw AAC-LC, automatic FDK fallback.
AAC_DECODER_MODE ?= 1
AAC_DECODER_ROUTE_PROFILE_WINDOW_MS ?= 10000
AAC_DECODER_MODE_STAMP := $(OBJ_DIR)/.aac_decoder_mode_$(AAC_DECODER_MODE)
$(AAC_DECODER_MODE_STAMP):
	@mkdir -p $(OBJ_DIR)
	@rm -f $(OBJ_DIR)/.aac_decoder_mode_*
	@touch $@
../src/carbox/aac_decoder_router.o: $(AAC_DECODER_MODE_STAMP)

../src/carbox/ota_local_upload_page.o: ../src/carbox/web/update.js
# One-shot FDK-vs-Helix AAC-LC benchmark. The sample is loaded from FAT into
# RAM, decoder calls alone are timed, and cached results are reported every
# 10 seconds without continuously consuming CPU.
AAC_DECODER_BENCHMARK ?= 0
AAC_DECODER_BENCHMARK_LOOPS ?= 1
AAC_DECODER_BENCHMARK_START_DELAY_MS ?= 3000
AAC_DECODER_BENCHMARK_REPORT_MS ?= 10000
AAC_DECODER_BENCHMARK_TASK_PRIORITY ?= 1
AAC_DECODER_BENCHMARK_RUN_PRIORITY ?= 11
AAC_DECODER_BENCHMARK_TASK_STACK ?= 2048
AAC_DECODER_BENCHMARK_SAMPLE := ../src/carbox/testdata/bear-audio-lc-aac.aac
AAC_DECODER_BENCHMARK_STAMP := $(OBJ_DIR)/.aac_benchmark_$(AAC_DECODER_BENCHMARK)
$(AAC_DECODER_BENCHMARK_STAMP):
	@mkdir -p $(OBJ_DIR)
	@rm -f $(OBJ_DIR)/.aac_benchmark_*
	@touch $@
../src/carbox/aac_decoder_benchmark.o: $(AAC_DECODER_BENCHMARK_STAMP)
# Remove the closed AirPlay library's redundant full-frame handover memcpy.
# Phase B retains the temporary allocation until queue publication is proven,
# then frees it immediately.  The build creates derived archives whose hooks
# are redirected only in AirPlayScreen.o and AirPlayReceiverSessionScreen.o;
# the supplied vendor archives are never modified.
VIDEO_HANDOVER_ZERO_COPY ?= 1
VIDEO_HANDOVER_ZERO_COPY_MIN_BYTES ?= 4096
# Bound queued plus actively-sent screen frames before entering the closed
# AirPlayScreen mutex.  This lets downstream TCP/USB pressure stop the receiver
# callback and naturally close the upstream TCP window toward the iPhone.
VIDEO_HANDOVER_BACKPRESSURE ?= 1
VIDEO_HANDOVER_MAX_INFLIGHT ?= 2
VIDEO_HANDOVER_GATE_POLL_TICKS ?= 1
# Closed AirPlay normal-frame sender optimization. Its payload memcpy is
# deferred only after object-local allocation/header/layout validation, then
# AES or ChaCha writes ciphertext directly into wireBuffer + 128.
# The direct path has passed frame correctness, ownership and long-run tests.
SCREEN_TX_DIRECT_CRYPTO ?= 1
SCREEN_TX_DIRECT_CRYPTO_MIN_BYTES ?= 4096
# Match the ChaCha rollout convention: 0=software only,
# 1=software-authoritative with hardware shadow comparison, 2=hardware.
AES_MODE ?= 2
# Compiler command-line changes are not part of GNU Make's normal timestamp
# dependency model.  Keep exactly one mode stamp so switching AES_MODE forces
# the selector object (and only that object) to be rebuilt.
AES_MODE_STAMP := $(OBJ_DIR)/.aes_mode_$(AES_MODE)
$(AES_MODE_STAMP):
	@mkdir -p $(OBJ_DIR)
	@rm -f $(OBJ_DIR)/.aes_mode_*
	@touch $@
../src/carbox/aes_backend_select.o: $(AES_MODE_STAMP)
# Video-only TCP no-copy bring-up.  The legacy socket API remains COPY; only a
# wire buffer proven by the direct-crypto transaction uses ACK-owned pbuf refs.
TCP_OWNED_WRITE ?= 1
# Report the ACK lifetime of video buffers independently of the retired full
# screen queue profiler.  This is the direct measure of queued video latency.
TCP_OWNED_AGE_PROFILE ?= 0
# Keep the expensive, already-concluded diagnostic probes independently
# switchable.  The normal screen timing/backlog profiler does not need them.
SCREEN_FRAME_FORMAT_PROFILE ?= 0
SCREEN_TCP_BUFFER_PROFILE ?= 0
# When SCREEN_QUEUE_PROFILE is enabled, correlate the local tick used to
# generate each outgoing screen NTP timestamp with that frame's receive time.
SCREEN_TIMESTAMP_PROFILE ?= 0
USB_HCD_PROFILE ?= 0
# Narrow NCM channel submit-size diagnostic.  This wraps only HCD submit and
# is intentionally independent of the older verbose USB_HCD_PROFILE.
USB_HCD_CHANNEL_PROFILE ?= 0
NET_QUEUE_PROFILE ?= 0
# Stage 1 validates the preallocated pbuf-pointer mailbox path without
# aggregation, delay, or GTimer.  Each WLAN packet is still posted immediately.
TCPIP_RX_BATCH_STAGE1 ?= 1
# Stage 2 arms an independent 1 ms one-shot GTimer under WLAN traffic. Its
# callback records timing only; it never touches pbufs or the TCP/IP mailbox.
TCPIP_RX_BATCH_TIMER_PROBE ?= 1
# Stage 2B validates GTimer ISR -> TCP/IP mailbox dispatch using a fixed
# diagnostic message. No packet pointer is carried by this message.
TCPIP_RX_BATCH_TIMER_MBOX_PROBE ?= 1
# Stage 3 enables the first real aggregation path after Stage 1/2/2B have
# independently validated ownership, timer timing, and ISR mailbox dispatch.
TCPIP_RX_BATCH_STAGE3 ?= 1
TCPIP_RX_BATCH_MAX_PACKETS ?= 8
TCPIP_RX_BATCH_TIMEOUT_US ?= 1000
# Diagnostic counters/logs only; aggregation remains enabled when this is 0.
TCPIP_RX_BATCH_PROFILE ?= 0
CRYPTO_ENGINE_PROFILE ?= 0
CARBOX_CRYPTO_OWNER_BOOST_PRIORITY ?= 11
SYS_PLL_OVERCLOCK ?= 0
SYS_PLL_TARGET_HZ ?= 300000000
GCCFLAGS += -DCONFIG_NET_GDMA_COPY=$(NET_GDMA_COPY)
GCCFLAGS += -DCONFIG_NET_GDMA_BENCH=$(NET_GDMA_BENCH)
GCCFLAGS += -DCONFIG_NET_GDMA_STATS=$(NET_GDMA_STATS)
GCCFLAGS += -DNET_GDMA_OWNER_BOOST_PRIORITY=$(NET_GDMA_OWNER_BOOST_PRIORITY)
GCCFLAGS += -DCONFIG_WLAN_RX_SWAP_BRINGUP_PROFILE=$(WLAN_RX_SWAP_BRINGUP_PROFILE)
GCCFLAGS += -DCONFIG_WLAN_RX_RING_SWAP=$(WLAN_RX_RING_SWAP)
GCCFLAGS += -DCONFIG_TCP_PHASE_PROFILE=$(TCP_PHASE_PROFILE)
GCCFLAGS += -DCONFIG_TCP_CORE_PHASE_PROFILE=$(TCP_CORE_PHASE_PROFILE)
GCCFLAGS += -DCONFIG_TCP_OUTPUT_PROFILE=$(TCP_OUTPUT_PROFILE)
GCCFLAGS += -DCONFIG_NCM_TX_PROFILE=$(NCM_TX_PROFILE)
GCCFLAGS += -DCONFIG_NCM_TX_ASYNC=$(NCM_TX_ASYNC)
GCCFLAGS += -DCONFIG_NCM_TX_ASYNC_PROFILE=$(NCM_TX_ASYNC_PROFILE)
GCCFLAGS += -DCONFIG_NCM_TX_BATCH=$(NCM_TX_BATCH)
GCCFLAGS += -DCONFIG_NCM_TX_BATCH_MAX_PACKETS=$(NCM_TX_BATCH_MAX_PACKETS)
GCCFLAGS += -DCONFIG_NCM_TX_BATCH_MAX_BYTES=$(NCM_TX_BATCH_MAX_BYTES)
GCCFLAGS += -DCONFIG_NCM_TX_BATCH_TIMEOUT_MS=$(NCM_TX_BATCH_TIMEOUT_MS)
GCCFLAGS += -DCONFIG_NCM_TX_BATCH_GDMA=$(NCM_TX_BATCH_GDMA)
GCCFLAGS += -DCONFIG_PC_PROFILER=$(PC_PROFILER)
GCCFLAGS += -DCONFIG_PC_PROFILER_PC_DETAIL=$(PC_PROFILER_PC_DETAIL)
GCCFLAGS += -DCONFIG_PC_PROFILER_RTW_RECV_DETAIL=$(PC_PROFILER_RTW_RECV_DETAIL)
GCCFLAGS += -DCONFIG_PC_PROFILER_RTW_DUMP_PROFILE=$(PC_PROFILER_RTW_DUMP_PROFILE)
GCCFLAGS += -DCONFIG_IRQ_PROFILE=$(IRQ_PROFILE)
GCCFLAGS += -DCONFIG_IRQ_PROFILE_REPORT=$(IRQ_PROFILE_REPORT)
GCCFLAGS += -DCONFIG_IRQ_PROFILE_USB_CAUSE=$(IRQ_PROFILE_USB_CAUSE)
GCCFLAGS += -DCONFIG_IRQ_PROFILE_USB_HANDOFF=$(IRQ_PROFILE_USB_HANDOFF)
GCCFLAGS += -DCONFIG_IRQ_PROFILE_USB_CH4_FLOW=$(IRQ_PROFILE_USB_CH4_FLOW)
GCCFLAGS += -DCONFIG_IRQ_PROFILE_USB_CH4_NCM=$(IRQ_PROFILE_USB_CH4_NCM)
GCCFLAGS += -DCONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE=$(IRQ_PROFILE_USB_CH4_SEQUENCE)
GCCFLAGS += -DCONFIG_USB_IRQ_SAFE_DEDUP=$(USB_IRQ_SAFE_DEDUP)
GCCFLAGS += -DCONFIG_USB_IRQ_CH3_ACK_FASTPATH=$(USB_IRQ_CH3_ACK_FASTPATH)
GCCFLAGS += -DCONFIG_USB_IRQ_CH4_NAK_COALESCE=$(USB_IRQ_CH4_NAK_COALESCE)
GCCFLAGS += -DUSB_IRQ_CH4_NAK_COALESCE_TIMEOUT_US=$(USB_IRQ_CH4_NAK_COALESCE_TIMEOUT_US)
GCCFLAGS += -DCONFIG_USB_IRQ_CLEAR_STALE_PENDING=$(USB_IRQ_CLEAR_STALE_PENDING)
GCCFLAGS += -DCONFIG_MEMCPY_TASK_PROFILE=$(MEMCPY_TASK_PROFILE)
GCCFLAGS += -DCONFIG_LARGE_MEMCPY_GDMA=$(LARGE_MEMCPY_GDMA)
GCCFLAGS += -DLARGE_MEMCPY_GDMA_THRESHOLD=$(LARGE_MEMCPY_GDMA_THRESHOLD)
GCCFLAGS += -DLARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY=$(LARGE_MEMCPY_GDMA_OWNER_BOOST_PRIORITY)
GCCFLAGS += -DLARGE_MEMCPY_GDMA_COPYV_VERIFY=$(LARGE_MEMCPY_GDMA_COPYV_VERIFY)
GCCFLAGS += -DCONFIG_SOCKET_RECV_PROFILE=$(SOCKET_RECV_PROFILE)
GCCFLAGS += -DCONFIG_SOCKET_RECV_GDMA=$(SOCKET_RECV_GDMA)
GCCFLAGS += -DCONFIG_SOCKET_RECV_GDMA_PROFILE=$(SOCKET_RECV_GDMA_PROFILE)
GCCFLAGS += -DSOCKET_RECV_GDMA_THRESHOLD=$(SOCKET_RECV_GDMA_THRESHOLD)
GCCFLAGS += -DCONFIG_TCP_SOCKET_HANDOFF_PROFILE=$(TCP_SOCKET_HANDOFF_PROFILE)
GCCFLAGS += -DCONFIG_GDMA_RESERVE_MULTIBLOCK_CHANNELS=$(GDMA_RESERVE_MULTIBLOCK_CHANNELS)
GCCFLAGS += -DCONFIG_GCD_SYNC_PROFILE=$(GCD_SYNC_PROFILE)
GCCFLAGS += -DCONFIG_GCD_WORK_PRIORITY=$(GCD_WORK_PRIORITY)
GCCFLAGS += -DCONFIG_USBH_ISR_TASK_PRIORITY=$(USBH_ISR_TASK_PRIORITY)
GCCFLAGS += -DCONFIG_USBH_MAIN_TASK_PRIORITY=$(USBH_MAIN_TASK_PRIORITY)
GCCFLAGS += -DCONFIG_SCREEN_QUEUE_PROFILE=$(SCREEN_QUEUE_PROFILE)
GCCFLAGS += -DCONFIG_SCREEN_DATAPATH_PROFILE=$(SCREEN_DATAPATH_PROFILE)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT=$(SCREEN_RX_RATE_LIMIT)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_BPS=$(SCREEN_RX_RATE_LIMIT_BPS)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_INTERVAL_MS=$(SCREEN_RX_RATE_LIMIT_INTERVAL_MS)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_BUCKET_BYTES=$(SCREEN_RX_RATE_LIMIT_BUCKET_BYTES)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_CONTROL_MS=$(SCREEN_RX_RATE_LIMIT_CONTROL_MS)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_DEADBAND_PERCENT=$(SCREEN_RX_RATE_LIMIT_DEADBAND_PERCENT)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_FILTER_SHIFT=$(SCREEN_RX_RATE_LIMIT_FILTER_SHIFT)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_VALVE_MIN_BPS=$(SCREEN_RX_RATE_LIMIT_VALVE_MIN_BPS)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_VALVE_MAX_BPS=$(SCREEN_RX_RATE_LIMIT_VALVE_MAX_BPS)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_OPEN_STEP_BPS=$(SCREEN_RX_RATE_LIMIT_OPEN_STEP_BPS)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_CLOSE_STEP_BPS=$(SCREEN_RX_RATE_LIMIT_CLOSE_STEP_BPS)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_OPEN_HOLD_MS=$(SCREEN_RX_RATE_LIMIT_OPEN_HOLD_MS)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_CLOSE_HOLD_MS=$(SCREEN_RX_RATE_LIMIT_CLOSE_HOLD_MS)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_TASK_PRIORITY=$(SCREEN_RX_RATE_LIMIT_TASK_PRIORITY)
GCCFLAGS += -DCONFIG_SCREEN_RX_RATE_LIMIT_TASK_STACK=$(SCREEN_RX_RATE_LIMIT_TASK_STACK)
GCCFLAGS += -DCONFIG_SCREEN_TX_PACER=$(SCREEN_TX_PACER)
GCCFLAGS += -DCONFIG_SCREEN_TX_PACER_BPS=$(SCREEN_TX_PACER_BPS)
GCCFLAGS += -DCONFIG_SCREEN_TX_PACER_BUCKET_BYTES=$(SCREEN_TX_PACER_BUCKET_BYTES)
GCCFLAGS += -DCONFIG_SCREEN_TX_PACER_CHUNK_BYTES=$(SCREEN_TX_PACER_CHUNK_BYTES)
GCCFLAGS += -DCONFIG_SCREEN_TX_PACER_WAIT_MS=$(SCREEN_TX_PACER_WAIT_MS)
GCCFLAGS += -DCONFIG_SCREEN_TX_PRESSURE_FEEDBACK=$(SCREEN_TX_PRESSURE_FEEDBACK)
GCCFLAGS += -DCONFIG_SCREEN_TX_PRESSURE_TRIGGER_MS=$(SCREEN_TX_PRESSURE_TRIGGER_MS)
GCCFLAGS += -DCONFIG_SCREEN_TX_PRESSURE_CLEAN_MS=$(SCREEN_TX_PRESSURE_CLEAN_MS)
GCCFLAGS += -DCONFIG_SCREEN_BLOCK_PROFILE=$(SCREEN_BLOCK_PROFILE)
GCCFLAGS += -DCONFIG_SCREEN_TCP_ACK_PROFILE=$(SCREEN_TCP_ACK_PROFILE)
GCCFLAGS += -DCONFIG_AUDIO_DECODE_PROFILE=$(AUDIO_DECODE_PROFILE)
GCCFLAGS += -DAUDIO_DECODE_PROFILE_WINDOW_MS=$(AUDIO_DECODE_PROFILE_WINDOW_MS)
GCCFLAGS += -DAUDIO_DECODE_PROFILE_SLOW_US=$(AUDIO_DECODE_PROFILE_SLOW_US)
GCCFLAGS += -DCARBOX_AAC_DECODER_MODE=$(AAC_DECODER_MODE)
GCCFLAGS += -DAAC_DECODER_ROUTE_PROFILE_WINDOW_MS=$(AAC_DECODER_ROUTE_PROFILE_WINDOW_MS)
GCCFLAGS += -DCONFIG_AAC_DECODER_BENCHMARK=$(AAC_DECODER_BENCHMARK)
GCCFLAGS += -DAAC_DECODER_BENCHMARK_LOOPS=$(AAC_DECODER_BENCHMARK_LOOPS)
GCCFLAGS += -DAAC_DECODER_BENCHMARK_START_DELAY_MS=$(AAC_DECODER_BENCHMARK_START_DELAY_MS)
GCCFLAGS += -DAAC_DECODER_BENCHMARK_REPORT_MS=$(AAC_DECODER_BENCHMARK_REPORT_MS)
GCCFLAGS += -DAAC_DECODER_BENCHMARK_TASK_PRIORITY=$(AAC_DECODER_BENCHMARK_TASK_PRIORITY)
GCCFLAGS += -DAAC_DECODER_BENCHMARK_RUN_PRIORITY=$(AAC_DECODER_BENCHMARK_RUN_PRIORITY)
GCCFLAGS += -DAAC_DECODER_BENCHMARK_TASK_STACK=$(AAC_DECODER_BENCHMARK_TASK_STACK)
GCCFLAGS += -DCONFIG_VIDEO_HANDOVER_ZERO_COPY=$(VIDEO_HANDOVER_ZERO_COPY)
GCCFLAGS += -DVIDEO_HANDOVER_ZERO_COPY_MIN_BYTES=$(VIDEO_HANDOVER_ZERO_COPY_MIN_BYTES)
GCCFLAGS += -DCONFIG_VIDEO_HANDOVER_BACKPRESSURE=$(VIDEO_HANDOVER_BACKPRESSURE)
GCCFLAGS += -DVIDEO_HANDOVER_MAX_INFLIGHT=$(VIDEO_HANDOVER_MAX_INFLIGHT)
GCCFLAGS += -DVIDEO_HANDOVER_GATE_POLL_TICKS=$(VIDEO_HANDOVER_GATE_POLL_TICKS)
GCCFLAGS += -DCONFIG_SCREEN_TX_DIRECT_CRYPTO=$(SCREEN_TX_DIRECT_CRYPTO)
GCCFLAGS += -DCONFIG_AES_MODE=$(AES_MODE)
GCCFLAGS += -DSCREEN_TX_DIRECT_CRYPTO_MIN_BYTES=$(SCREEN_TX_DIRECT_CRYPTO_MIN_BYTES)
GCCFLAGS += -DCONFIG_TCP_OWNED_WRITE=$(TCP_OWNED_WRITE)
GCCFLAGS += -DCONFIG_TCP_OWNED_AGE_PROFILE=$(TCP_OWNED_AGE_PROFILE)
GCCFLAGS += -DCONFIG_SCREEN_FRAME_FORMAT_PROFILE=$(SCREEN_FRAME_FORMAT_PROFILE)
GCCFLAGS += -DCONFIG_SCREEN_TCP_BUFFER_PROFILE=$(SCREEN_TCP_BUFFER_PROFILE)
GCCFLAGS += -DCONFIG_SCREEN_TIMESTAMP_PROFILE=$(SCREEN_TIMESTAMP_PROFILE)
GCCFLAGS += -DCONFIG_USB_HCD_PROFILE=$(USB_HCD_PROFILE)
GCCFLAGS += -DCONFIG_USB_HCD_CHANNEL_PROFILE=$(USB_HCD_CHANNEL_PROFILE)
GCCFLAGS += -DCONFIG_NET_QUEUE_PROFILE=$(NET_QUEUE_PROFILE)
GCCFLAGS += -DCONFIG_TCPIP_RX_BATCH_STAGE1=$(TCPIP_RX_BATCH_STAGE1)
GCCFLAGS += -DCONFIG_TCPIP_RX_BATCH_TIMER_PROBE=$(TCPIP_RX_BATCH_TIMER_PROBE)
GCCFLAGS += -DCONFIG_TCPIP_RX_BATCH_TIMER_MBOX_PROBE=$(TCPIP_RX_BATCH_TIMER_MBOX_PROBE)
GCCFLAGS += -DCONFIG_TCPIP_RX_BATCH_STAGE3=$(TCPIP_RX_BATCH_STAGE3)
GCCFLAGS += -DCONFIG_TCPIP_RX_BATCH_MAX_PACKETS=$(TCPIP_RX_BATCH_MAX_PACKETS)
GCCFLAGS += -DCONFIG_TCPIP_RX_BATCH_TIMEOUT_US=$(TCPIP_RX_BATCH_TIMEOUT_US)
GCCFLAGS += -DCONFIG_TCPIP_RX_BATCH_PROFILE=$(TCPIP_RX_BATCH_PROFILE)
GCCFLAGS += -DCONFIG_CRYPTO_ENGINE_PROFILE=$(CRYPTO_ENGINE_PROFILE)
GCCFLAGS += -DCARBOX_CRYPTO_OWNER_BOOST_PRIORITY=$(CARBOX_CRYPTO_OWNER_BOOST_PRIORITY)
GCCFLAGS += -DCONFIG_SYS_PLL_OVERCLOCK=$(SYS_PLL_OVERCLOCK)
GCCFLAGS += -DCONFIG_SYS_PLL_TARGET_HZ=$(SYS_PLL_TARGET_HZ)
# Avoid FreeRTOS-Plus-POSIX vs newlib type conflicts (mode_t, clockid_t, timer_t)
GCCFLAGS += -DposixconfigENABLE_MODE_T=0
GCCFLAGS += -DposixconfigENABLE_CLOCKID_T=0
GCCFLAGS += -DposixconfigENABLE_TIMER_T=0
GCCFLAGS += -DCONFIG_FATFS
#GCCFLAGS += -DCONFIG_UART2_TEST
#GCCFLAGS += -DCONFIG_FATFS_TEST
GCCFLAGS += -DCONFIG_CARBOX_WIFI_STA_AP_COMPAT
GCCFLAGS += -DCONFIG_MEMCHECK
ifeq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
GCCFLAGS += -DCARBOX_EXPERIMENTAL_SMART_A_LINK=1
endif

ifeq ($(CARBOX_EXPERIMENTAL_USB),1)
GCCFLAGS += -DCARBOX_EXPERIMENTAL_USB=1
GCCFLAGS += -DCONFIG_USBH_CDC_NCM=1
GCCFLAGS += -DCONFIG_ADD_LOG=1
endif



CFLAGS = $(GCCFLAGS)
CFLAGS += -Wall -Wpointer-arith -Wstrict-prototypes -Wundef -Wno-write-strings -Wno-maybe-uninitialized
CFLAGS += -w

# Compile only measured lwIP hot paths for throughput.  Keeping the rest at
# -Os limits ITCM growth; TCP_PHASE_PROFILE can be disabled after board data
# identifies the stable bottleneck.
LWIP_HOT_OPT_FLAGS ?= -O3
LWIP_HOT_O3_O := \
	../../../component/common/network/lwip/lwip_v2.1.2/src/core/inet_chksum.o \
	../../../component/common/network/lwip/lwip_v2.1.2/src/core/pbuf.o \
	../../../component/common/network/lwip/lwip_v2.1.2/src/core/tcp.o \
	../../../component/common/network/lwip/lwip_v2.1.2/src/core/tcp_in.o \
	../../../component/common/network/lwip/lwip_v2.1.2/src/core/tcp_out.o \
	../../../component/common/network/lwip/lwip_v2.1.2/src/netif/ethernet.o
$(LWIP_HOT_O3_O): CFLAGS := $(filter-out -Os,$(CFLAGS)) $(LWIP_HOT_OPT_FLAGS)

CPPFLAGS = $(GCCFLAGS)
CPPFLAGS += -std=c++11 -fno-use-cxa-atexit
CPPFLAGS += -w
CPPFLAGS += -Wall -Wpointer-arith -Wundef -Wno-write-strings -Wno-maybe-uninitialized

LFLAGS = 
LFLAGS += -march=armv8-m.main+dsp -mthumb -mcmse -mfloat-abi=softfp -mfpu=fpv5-sp-d16 -Os -nostartfiles -specs=nosys.specs -nodefaultlibs -nostdlib
LFLAGS += -Wl,--gc-sections -Wl,-Map=$(BIN_DIR)/$(TARGET).map -Wl,--cref -Wl,--build-id=none -Wl,--use-blx 
ifeq ($(PC_PROFILER_RTW_DUMP_PROFILE),1)
LFLAGS += -Wl,--wrap=rtw_enter_critical -Wl,--wrap=rtw_exit_critical \
	-Wl,--wrap=clean_cache_wlan -Wl,--wrap=rtl8195b_update_txdesc
endif
ifneq ($(filter 1,$(USB_IRQ_SAFE_DEDUP) $(USB_IRQ_CLEAR_STALE_PENDING)),)
LFLAGS += -Wl,--wrap=usb_hal_enable_interrupt
endif
ifneq ($(filter 1,$(USB_IRQ_CH3_ACK_FASTPATH) $(USB_IRQ_CH4_NAK_COALESCE)),)
LFLAGS += -Wl,--wrap=usb_hal_read_interrupts
endif
ifeq ($(IRQ_PROFILE_USB_CH4_FLOW),1)
LFLAGS += -Wl,--wrap=usbh_hal_hc_halt
endif
ifeq ($(IRQ_PROFILE_USB_CH4_NCM),1)
LFLAGS += -Wl,--wrap=usbh_ctrl_request -Wl,--wrap=ncm_receive_buf_size
endif
ifeq ($(GCD_SYNC_PROFILE),1)
LFLAGS += -Wl,--wrap=dispatch_sync_f
endif
ifneq ($(strip $(filter-out -1,$(GCD_WORK_PRIORITY) $(USBH_ISR_TASK_PRIORITY) $(USBH_MAIN_TASK_PRIORITY))),)
LFLAGS += -Wl,--wrap=xTaskCreate
endif
ifeq ($(NCM_TX_BATCH),1)
LFLAGS += -Wl,--wrap=ncm_wrap_ntb
endif
ifeq ($(SCREEN_QUEUE_PROFILE),1)
LFLAGS += -Wl,--wrap=AirPlayScreen_SendVideo
LFLAGS += -Wl,--wrap=CVector_push_back -Wl,--wrap=CVector_erase
LFLAGS += -Wl,--wrap=CVector_delete
LFLAGS += -Wl,--wrap=lwip_recv -Wl,--wrap=lwip_write
LFLAGS += -Wl,--wrap=lwip_select
ifeq ($(SCREEN_TIMESTAMP_PROFILE),1)
LFLAGS += -Wl,--wrap=UpTicksToNTP
endif
endif
ifeq ($(SCREEN_RX_RATE_LIMIT),1)
ifeq ($(SCREEN_QUEUE_PROFILE),0)
LFLAGS += -Wl,--wrap=lwip_recv
endif
LFLAGS += -Wl,--wrap=lwip_close
endif
ifeq ($(VIDEO_HANDOVER_ZERO_COPY),1)
# In a non-profile build, retain only the two functional wrappers required for
# exact transaction scoping and atomic CVector pointer publication.
ifeq ($(SCREEN_QUEUE_PROFILE),0)
LFLAGS += -Wl,--wrap=AirPlayScreen_SendVideo
LFLAGS += -Wl,--wrap=CVector_push_back
endif
endif
LFLAGS += -Wl,--wrap=AES_CTR_Init -Wl,--wrap=AES_CTR_Update
LFLAGS += -Wl,--wrap=AES_CTR_Final
ifeq ($(SCREEN_TX_DIRECT_CRYPTO),1)
ifeq ($(SCREEN_QUEUE_PROFILE),0)
LFLAGS += -Wl,--wrap=lwip_write
endif
endif
ifeq ($(USB_HCD_PROFILE),1)
LFLAGS += -Wl,--wrap=usbh_hcd_hc_submit_request
LFLAGS += -Wl,--wrap=usbh_hcd_hc_get_urb_state
LFLAGS += -Wl,--wrap=usbh_hcd_hc_get_transfer_size
endif
ifeq ($(USB_HCD_CHANNEL_PROFILE),1)
ifneq ($(USB_HCD_PROFILE),1)
LFLAGS += -Wl,--wrap=usbh_hcd_hc_submit_request
endif
endif
ifeq ($(CRYPTO_ENGINE_PROFILE),1)
LFLAGS += -Wl,--wrap=device_mutex_lock -Wl,--wrap=device_mutex_unlock
LFLAGS += -Wl,--wrap=rtw_down_timeout_sema -Wl,--wrap=rtw_up_sema_from_isr
LFLAGS += -Wl,--wrap=g_crypto_handler
LFLAGS += -Wl,--wrap=crypto_aes_ecb_encrypt -Wl,--wrap=crypto_aes_ecb_decrypt
LFLAGS += -Wl,--wrap=crypto_aes_cbc_encrypt -Wl,--wrap=crypto_aes_cbc_decrypt
LFLAGS += -Wl,--wrap=crypto_aes_ctr_encrypt -Wl,--wrap=crypto_aes_ctr_decrypt
LFLAGS += -Wl,--wrap=crypto_aes_gcm_encrypt -Wl,--wrap=crypto_aes_gcm_decrypt
LFLAGS += -Wl,--wrap=rtl_crypto_chacha_encrypt -Wl,--wrap=rtl_crypto_chacha_decrypt
LFLAGS += -Wl,--wrap=rtl_crypto_poly1305 -Wl,--wrap=rtl_crypto_poly1305_process
LFLAGS += -Wl,--wrap=rtl_crypto_chacha_poly1305_encrypt
LFLAGS += -Wl,--wrap=rtl_crypto_chacha_poly1305_decrypt
endif
ifeq ($(FDK_AAC_PROFILE),1)
LFLAGS += -Wl,--wrap=aacEncEncode -Wl,--wrap=aacDecoder_DecodeFrame
endif
ifeq ($(AUDIO_DECODE_PROFILE),1)
LFLAGS += -Wl,--wrap=AudioConverterFillComplexBuffer
endif
# FDK-compatible decoder router. Encoder APIs are intentionally absent here.
LFLAGS += -Wl,--wrap=aacDecoder_Open -Wl,--wrap=aacDecoder_ConfigRaw
LFLAGS += -Wl,--wrap=aacDecoder_Fill -Wl,--wrap=aacDecoder_DecodeFrame
LFLAGS += -Wl,--wrap=aacDecoder_SetParam -Wl,--wrap=aacDecoder_Close
##noisy warning option
##LFLAGS += -Wl,--warn-section-align
# libc api wrapper
LFLAGS += -Wl,-wrap,strcat  -Wl,-wrap,strchr   -Wl,-wrap,strcmp
LFLAGS += -Wl,-wrap,strncmp -Wl,-wrap,strnicmp -Wl,-wrap,strcpy
LFLAGS += -Wl,-wrap,strncpy -Wl,-wrap,strlcpy  -Wl,-wrap,strlen
LFLAGS += -Wl,-wrap,strnlen -Wl,-wrap,strncat  -Wl,-wrap,strpbrk
LFLAGS += -Wl,-wrap,strspn  -Wl,-wrap,strstr   -Wl,-wrap,strtok
LFLAGS += -Wl,-wrap,strxfrm -Wl,-wrap,strsep   -Wl,-wrap,strtod
LFLAGS += -Wl,-wrap,strtof  -Wl,-wrap,strtold  -Wl,-wrap,strtoll
LFLAGS += -Wl,-wrap,strtoul -Wl,-wrap,strtoull -Wl,-wrap,atoi
LFLAGS += -Wl,-wrap,atoui   -Wl,-wrap,atol     -Wl,-wrap,atoul
LFLAGS += -Wl,-wrap,atoull  -Wl,-wrap,atof
LFLAGS += -Wl,-wrap,malloc  -Wl,-wrap,realloc  -Wl,-wrap,free
LFLAGS += -Wl,-wrap,calloc
LFLAGS += -Wl,-wrap,_malloc_r -Wl,-wrap,_realloc_r
LFLAGS += -Wl,-wrap,_calloc_r -Wl,-wrap,_free_r
LFLAGS += -Wl,-wrap,memcmp  -Wl,-wrap,memcpy
LFLAGS += -Wl,-wrap,memmove -Wl,-wrap,memset
LFLAGS += -Wl,-wrap,printf  -Wl,-wrap,sprintf
LFLAGS += -Wl,-wrap,snprintf  -Wl,-wrap,vsnprintf  -Wl,-wrap,vprintf
LFLAGS += -Wl,-wrap,asprintf
LFLAGS += -Wl,-wrap,strdup
LFLAGS += -Wl,-wrap,abort     -Wl,-wrap,puts
LFLAGS += -Wl,-wrap,fopen     -Wl,-wrap,fclose
LFLAGS += -Wl,-wrap,fread     -Wl,-wrap,fwrite
LFLAGS += -Wl,-wrap,fseek     -Wl,-wrap,fflush
LFLAGS += -Wl,-wrap,rename    -Wl,-wrap,feof
LFLAGS += -Wl,-wrap,ferror    -Wl,-wrap,ftell
LFLAGS += -Wl,-wrap,fputc     -Wl,-wrap,fputs
LFLAGS += -Wl,-wrap,fgets     -Wl,-wrap,remove
LFLAGS += -Wl,-wrap,opendir   -Wl,-wrap,readdir
LFLAGS += -Wl,-wrap,closedir  -Wl,-wrap,scandir
LFLAGS += -Wl,-wrap,rmdir     -Wl,-wrap,mkdir
LFLAGS += -Wl,-wrap,access    -Wl,-wrap,stat
LFLAGS += -Wl,-wrap,aesccmp_construct_mic_iv
LFLAGS += -Wl,-wrap,aesccmp_construct_mic_header1
LFLAGS += -Wl,-wrap,aesccmp_construct_ctr_preload
LFLAGS += -Wl,-wrap,rom_psk_CalcGTK
LFLAGS += -Wl,-wrap,rom_psk_CalcPTK
LFLAGS += -Wl,-wrap,aes_80211_encrypt
LFLAGS += -Wl,-wrap,aes_80211_decrypt
LFLAGS += -Wl,-wrap,usb_os_sema_give


LIBFLAGS =
LIBFLAGS += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/lib/lib/hal_pmc_hs.a
LIBFLAGS += -L../../../component/soc/realtek/8195b/misc/bsp/lib/common/GCC/
WLAN_RX_HOOK_DIR := ../../../component/common/drivers/wlan/realtek/wlan_rx_gdma
WLAN_RX_HOOK_ARCHIVE := $(WLAN_RX_HOOK_DIR)/build/lib_wlan_rx_gdma.a
WLAN_RX_HOOK_ORIGINAL := ../../../component/soc/realtek/8195b/misc/bsp/lib/common/GCC/lib_wlan.a
# lib_rtk264 is the standalone RTL8195B H.264 encoder wrapper.  It requires
# lib_h264 plus the VOE/ISP video-subsystem HAL; keep all of them disabled as a
# unit until the encoder has a real consumer and is validated on hardware.
CARBOX_BUILD_RTK264 ?= 0
CARBOX_RTK264_OPT_FLAGS ?= -O3
CARBOX_RTK264_DIR := ../src/carbox/rtk264
CARBOX_RTK264_BUILD_DIR := $(CARBOX_RTK264_DIR)/build
CARBOX_RTK264_SOURCE := $(CARBOX_RTK264_DIR)/lib_rtk264.c
CARBOX_RTK264_HEADER := $(CARBOX_RTK264_DIR)/lib_rtk264.h
CARBOX_RTK264_OBJECT := $(CARBOX_RTK264_BUILD_DIR)/lib_rtk264.o
CARBOX_RTK264_ARCHIVE := $(CARBOX_RTK264_BUILD_DIR)/lib_rtk264.a
ifneq ($(filter 1,$(NET_GDMA_COPY) $(WLAN_RX_SWAP_BRINGUP_PROFILE) $(WLAN_RX_RING_SWAP)),)
WLAN_RX_HOOK_LIBRARY := $(WLAN_RX_HOOK_ARCHIVE)
else
WLAN_RX_HOOK_LIBRARY := -l_wlan
endif
all: LIBFLAGS += -l_codec -l_dct -l_haac -l_hmp3 -l_http -l_mmf -l_muxer -l_p2p -l_rtsp -l_sdcard -l_soc_is -l_speex  -l_websocket $(WLAN_RX_HOOK_LIBRARY) -l_wps -l_qr_code -l_tftp -l_opusenc -l_opusfile -l_opus
mp: LIBFLAGS += -l_codec -l_dct -l_haac -l_hmp3 -l_http -l_mmf -l_muxer -l_p2p -l_rtsp -l_sdcard -l_soc_is -l_speex  -l_websocket -l_wlan_mp -l_wps -l_qr_code -l_tftp -l_opusenc -l_opusfile -l_opus
ifneq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
all mp: LIBFLAGS += -l_mdns
endif
ifneq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
all mp: LIBFLAGS += -l_faac
endif
all: LIBFLAGS += -lrtstream -lrtscamkit -lrtsv4l2
LIBFLAGS += -Wl,-u,ram_start -Wl,-u,cinit_start
ifeq ($(CARBOX_USB_LIB),1)
LIBFLAGS += -Wl,--whole-archive $(CARBOX_USB_ARCHIVE) -Wl,--no-whole-archive
endif

ifeq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
CARBOX_SMART_CARPLAY_LIB_DIR := carplay_app
CARBOX_CARPLAY_VENDOR_ARCHIVE := $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_CarPlay.a
CARBOX_CARPLAY_CHACHA_DIR := $(CARBOX_SMART_CARPLAY_LIB_DIR)/chacha_m33
CARBOX_CARPLAY_ARCHIVE := $(CARBOX_CARPLAY_CHACHA_DIR)/build/lib_CarPlay_chacha_m33.a
CARBOX_VIDEO_HANDOVER_PATCH := $(CARBOX_SMART_CARPLAY_LIB_DIR)/patch_video_handover_archive.sh
CARBOX_ACCESSORY2_VENDOR_ARCHIVE := $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_Accessory2.a
CARBOX_ACCESSORY2_HANDOVER_ARCHIVE := $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_Accessory2_handover.a
CARBOX_CARPLAY_HANDOVER_ARCHIVE := $(CARBOX_CARPLAY_CHACHA_DIR)/build/lib_CarPlay_chacha_m33_handover.a
# The vendor archive remains available for an immediate A/B fallback.  The O3
# archive is rebuilt from upstream FDK AAC v0.1.6 with the RTL8195B/M33 DSP ISA.
CARBOX_FDK_AAC_OPTIMIZED ?= 1
CARBOX_FDK_AAC_VENDOR_ARCHIVE := $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_fdkaac.a
CARBOX_FDK_AAC_O3_ARCHIVE := $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_fdkaac_o3.a
CARBOX_FDK_AAC_SOURCE_DIR := third_party/fdk-aac
ifeq ($(CARBOX_FDK_AAC_OPTIMIZED),1)
CARBOX_FDK_AAC_ARCHIVE := $(CARBOX_FDK_AAC_O3_ARCHIVE)
else
CARBOX_FDK_AAC_ARCHIVE := $(CARBOX_FDK_AAC_VENDOR_ARCHIVE)
endif
ifeq ($(CARBOX_FDK_AAC_OPTIMIZED),1)
.PHONY: carbox_fdkaac
carbox_fdkaac:
	$(MAKE) -C $(CARBOX_FDK_AAC_SOURCE_DIR) -f Makefile.rtl8195b \
		TOOLCHAIN_BIN=$(abspath $(ARM_GCC_TOOLCHAIN))
application: carbox_fdkaac
endif
$(CARBOX_CARPLAY_ARCHIVE): $(CARBOX_CARPLAY_VENDOR_ARCHIVE) \
		$(CARBOX_CARPLAY_CHACHA_DIR)/ChaCha20Poly1305.c \
		$(CARBOX_CARPLAY_CHACHA_DIR)/ChaCha20Poly1305.h \
		$(CARBOX_CARPLAY_CHACHA_DIR)/ChaCha20Poly1305_rtl8195b.c \
		../src/carbox/crypto_priority_lock.h \
		$(CARBOX_CARPLAY_CHACHA_DIR)/Makefile
	$(MAKE) -C $(CARBOX_CARPLAY_CHACHA_DIR) replacement \
		CHACHA_MODE=2 CHACHA_HW_MIN_LEN=4096 \
		SCREEN_TX_DIRECT_CRYPTO=$(SCREEN_TX_DIRECT_CRYPTO) \
		CHACHA_STATS_INTERVAL_MS=0 CHACHA_HW_SELFTEST=0 \
		CRYPTO_OWNER_BOOST_PRIORITY=$(CARBOX_CRYPTO_OWNER_BOOST_PRIORITY)
application: $(CARBOX_CARPLAY_ARCHIVE)
ifneq ($(filter 1,$(VIDEO_HANDOVER_ZERO_COPY) $(SCREEN_TX_DIRECT_CRYPTO)),)
$(CARBOX_ACCESSORY2_HANDOVER_ARCHIVE): $(CARBOX_ACCESSORY2_VENDOR_ARCHIVE) \
		$(CARBOX_VIDEO_HANDOVER_PATCH)
	sh $(CARBOX_VIDEO_HANDOVER_PATCH) accessory $(AR) $(OBJCOPY) \
		$(CARBOX_ACCESSORY2_VENDOR_ARCHIVE) $@ AirPlayScreen.o
application: $(CARBOX_ACCESSORY2_HANDOVER_ARCHIVE)
endif
ifeq ($(VIDEO_HANDOVER_ZERO_COPY),1)
$(CARBOX_CARPLAY_HANDOVER_ARCHIVE): $(CARBOX_CARPLAY_ARCHIVE) \
		$(CARBOX_VIDEO_HANDOVER_PATCH)
	sh $(CARBOX_VIDEO_HANDOVER_PATCH) receiver $(AR) $(OBJCOPY) \
		$(CARBOX_CARPLAY_ARCHIVE) $@ AirPlayReceiverSessionScreen.o

application: $(CARBOX_CARPLAY_HANDOVER_ARCHIVE)
endif
LFLAGS += -Wl,--no-warn-mismatch
LIBFLAGS += -Wl,--whole-archive
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_link.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_x264.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_png.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_SystemLib.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_SystemLibEx.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_UiLib.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_accessory.a
LIBFLAGS += $(CARBOX_FDK_AAC_ARCHIVE)
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_init.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_zlib.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_AndroidAuto.a
ifneq ($(filter 1,$(VIDEO_HANDOVER_ZERO_COPY) $(SCREEN_TX_DIRECT_CRYPTO)),)
LIBFLAGS += $(CARBOX_ACCESSORY2_HANDOVER_ARCHIVE)
else
LIBFLAGS += $(CARBOX_ACCESSORY2_VENDOR_ARCHIVE)
endif
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_jpeg.a
# LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_usbdev.a
# LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_ncm.a
LIBFLAGS += -Wl,--no-whole-archive
# Keep the customer's RTL8195B hardware backend, replacing only the protocol
# object so its internal copies use the measured M33 ITCM memcpy.  Leave this
# archive outside --whole-archive so unrelated members remain demand-linked.
ifeq ($(VIDEO_HANDOVER_ZERO_COPY),1)
LIBFLAGS += $(CARBOX_CARPLAY_HANDOVER_ARCHIVE)
else
LIBFLAGS += $(CARBOX_CARPLAY_ARCHIVE)
endif
endif


LIBFLAGS += -lm -lc -lnosys -lgcc -lstdc++

# Sensor
include sensor.mk

# Prebuild
# -------------------------------------------------------------------
	
.PHONY: prebuild
CARBOX_LINK_FW ?= 3

prebuild:	
	@echo ===========================================================
	@echo Prebuild
	@echo ===========================================================
	@echo "  ELF2BIN  prebuild GCC"
	@$(ELF2BIN) prebuild GCC xip_fw.ld FW >/dev/null 2>&1
	@# Two 0x370000 A/B slots: FW1 0x040000, FW2 0x3C0000.  The 64 KiB
	@# gap keeps FW2 on the 256 KiB boundary required by C-cut SCE page 2.
	@sed -i 's/__ICFEDIT_region_XIP_FW1_FLASH_end__.*=.*;/__ICFEDIT_region_XIP_FW1_FLASH_end__\t\t= 0x983B0220 ;/' xip_fw.ld
	@sed -i 's/__ICFEDIT_region_XIP_FW2_FLASH_start__.*=.*;/__ICFEDIT_region_XIP_FW2_FLASH_start__\t\t= 0x983C0220 ;/' xip_fw.ld
	@sed -i 's/__ICFEDIT_region_XIP_FW2_FLASH_end__.*=.*;/__ICFEDIT_region_XIP_FW2_FLASH_end__\t\t= 0x98730220 ;/' xip_fw.ld
	@# Select fixed-slot or slot-independent remap XIP linkage.  Keep this a
	@# build variable so a known-good fixed-slot recovery image remains easy
	@# to produce while remap-mode boot compatibility is being validated.
	@sed -i 's/linkFW = .*/linkFW = $(CARBOX_LINK_FW);/' amebapro_config_is.ld
	@sed -i 's/reserveVOE = .*/reserveVOE = 0;/' amebapro_config_is.ld
	@# Extend RAM region into RAM_SHARED (video off) — need ~352KB
	@sed -i 's/RAM_END = reserveVOE==1 ? 0x20124000 : 0x2013CC00;/RAM_END = 0x20160000;/' rtl8195bhp_ram_is.ld

ifeq ($(findstring Linux, $(OS)), Linux)
	@chmod 777 ../../../component/soc/realtek/8195b/misc/iar_utility/elf2bin.linux
	@chmod 777 ../../../component/soc/realtek/8195b/misc/iar_utility/checksum.linux
	@chmod 777 ../../../component/soc/realtek/8195b/misc/gcc_utility/postbuild.sh
	@chmod 777 ../../../component/soc/realtek/8195b/misc/gcc_utility/set_fw_json_serial.sh
	@if [ -d $(VFSDIR) ]; then \
		chmod 777 $(VFSDIR)/Version.txt; \
	fi
	@chmod 777 $(UTILITYDIR)/vfs.py
endif


# Compile
# -------------------------------------------------------------------

.PHONY: application
ifneq ($(filter 1,$(NET_GDMA_COPY) $(WLAN_RX_SWAP_BRINGUP_PROFILE) $(WLAN_RX_RING_SWAP)),)
.PHONY: wlan_rx_hook
wlan_rx_hook:
	@$(MAKE) -C $(WLAN_RX_HOOK_DIR) \
		CROSS_COMPILE=$(abspath $(CROSS_COMPILE)) \
		ORIGINAL_ARCHIVE=$(abspath $(WLAN_RX_HOOK_ORIGINAL))
application: wlan_rx_hook
endif
$(CARBOX_RTK264_OBJECT): $(CARBOX_RTK264_SOURCE) $(CARBOX_RTK264_HEADER)
	@mkdir -p $(CARBOX_RTK264_BUILD_DIR)
	@echo "  CC   $<"
	@$(CC) $(filter-out -Os,$(CFLAGS)) $(CARBOX_RTK264_OPT_FLAGS) \
		$(INCLUDES) -I$(CARBOX_RTK264_DIR) -c $< -o $@

$(CARBOX_RTK264_ARCHIVE): $(CARBOX_RTK264_OBJECT)
	@echo "  AR   $@"
	@$(AR) rcs $@ $<

.PHONY: carbox_rtk264
carbox_rtk264: $(CARBOX_RTK264_ARCHIVE)
	@$(NM) --defined-only $(CARBOX_RTK264_ARCHIVE) | \
		grep -q ' lib_rtk264_encode$$' || \
		{ echo "ERROR: lib_rtk264_encode missing from $@"; exit 1; }

ifeq ($(CARBOX_BUILD_RTK264),1)
application: carbox_rtk264
endif
application: prerequirement $(SRC_O) $(ERAM_O) $(SRAM_O) $(CINIT_O) $(ASM_O) $(ITCM_O) $(CPP_O)
# Fixup: when ram_lp runs first and creates .o in source tree, make skips
# our compile step but then cp to OBJ_DIR never happens.  Copy any
# orphaned .o into OBJ_DIR before linking.
	@for f in $(OBJ_LIST); do \
		if [ ! -f "$$f" ]; then \
			base=$$(basename "$$f"); \
			for src in $(SRC_O) $(ERAM_O) $(SRAM_O) $(CINIT_O) $(ITCM_O) $(DTCM_O) $(ASM_O) $(CPP_O); do \
				if [ "$$(basename "$$src")" = "$$base" ] && [ -f "$$src" ]; then \
					echo "  [fixup] cp $$src -> $$f"; \
					cp "$$src" "$$f"; \
					break; \
				fi; \
			done; \
			if [ ! -f "$$f" ]; then \
				echo "ERROR: Missing $$f and no upstream .o found"; \
				exit 1; \
			fi; \
		fi; \
	done
	@echo "  LD   linking..."
	@$(LD) $(LFLAGS) -o $(BIN_DIR)/$(TARGET).axf  $(OBJ_LIST) $(ROMIMG) $(LIBFLAGS) -T$(LDSCRIPT)
	@$(OBJDUMP) -d $(BIN_DIR)/$(TARGET).axf > $(BIN_DIR)/$(TARGET).asm



# Manipulate Image
# -------------------------------------------------------------------

CARBOX_FATFS_BIN ?= application_is/fatfs.bin
CARBOX_OTA_FATFS_BIN ?= application_is/ota_fatfs.bin
CARBOX_OTA_ALL_BIN ?= application_is/ota_all.bin
CARBOX_OTA_FW_VER ?= $(shell date +%y%m%d%H%M)
CARBOX_FW_SERIAL ?= $(shell date +%s)
CARBOX_RAW_SECTION_BIN ?= application_is/raw_section.bin
CARBOX_LP_AXF ?= application_lp/Debug/bin/application_lp.axf
CARBOX_FW_BIN ?= application_is/firmware_is.bin
# A valid CarBox image is currently about 3.8 MiB.  Keep the threshold well
# below that, but high enough to reject elf2bin's silent empty/4-byte output.
CARBOX_MIN_FIRMWARE_SIZE ?= 1048576
CARBOX_FLASH_LAYOUT_H := ../inc/carbox_flash_layout.h
CARBOX_FATFS_BASE := $(shell awk '/^\#define CARBOX_FATFS_BASE /{print $$3}' $(CARBOX_FLASH_LAYOUT_H))
CARBOX_FATFS_SIZE := $(shell awk '/^\#define CARBOX_FATFS_SIZE /{print $$3}' $(CARBOX_FLASH_LAYOUT_H))
CARBOX_RAW_SECTION_BASE := $(shell awk '/^\#define CARBOX_RAW_SECTION_BASE /{print $$3}' $(CARBOX_FLASH_LAYOUT_H))
CARBOX_RAW_SECTION_SIZE := $(shell awk '/^\#define CARBOX_RAW_SECTION_SIZE /{print $$3}' $(CARBOX_FLASH_LAYOUT_H))

partition.json: ../inc/carbox_flash_layout.h gen_partition_json.py
	@python3 gen_partition_json.py ../inc/carbox_flash_layout.h $@
 		
.PHONY: manipulate_images
manipulate_images:	partition.json | application
	@echo ===========================================================
	@echo Image manipulating
	@echo ===========================================================
	@if [ ! -s "$(CARBOX_LP_AXF)" ]; then \
		echo "ERROR: missing LP firmware input: $(CARBOX_LP_AXF)"; \
		echo "       Build ram_lp before ram_is (or run make all)."; \
		exit 1; \
	fi
	@if [ -d $(VFSDIR) ]; then \
		if [ "$(AAC_DECODER_BENCHMARK)" = "1" ]; then \
			test -f "$(AAC_DECODER_BENCHMARK_SAMPLE)" || { echo "ERROR: missing $(AAC_DECODER_BENCHMARK_SAMPLE)"; exit 1; }; \
			cp "$(AAC_DECODER_BENCHMARK_SAMPLE)" "$(VFSDIR)/bear-audio-lc-aac.aac"; \
		else \
			rm -f "$(VFSDIR)/bear-audio-lc-aac.aac"; \
		fi; \
		$(VFSTOOL) -t FATFS -s 512 -c $(FATFS_SECTORS) -dir $(VFSDIR) -out application_is/fatfs.bin >/dev/null 2>&1; \
	fi	
	@cp  ../../../component/soc/realtek/8195b/misc/bsp/image/boot.bin application_is/boot.bin
	@echo "  ELF2BIN  keygen keycfg.json"
	@$(ELF2BIN) keygen keycfg.json >/dev/null 2>&1
	@echo "  ELF2BIN  convert amebapro_bootloader.json"
	@$(ELF2BIN) convert amebapro_bootloader.json PARTITIONTABLE secure_bit=0 >/dev/null 2>&1
	@$(FLASH_TOOLDIR)/set_fw_json_serial.sh amebapro_firmware_is.json amebapro_firmware_is.serial.json $(CARBOX_FW_SERIAL) >/dev/null 2>&1
	@python3 filter_firmware_images.py amebapro_firmware_is.serial.json ISP WOWLANB WOWLANC
	@echo "  ELF2BIN  convert amebapro_firmware_is.serial.json"
	@$(ELF2BIN) convert amebapro_firmware_is.serial.json FIRMWARE secure_bit=0 >/dev/null 2>&1
	@if [ ! -f "$(CARBOX_FW_BIN)" ]; then \
		echo "ERROR: elf2bin did not create $(CARBOX_FW_BIN)"; \
		exit 1; \
	else \
		size=$$(stat -c %s "$(CARBOX_FW_BIN)"); \
		min=$$(( $(CARBOX_MIN_FIRMWARE_SIZE) )); \
		if [ $$size -lt $$min ]; then \
			echo "ERROR: invalid firmware image: $(CARBOX_FW_BIN) is $$size bytes (minimum $$min)"; \
			echo "       elf2bin may be missing application_lp.axf or required input sections."; \
			exit 1; \
		fi; \
		echo "  VERIFY $(CARBOX_FW_BIN): $$size bytes (minimum $$min)"; \
	fi
	@# Checksum the firmware before packing it into FW1.  OTA is generated
	@# from this exact same file, so the image validated in FW1 and the image
	@# later written into FW2 are byte-for-byte identical.
	@echo "  CHKSUM"
	@$(CHKSUM) application_is/firmware_is.bin >/dev/null 2>&1
	@cp ../../../component/soc/realtek/8195b/misc/bsp/image/bootloader.axf $(BOOT_BIN_DIR)/bootloader.axf
	@echo "  ELF2BIN  convert amebapro_bootloader.json"
	@$(ELF2BIN) convert amebapro_bootloader.json BOOTLOADER secure_bit=0 >/dev/null 2>&1
	@cp bootloader/boot.bin application_is/boot.bin
	@if [ "$(CARBOX_LINK_FW)" = "3" ]; then \
		echo "  ELF2BIN  combine application_is/flash_is.bin (remap bootstrap: FW1 only)"; \
		$(ELF2BIN) combine application_is/flash_is.bin PTAB=partition.bin,BOOT=application_is/boot.bin,FW1=application_is/firmware_is.bin >/dev/null 2>&1; \
	else \
		echo "  ELF2BIN  combine application_is/flash_is.bin (fixed XIP: FW1=FW2)"; \
		$(ELF2BIN) combine application_is/flash_is.bin PTAB=partition.bin,BOOT=application_is/boot.bin,FW1=application_is/firmware_is.bin,FW2=application_is/firmware_is.bin >/dev/null 2>&1; \
	fi
	@if grep -q '"fatfs"' partition.json && [ -f "$(CARBOX_FATFS_BIN)" ]; then \
		size=$$(stat -c %s "$(CARBOX_FATFS_BIN)"); \
		max=$$(( $(CARBOX_FATFS_SIZE) )); \
		if [ $$size -gt $$max ]; then \
			echo "ERROR: $(CARBOX_FATFS_BIN) size $$size exceeds fatfs partition size $$max"; \
			exit 1; \
		fi; \
		echo "Pack $(CARBOX_FATFS_BIN) -> application_is/flash_is.bin @$(CARBOX_FATFS_BASE) ($$size bytes)"; \
		dd if="$(CARBOX_FATFS_BIN)" of=application_is/flash_is.bin bs=4096 seek=$$(( $(CARBOX_FATFS_BASE) / 0x1000 )) conv=notrunc status=none; \
		python3 gen_ota_fatfs.py "$(CARBOX_FATFS_BIN)" $(CARBOX_OTA_FW_VER) "$(CARBOX_OTA_FATFS_BIN)"; \
	fi
	@if [ -f "$(CARBOX_RAW_SECTION_BIN)" ]; then \
		size=$$(stat -c %s "$(CARBOX_RAW_SECTION_BIN)"); \
		max=$$(( $(CARBOX_RAW_SECTION_SIZE) )); \
		if [ $$size -gt $$max ]; then \
			echo "ERROR: $(CARBOX_RAW_SECTION_BIN) size $$size exceeds raw_section partition size $$max"; \
			exit 1; \
		fi; \
		echo "Pack $(CARBOX_RAW_SECTION_BIN) -> application_is/flash_is.bin @$(CARBOX_RAW_SECTION_BASE) ($$size bytes)"; \
		dd if="$(CARBOX_RAW_SECTION_BIN)" of=application_is/flash_is.bin bs=4096 seek=$$(( $(CARBOX_RAW_SECTION_BASE) / 0x1000 )) conv=notrunc status=none; \
	fi
	@$(FLASH_TOOLDIR)/postbuild.sh $(ELF2BIN) >/dev/null 2>&1
	@if [ ! -f application_is/flash_is.bin ] && [ -f application_is/flash_is_ota1.bin ]; then \
		cp application_is/flash_is_ota1.bin application_is/flash_is.bin; \
	fi
	@FW_SRC=application_is/firmware_is.bin; \
	 if [ ! -f "$$FW_SRC" ]; then \
	   cat application_is/firmware_is_ota1.bin application_is/firmware_is_ota2.bin > /tmp/carbox_fw_merged.bin 2>/dev/null; \
	   FW_SRC=/tmp/carbox_fw_merged.bin; \
	 fi; \
	 if [ -f "$$FW_SRC" ]; then \
	   python3 gen_ota_app.py "$$FW_SRC" $(CARBOX_OTA_FW_VER) application_is/ota_app.bin; \
	 fi
	@if grep -q '"fatfs"' partition.json && [ -f "application_is/ota_app.bin" ] && [ -f "$(CARBOX_FATFS_BIN)" ]; then \
		python3 gen_ota_all.py application_is/ota_app.bin "$(CARBOX_FATFS_BIN)" $(CARBOX_OTA_FW_VER) "$(CARBOX_OTA_ALL_BIN)"; \
	fi
	

# Generate build info
# -------------------------------------------------------------------	

.PHONY: build_info
build_info:
	@echo \#define RTL_FW_COMPILE_TIME RTL8195BFW_COMPILE_TIME\ > .ver
	@echo \#define RTL_FW_COMPILE_DATE RTL8195BFW_COMPILE_DATE\ >> .ver
	@echo \#define UTS_VERSION \"`date +%Y/%m/%d-%T`\" >> .ver
	@echo \#define RTL8195BFW_COMPILE_TIME \"`date +%Y/%m/%d-%T`\" >> .ver
	@echo \#define RTL8195BFW_COMPILE_DATE \"`date +%Y%m%d`\" >> .ver
	@echo \#define RTL8195BFW_COMPILE_BY \"`id -u -n`\" >> .ver
	@echo \#define RTL8195BFW_COMPILE_HOST \"`$(HOSTNAME_APP)`\" >> .ver
	@BOX_VER="`date +%Y%m%d%H%M%S`_$(shell od -An -N2 -tx1 /dev/urandom | tr -d ' \n' | tr 'a-f' 'A-F')"; \
	echo \#define BOX_APP_VERSION \"$$BOX_VER\" >> .ver; \
	mkdir -p $(TARGET); \
	echo "{ \"version\": \"$$BOX_VER\" }" > $(TARGET)/versions.json; \
	if [ -d $(VFSDIR) ]; then \
		echo "$$BOX_VER" > $(VFSDIR)/Version.txt; \
		echo "$$BOX_VER" > $(VFSDIR)/OsVer.txt; \
	fi
	@if [ -x /bin/dnsdomainname ]; then \
		echo \#define RTL8195BFW_COMPILE_DOMAIN \"`dnsdomainname`\"; \
	elif [ -x /bin/domainname ]; then \
		echo \#define RTL8195BFW_COMPILE_DOMAIN \"`domainname`\"; \
	else \
		echo \#define RTL8195BFW_COMPILE_DOMAIN ; \
	fi >> .ver

	@echo \#define RTL8195BFW_COMPILER \"gcc `$(CC) $(CFLAGS) -dumpversion | tr --delete '\r'`\" >> .ver
	@if cmp -s .ver ../inc/$@.h 2>/dev/null; then \
		rm -f .ver; \
	else \
		mv -f .ver ../inc/$@.h; \
	fi



.PHONY: prerequirement
prerequirement:
	@echo ===========================================================
	@echo Build $(TARGET)
	@echo ===========================================================
	@mkdir -p $(OBJ_DIR)
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(INFO_DIR)
	@mkdir -p $(BOOT_BIN_DIR)
	
$(ASM_O): %.o : %.S
	#$(AS) -march=armv8-m.main+dsp -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16 -g $< -o $@
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@cp $@ $(OBJ_DIR)/$(notdir $@)
	@mv $(notdir $*.s) $(INFO_DIR)
	@chmod 777 $(OBJ_DIR)/$(notdir $@)

$(CPP_O): %.o : %.cpp
	@echo "  CC   $<"
	@$(CC) $(CPPFLAGS) $(INCLUDES) -c $< -o $@
	@cp $@ $(OBJ_DIR)/$(notdir $@)
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)
	@chmod 777 $(OBJ_DIR)/$(notdir $@)

$(SRC_O): %.o : %.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS)  $(INCLUDES) -c $< -o $@
	@#$(CC) $(CFLAGS) $(INCLUDES) -c $< -MM -MT $@ -MF $(OBJ_DIR)/$(notdir $(patsubst %.o,%.d,$@))
	@cp $@ $(OBJ_DIR)/$(notdir $@)
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)
	@chmod 777 $(OBJ_DIR)/$(notdir $@)

$(SRAM_O): %.o : %.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@$(OBJCOPY) --prefix-alloc-sections .sram $@
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -MM -MT $@ -MF $(OBJ_DIR)/$(notdir $(patsubst %.o,%.d,$@))
	@cp $@ $(OBJ_DIR)/$(notdir $@)
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)
	@chmod 777 $(OBJ_DIR)/$(notdir $@)

$(ERAM_O): %.o : %.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@$(OBJCOPY) --prefix-alloc-sections .eram $@
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -MM -MT $@ -MF $(OBJ_DIR)/$(notdir $(patsubst %.o,%.d,$@))
	@cp $@ $(OBJ_DIR)/$(notdir $@)
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)
	@chmod 777 $(OBJ_DIR)/$(notdir $@)

$(CINIT_O): %.o : %.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@$(OBJCOPY) --prefix-alloc-sections .cinit $@
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -MM -MT $@ -MF $(OBJ_DIR)/$(notdir $(patsubst %.o,%.d,$@))
	@cp $@ $(OBJ_DIR)/$(notdir $@)
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)
	@chmod 777 $(OBJ_DIR)/$(notdir $@)

$(ITCM_O): %.o : %.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@$(OBJCOPY) --prefix-alloc-sections .itcm $@
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -MM -MT $@ -MF $(OBJ_DIR)/$(notdir $(patsubst %.o,%.d,$@))
	@cp $@ $(OBJ_DIR)/$(notdir $@)
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)
	@chmod 777 $(OBJ_DIR)/$(notdir $@)

$(DTCM_O): %.o : %.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@$(OBJCOPY) --prefix-alloc-sections .dtcm $@
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -MM -MT $@ -MF $(OBJ_DIR)/$(notdir $(patsubst %.o,%.d,$@))
	@cp $@ $(OBJ_DIR)/$(notdir $@)
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)
	@chmod 777 $(OBJ_DIR)/$(notdir $@)

-include $(DEPENDENCY_LIST)

# Generate build info
# -------------------------------------------------------------------	
#ifeq (setup,$(firstword $(MAKECMDGOALS)))
#  # use the rest as arguments for "run"
#  RUN_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
#  # ...and turn them into do-nothing targets
#  $(eval $(RUN_ARGS):;@:)
#endif

.PHONY: setup
setup:
	@echo "----------------"
	@echo Setup $(GDB_SERVER)
	@echo "----------------"
ifeq ($(GDB_SERVER), openocd)
	@cp -p $(FLASH_TOOLDIR)/rtl_gdb_debug_openocd.txt $(FLASH_TOOLDIR)/rtl_gdb_debug.txt
#	cp -p $(FLASH_TOOLDIR)/rtl_gdb_ramdebug_openocd.txt $(FLASH_TOOLDIR)/rtl_gdb_ramdebug.txt
#	cp -p $(FLASH_TOOLDIR)/rtl_gdb_flash_write_openocd.txt $(FLASH_TOOLDIR)/rtl_gdb_flash_write.txt
else
	@cp -p $(FLASH_TOOLDIR)/rtl_gdb_debug_jlink.txt $(FLASH_TOOLDIR)/rtl_gdb_debug.txt
#	cp -p $(FLASH_TOOLDIR)/rtl_gdb_ramdebug_jlink.txt $(FLASH_TOOLDIR)/rtl_gdb_ramdebug.txt
#	cp -p $(FLASH_TOOLDIR)/rtl_gdb_flash_write_jlink.txt $(FLASH_TOOLDIR)/rtl_gdb_flash_write.txt
endif

.PHONY: flashburn
flashburn:
#	@if [ ! -f $(FLASH_TOOLDIR)/rtl_gdb_flash_write.txt ] ; then echo Please do /"make setup GDB_SERVER=[jlink or openocd]/" first; echo && false ; fi
#ifeq ($(findstring CYGWIN, $(OS)), CYGWIN) 
#	$(FLASH_TOOLDIR)/Check_Jtag.sh
#endif
	@chmod +rx $(FLASH_TOOLDIR)/flashloader.sh
	@$(FLASH_TOOLDIR)/flashloader.sh
	$(GDB) -x $(FLASH_TOOLDIR)/rtl_gdb_flashloader_jlink.txt
	
.PHONY: debug
debug:
#	@if [ ! -f $(FLASH_TOOLDIR)/rtl_gdb_debug.txt ] ; then echo Please do /"make setup GDB_SERVER=[jlink or openocd]/" first; echo && false ; fi

	@chmod +rx $(FLASH_TOOLDIR)/debug.sh
	@$(FLASH_TOOLDIR)/debug.sh $(BIN_DIR)/$(TARGET).axf
	$(GDB) -x $(FLASH_TOOLDIR)/rtl_gdb_debug_jlink.txt


.PHONY: clean
clean:
	@rm -rf $(TARGET)
	@rm -rf $(CARBOX_RTK264_BUILD_DIR)
	@rm -f $(patsubst %.o,%.d,$(SRC_O)) $(patsubst %.o,%.d,$(ERAM_O)) $(patsubst %.o,%.d,$(SRAM_O))
	@rm -f $(patsubst %.o,%.d,$(CINIT_O)) $(patsubst %.o,%.d,$(ITCM_O)) $(patsubst %.o,%.d,$(DTCM_O))
	@rm -f $(patsubst %.o,%.d,$(ASM_O)) $(patsubst %.o,%.d,$(CPP_O))
	@rm -f $(patsubst %.o,%.su,$(SRC_O)) $(patsubst %.o,%.su,$(ERAM_O)) $(patsubst %.o,%.su,$(SRAM_O))
	@rm -f $(patsubst %.o,%.su,$(CINIT_O)) $(patsubst %.o,%.su,$(ITCM_O)) $(patsubst %.o,%.su,$(DTCM_O))
	@rm -f $(patsubst %.o,%.su,$(ASM_O)) $(patsubst %.o,%.su,$(CPP_O))
	@rm -f $(SRC_O) $(ERAM_O) $(SRAM_O) $(CINIT_O) $(ITCM_O) $(ASM_O) $(CPP_O) $(DTCM_O)
	@rm -f *.i
	@rm -f *.s

# ---- Backtrace parser ----
# 1. 把地址写入 code/backtrace.txt（每行一个地址）
# 2. 在 GCC-RELEASE 目录运行
#make backtrace

# 或者直接传地址
#make backtrace ADDR="E8F3C05C E940B0B2 E940AD84"

BT_ELF = $(TARGET)/Debug/bin/$(TARGET).axf
BT_MAP = $(TARGET)/Debug/bin/$(TARGET).map
BT_TOOL = gnu_utility/backtrace_parser.py
BT_FILE = backtrace.txt

backtrace:
ifneq ($(ADDR),)
	@echo "$(ADDR)" | python3 $(BT_TOOL) - $(BT_MAP)
else
	@python3 $(BT_TOOL) $(BT_FILE) $(BT_MAP)
endif

# symbols mode — nm function symbols only, faster and cleaner than map
# cross-references map file to show [object file]
symbols:
ifneq ($(ADDR),)
	@echo "$(ADDR)" | python3 $(BT_TOOL) symbols - $(BT_ELF) --map $(BT_MAP)
else
	@python3 $(BT_TOOL) symbols $(BT_FILE) $(BT_ELF) --map $(BT_MAP)
endif

# text mode — addr2line for precise file:line (slow, use on specific addresses)
text:
ifneq ($(ADDR),)
	@echo "$(ADDR)" | python3 $(BT_TOOL) text - $(BT_ELF) --map $(BT_MAP)
else
	@python3 $(BT_TOOL) text $(BT_FILE) $(BT_ELF) --map $(BT_MAP)
endif
