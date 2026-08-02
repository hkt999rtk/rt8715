
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
CARBOX_USB_ARCHIVE := usb_lib/lib_usbsmart.a
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
SRC_C += ../../../component/common/api/at_cmd/atcmd_isp.c
SRC_C += ../../../component/common/api/at_cmd/atcmd_lwip.c
SRC_C += ../../../component/common/api/at_cmd/atcmd_media.c
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
SRC_C += ../../../component/common/media/mmfv2/module_h264.c
SRC_C += ../../../component/common/media/mmfv2/module_i2s.c
SRC_C += ../../../component/common/media/mmfv2/module_isp.c
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
SRC_C += ../../../component/common/media/framework/sensor_service.c
SRC_C += ../../../component/common/drivers/video/realtek/common/encoder_buffer_handler.c
SRC_C += ../../../component/common/media/framework/jpeg_snapshot.c
SRC_C += ../../../component/common/media/media_amebacam_broadcast.c
SRC_C += ../../../component/common/media/framework/mmf_command.c
SRC_C += ../../../component/common/media/framework/snapshot_sd_handler.c
SRC_C += ../../../component/common/media/framework/snapshot_tftp_handler.c

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
SRC_C += ../../../component/common/example/isp/example_isp_osd_multi.c
SRC_C += ../../../component/common/example/isp/example_md.c
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
SRC_C += ../src/eval/sensor_board_v1/ir_ctrl.c
SRC_C += ../src/eval/sensor_board_v1/ir_cut.c
SRC_C += ../src/eval/sensor_board_v1/sensor_external_ctrl.c
SRC_C += ../../../component/common/drivers/wlan/realtek/src/core/option/rtw_opt_skbuf.c

SRC_C += ../src/main.c
SRC_C += ../src/carbox/carbox_diag.c
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
CINIT_C += ../../../component/os/freertos/freertos_v10.0.0/portable/MemMang/heap_4_2.c
CINIT_C += ../../../component/soc/realtek/8195b/misc/utilities/source/ram/libc_wrap.c
CINIT_C += ../src/isp_boot_config.c

#SRAM
# -------------------------------------------------------------------
#@SRAM
SRAM_C += ../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/flash_api.c
SRAM_C += ../../../component/soc/realtek/8195b/misc/driver/flash_api_ext.c
SRAM_C += ../../../component/common/file_system/fatfs/disk_if/src/flash_fatfs.c
SRAM_C += ../src/carbox/vfs_compat/carbox_littlefs.c
SRAM_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_s/hal_flash.c

#ERAM
# -------------------------------------------------------------------
#@ERAM
ERAM_C +=


# Immutable FW1 recovery image. Use an explicit allow-list so application,
# multimedia, USB, filesystem and CarPlay code cannot silently enter the
# 512 KiB recovery partition as the normal firmware grows.
ifeq ($(CARBOX_RECOVERY_BUILD),1)
SRC_ASM :=
SRC_CPP :=
ERAM_C :=
DTCM_C :=
SRC_C := $(filter \
	../../../component/soc/realtek/8195b/cmsis/rtl8195b-hp/source/ram_s/app_start.c \
	../../../component/soc/realtek/8195b/cmsis/rtl8195b-hp/source/ram/mpu_config.c \
	../../../component/soc/realtek/8195b/app/shell/% \
	../../../component/soc/realtek/8195b/misc/driver/% \
	../../../component/soc/realtek/8195b/misc/platform/ota_8195b.c \
	../../../component/common/api/wifi/% \
	../../../component/common/api/lwip_netconf.c \
	../../../component/common/network/ssl/% \
	../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/% \
	../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/% \
	../../../component/os/% \
	../../../component/common/drivers/wlan/realtek/src/core/option/rtw_opt_skbuf.c, \
	$(SRC_C))
SRC_C := $(filter-out \
	../../../component/common/api/wifi/wifi_simple_config.c \
	../../../component/common/api/wifi/rtw_wpa_supplicant/wpa_supplicant/wifi_p2p_config.c \
	../../../component/common/api/wifi/rtw_wpa_supplicant/wpa_supplicant/wifi_wps_config.c, \
	$(SRC_C))
SRC_C += ../src/recovery/recovery_main.c
CINIT_C := \
	../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram/hal_timer.c \
	../../../component/os/freertos/freertos_v10.0.0/portable/MemMang/heap_4_2.c \
	../../../component/soc/realtek/8195b/misc/utilities/source/ram/libc_wrap.c
SRAM_C := \
	../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/flash_api.c \
	../../../component/soc/realtek/8195b/misc/driver/flash_api_ext.c \
	../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_s/hal_flash.c
ITCM_C := $(filter-out \
	../../../component/common/network/lwip/lwip_v2.1.2/src/core/ipv4/ip_nat/% \
	../../../component/common/network/dhcp/dhcps.c \
	../../../component/common/network/sntp/sntp.c, \
	$(ITCM_C))
endif




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
WLAN_RX_GDMA_VERIFY ?= 0
GCCFLAGS += -DCONFIG_WLAN_RX_RING_GDMA_VERIFY=$(WLAN_RX_GDMA_VERIFY)
CARBOX_IMMUTABLE_RECOVERY_OTA ?= 1
GCCFLAGS += -DCARBOX_IMMUTABLE_RECOVERY_OTA=$(CARBOX_IMMUTABLE_RECOVERY_OTA)
CARBOX_RECOVERY_BUILD ?= 0
GCCFLAGS += -DCARBOX_RECOVERY_BUILD=$(CARBOX_RECOVERY_BUILD)
CARBOX_LINK_FW ?= 2
ifeq ($(CARBOX_RECOVERY_BUILD),1)
GCCFLAGS += -DCONFIG_OTA_HTTP_UPDATE=0 -DCONFIG_OTA_DFU_UPDATE=0
endif

# This SDK compiles .o files beside their sources before copying them into the
# target directory. Recovery and Main use different preprocessor settings, so
# remember which mode produced those shared source-tree objects. Switching
# modes touches the stamp and forces a safe rebuild instead of silently linking
# Recovery-configured objects into Main (or vice versa).
CARBOX_COMPILE_MODE_STAMP := .carbox_compile_mode
.PHONY: carbox_compile_mode_force
carbox_compile_mode_force:

$(CARBOX_COMPILE_MODE_STAMP): carbox_compile_mode_force
	@printf '%s\n' 'CARBOX_RECOVERY_BUILD=$(CARBOX_RECOVERY_BUILD)' > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv -f $@.tmp $@; else rm -f $@.tmp; fi

$(ASM_O) $(SRC_O) $(CPP_O) $(ERAM_O) $(SRAM_O) $(CINIT_O) $(ITCM_O) $(DTCM_O): $(CARBOX_COMPILE_MODE_STAMP)
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

CPPFLAGS = $(GCCFLAGS)
CPPFLAGS += -std=c++11 -fno-use-cxa-atexit
CPPFLAGS += -w
CPPFLAGS += -Wall -Wpointer-arith -Wundef -Wno-write-strings -Wno-maybe-uninitialized

LFLAGS = 
LFLAGS += -march=armv8-m.main+dsp -mthumb -mcmse -mfloat-abi=softfp -mfpu=fpv5-sp-d16 -Os -nostartfiles -specs=nosys.specs -nodefaultlibs -nostdlib
LFLAGS += -Wl,--gc-sections -Wl,-Map=$(BIN_DIR)/$(TARGET).map -Wl,--cref -Wl,--build-id=none -Wl,--use-blx 
ifeq ($(FDK_AAC_PROFILE),1)
LFLAGS += -Wl,--wrap=aacEncEncode -Wl,--wrap=aacDecoder_DecodeFrame
endif
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
WLAN_RX_GDMA_DIR := ../../../component/common/drivers/wlan/realtek/wlan_rx_gdma
WLAN_RX_GDMA_ARCHIVE := $(WLAN_RX_GDMA_DIR)/build/lib_wlan_rx_gdma.a
WLAN_RX_GDMA_ORIGINAL := ../../../component/soc/realtek/8195b/misc/bsp/lib/common/GCC/lib_wlan.a
CARBOX_BUILD_RTK264 ?= 1
CARBOX_RTK264_OPT_FLAGS ?= -O3
CARBOX_RTK264_DIR := ../src/carbox/rtk264
CARBOX_RTK264_BUILD_DIR := $(CARBOX_RTK264_DIR)/build
CARBOX_RTK264_SOURCE := $(CARBOX_RTK264_DIR)/lib_rtk264.c
CARBOX_RTK264_HEADER := $(CARBOX_RTK264_DIR)/lib_rtk264.h
CARBOX_RTK264_OBJECT := $(CARBOX_RTK264_BUILD_DIR)/lib_rtk264.o
CARBOX_RTK264_ARCHIVE := $(CARBOX_RTK264_BUILD_DIR)/lib_rtk264.a
ifeq ($(CARBOX_RECOVERY_BUILD),1)
all recovery_image: LIBFLAGS += -l_soc_is $(WLAN_RX_GDMA_ORIGINAL) -l_wps
else
all: LIBFLAGS += -l_codec -l_dct -l_h264 -l_haac -l_hmp3 -l_http -l_mmf -l_muxer -l_p2p -l_rtsp -l_sdcard -l_soc_is -l_speex  -l_websocket $(WLAN_RX_GDMA_ARCHIVE) -l_wps -l_qr_code -l_tftp -l_opusenc -l_opusfile -l_opus
endif
mp: LIBFLAGS += -l_codec -l_dct -l_h264 -l_haac -l_hmp3 -l_http -l_mmf -l_muxer -l_p2p -l_rtsp -l_sdcard -l_soc_is -l_speex  -l_websocket -l_wlan_mp -l_wps -l_qr_code -l_tftp -l_opusenc -l_opusfile -l_opus
ifneq ($(CARBOX_RECOVERY_BUILD),1)
ifneq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
all: LIBFLAGS += -l_mdns -l_faac
endif
all: LIBFLAGS += -lrtstream -lrtscamkit -lrtsv4l2 -lrtsisp -lrtsosd
endif
ifneq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
mp: LIBFLAGS += -l_mdns -l_faac
endif
LIBFLAGS += -Wl,-u,ram_start -Wl,-u,cinit_start
ifeq ($(CARBOX_USB_LIB),1)
LIBFLAGS += -Wl,--whole-archive $(CARBOX_USB_ARCHIVE) -Wl,--no-whole-archive
endif

ifeq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
CARBOX_SMART_CARPLAY_LIB_DIR := carplay_app
CARBOX_CHACHA_M33 ?= 1
CARBOX_CHACHA_MODE ?= 0
CARBOX_CHACHA_HW_MIN_LEN ?= 4096
CARBOX_CHACHA_NONALIGNED_SW_POLY ?= 0
CARBOX_CHACHA_STATS_INTERVAL_MS ?= 5000
CARBOX_CHACHA_HW_SELFTEST ?= 0
CARBOX_CHACHA_OPT_FLAGS ?= -O3
CARBOX_CHACHA_M33_DIR := $(CARBOX_SMART_CARPLAY_LIB_DIR)/chacha_m33
CARBOX_CHACHA_M33_ARCHIVE := $(CARBOX_CHACHA_M33_DIR)/build/lib_CarPlay_chacha_m33.a
ifeq ($(CARBOX_CHACHA_M33),1)
CARBOX_CARPLAY_ARCHIVE := $(CARBOX_CHACHA_M33_ARCHIVE)
else
CARBOX_CARPLAY_ARCHIVE := $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_CarPlay.a
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
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_fdkaac.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_init.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_zlib.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_AndroidAuto.a
LIBFLAGS += $(CARBOX_CARPLAY_ARCHIVE)
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_Accessory2.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_jpeg.a
# LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_usbdev.a
# LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_ncm.a
LIBFLAGS += -Wl,--no-whole-archive
endif

ifeq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
ifeq ($(CARBOX_CHACHA_M33),1)
.PHONY: carbox_chacha_m33
carbox_chacha_m33:
	@$(MAKE) -C $(CARBOX_CHACHA_M33_DIR) replacement \
		CROSS_COMPILE=$(abspath $(CROSS_COMPILE)) \
		CHACHA_MODE=$(CARBOX_CHACHA_MODE) \
		CHACHA_HW_MIN_LEN=$(CARBOX_CHACHA_HW_MIN_LEN) \
		CHACHA_NONALIGNED_SW_POLY=$(CARBOX_CHACHA_NONALIGNED_SW_POLY) \
		CHACHA_STATS_INTERVAL_MS=$(CARBOX_CHACHA_STATS_INTERVAL_MS) \
		CHACHA_HW_SELFTEST=$(CARBOX_CHACHA_HW_SELFTEST) \
		OPT_FLAGS=$(CARBOX_CHACHA_OPT_FLAGS)
application: carbox_chacha_m33
endif
endif


LIBFLAGS += -lm -lc -lnosys -lgcc -lstdc++

# Sensor
include sensor.mk

# Prebuild
# -------------------------------------------------------------------
	
.PHONY: prebuild
prebuild:	
	@echo ===========================================================
	@echo Prebuild
	@echo ===========================================================
	@echo "  ELF2BIN  prebuild GCC"
	@$(ELF2BIN) prebuild GCC xip_fw.ld FW >/dev/null 2>&1
	@# Immutable Recovery FW1 is 512 KiB; upgradeable Main FW2 ends at FATFS.
	@sed -i 's/__ICFEDIT_region_XIP_FW1_FLASH_end__.*=.*;/__ICFEDIT_region_XIP_FW1_FLASH_end__\t\t= 0x980C0220 ;/' xip_fw.ld
	@sed -i 's/__ICFEDIT_region_XIP_FW2_FLASH_start__.*=.*;/__ICFEDIT_region_XIP_FW2_FLASH_start__\t\t= 0x980C0220 ;/' xip_fw.ld
	@sed -i 's/__ICFEDIT_region_XIP_FW2_FLASH_end__.*=.*;/__ICFEDIT_region_XIP_FW2_FLASH_end__\t\t= 0x98640220 ;/' xip_fw.ld
	@# Recovery passes CARBOX_LINK_FW=1; the normal Main build defaults to FW2.
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
.PHONY: wlan_rx_gdma
wlan_rx_gdma:
	@$(MAKE) -C $(WLAN_RX_GDMA_DIR) \
		CROSS_COMPILE=$(abspath $(CROSS_COMPILE)) \
		ORIGINAL_ARCHIVE=$(abspath $(WLAN_RX_GDMA_ORIGINAL))

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
ifeq ($(CARBOX_RECOVERY_BUILD),1)
application: prerequirement $(SRC_O) $(ERAM_O) $(SRAM_O) $(CINIT_O) $(ASM_O) $(ITCM_O) $(CPP_O)
else
application: wlan_rx_gdma prerequirement $(SRC_O) $(ERAM_O) $(SRAM_O) $(CINIT_O) $(ASM_O) $(ITCM_O) $(CPP_O)
endif
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
CARBOX_RAW_SECTION_BIN ?= application_is/raw_section.bin
CARBOX_FLASH_LAYOUT_H := ../inc/carbox_flash_layout.h
CARBOX_FATFS_BASE := $(shell awk '/^\#define CARBOX_FATFS_BASE /{print $$3}' $(CARBOX_FLASH_LAYOUT_H))
CARBOX_FATFS_SIZE := $(shell awk '/^\#define CARBOX_FATFS_SIZE /{print $$3}' $(CARBOX_FLASH_LAYOUT_H))
CARBOX_RAW_SECTION_BASE := $(shell awk '/^\#define CARBOX_RAW_SECTION_BASE /{print $$3}' $(CARBOX_FLASH_LAYOUT_H))
CARBOX_RAW_SECTION_SIZE := $(shell awk '/^\#define CARBOX_RAW_SECTION_SIZE /{print $$3}' $(CARBOX_FLASH_LAYOUT_H))

partition.json: ../inc/carbox_flash_layout.h gen_partition_json.py
	@python3 gen_partition_json.py ../inc/carbox_flash_layout.h $@

CARBOX_RECOVERY_BIN := recovery_is/firmware_recovery.bin
CARBOX_RECOVERY_JSON := recovery_is/firmware_recovery.json
CARBOX_RECOVERY_SERIAL_JSON := recovery_is/firmware_recovery.serial.json
CARBOX_MAIN_BIN := application_is/firmware_main.bin
CARBOX_MAIN_JSON := application_is/firmware_main.json
CARBOX_MAIN_SERIAL_JSON := application_is/firmware_main.serial.json

.PHONY: recovery_image
recovery_image: partition.json | application
	@mkdir -p recovery_is
	@python3 gen_firmware_json.py amebapro_firmware_is.json FW1 \
		recovery_is/Debug/bin/recovery_is.axf $(CARBOX_RECOVERY_BIN) \
		$(CARBOX_RECOVERY_JSON) recovery
	@$(FLASH_TOOLDIR)/set_fw_json_serial.sh \
		$(CARBOX_RECOVERY_JSON) $(CARBOX_RECOVERY_SERIAL_JSON) >/dev/null
	@echo "  ELF2BIN  Recovery FW1"
	@$(ELF2BIN) convert $(CARBOX_RECOVERY_SERIAL_JSON) FIRMWARE secure_bit=0 >/dev/null
	@$(CHKSUM) $(CARBOX_RECOVERY_BIN) >/dev/null 2>&1
	@python3 check_firmware_size.py $(CARBOX_FLASH_LAYOUT_H) \
		CARBOX_RECOVERY_FW_SIZE $(CARBOX_RECOVERY_BIN)
 		
.PHONY: manipulate_images
manipulate_images:	partition.json | application
	@echo ===========================================================
	@echo Image manipulating
	@echo ===========================================================
	@if [ -d $(VFSDIR) ]; then \
		$(VFSTOOL) -t FATFS -s 512 -c $(FATFS_SECTORS) -dir $(VFSDIR) -out application_is/fatfs.bin >/dev/null 2>&1; \
	fi	
	@cp  ../../../component/soc/realtek/8195b/misc/bsp/image/boot.bin application_is/boot.bin
	@echo "  ELF2BIN  keygen keycfg.json"
	@$(ELF2BIN) keygen keycfg.json >/dev/null 2>&1
	@echo "  ELF2BIN  convert amebapro_bootloader.json"
	@$(ELF2BIN) convert amebapro_bootloader.json PARTITIONTABLE secure_bit=0 >/dev/null 2>&1
	@python3 gen_firmware_json.py amebapro_firmware_is.json FW2 \
		application_is/Debug/bin/application_is.axf $(CARBOX_MAIN_BIN) \
		$(CARBOX_MAIN_JSON)
	@$(FLASH_TOOLDIR)/set_fw_json_serial.sh \
		$(CARBOX_MAIN_JSON) $(CARBOX_MAIN_SERIAL_JSON) >/dev/null 2>&1
	@echo "  ELF2BIN  convert Main FW2"
	@$(ELF2BIN) convert $(CARBOX_MAIN_SERIAL_JSON) FIRMWARE secure_bit=0 >/dev/null 2>&1
	@$(CHKSUM) $(CARBOX_MAIN_BIN) >/dev/null 2>&1
	@python3 check_firmware_size.py $(CARBOX_FLASH_LAYOUT_H) \
		CARBOX_MAIN_FW_SIZE $(CARBOX_MAIN_BIN)
	@cp ../../../component/soc/realtek/8195b/misc/bsp/image/bootloader.axf $(BOOT_BIN_DIR)/bootloader.axf
	@echo "  ELF2BIN  convert amebapro_bootloader.json"
	@$(ELF2BIN) convert amebapro_bootloader.json BOOTLOADER secure_bit=0 >/dev/null 2>&1
	@cp bootloader/boot.bin application_is/boot.bin
	@echo "  ELF2BIN  combine application_is/flash_is.bin"
	@$(ELF2BIN) combine application_is/flash_is.bin \
		PTAB=partition.bin,BOOT=application_is/boot.bin,FW2=$(CARBOX_MAIN_BIN) >/dev/null 2>&1
	@if [ -f "$(CARBOX_FATFS_BIN)" ]; then \
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
	@python3 gen_ota_app.py $(CARBOX_MAIN_BIN) $(CARBOX_OTA_FW_VER) \
		application_is/ota_app.bin
	@if [ -f "application_is/ota_app.bin" ] && [ -f "$(CARBOX_FATFS_BIN)" ]; then \
		python3 gen_ota_all.py application_is/ota_app.bin "$(CARBOX_FATFS_BIN)" $(CARBOX_OTA_FW_VER) "$(CARBOX_OTA_ALL_BIN)"; \
	fi

.PHONY: factory_image
factory_image: partition.json
	@test -f $(CARBOX_RECOVERY_BIN) || \
		{ echo "ERROR: missing $(CARBOX_RECOVERY_BIN); run make recovery_image"; exit 1; }
	@test -f $(CARBOX_MAIN_BIN) || \
		{ echo "ERROR: missing $(CARBOX_MAIN_BIN); run make ram_is"; exit 1; }
	@python3 check_firmware_size.py $(CARBOX_FLASH_LAYOUT_H) \
		CARBOX_RECOVERY_FW_SIZE $(CARBOX_RECOVERY_BIN)
	@python3 check_firmware_size.py $(CARBOX_FLASH_LAYOUT_H) \
		CARBOX_MAIN_FW_SIZE $(CARBOX_MAIN_BIN)
	@echo "  ELF2BIN  combine factory image (immutable FW1 + Main FW2)"
	@$(ELF2BIN) combine application_is/flash_factory.bin \
		PTAB=partition.bin,BOOT=application_is/boot.bin,FW1=$(CARBOX_RECOVERY_BIN),FW2=$(CARBOX_MAIN_BIN) >/dev/null 2>&1
	@if [ -f "$(CARBOX_FATFS_BIN)" ]; then \
		size=$$(stat -c %s "$(CARBOX_FATFS_BIN)"); \
		max=$$(( $(CARBOX_FATFS_SIZE) )); \
		if [ $$size -gt $$max ]; then \
			echo "ERROR: $(CARBOX_FATFS_BIN) size $$size exceeds FATFS partition size $$max"; \
			exit 1; \
		fi; \
		echo "Pack $(CARBOX_FATFS_BIN) -> application_is/flash_factory.bin @$(CARBOX_FATFS_BASE) ($$size bytes)"; \
		dd if="$(CARBOX_FATFS_BIN)" of=application_is/flash_factory.bin bs=4096 seek=$$(( $(CARBOX_FATFS_BASE) / 0x1000 )) conv=notrunc status=none; \
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
