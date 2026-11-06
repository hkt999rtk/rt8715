# 客户测通的 Recovery MP 独立构建入口；不替换主线 application.is.mk。
override CARBOX_RECOVERY := 1
override CARBOX_LINK_FW := 2
override WLAN_RX_DMA_SKB := 0
override SYSTEM_CLOCK_PROFILE := 300
override SYS_PLL_OVERCLOCK := 0
override SYS_PLL_TARGET_HZ := 300000000
.DEFAULT_GOAL := all

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

CROSS_COMPILE = $(ARM_GCC_TOOLCHAIN)/arm-none-eabi-

# Compilation tools
# Some customer copies of the legacy Realtek Linux toolchain contain gcc,
# nm and objcopy but have an incomplete or transient arm-none-eabi-ar.  GNU ar
# archives are target-independent containers, so prefer the independently
# installed host ar and only use cross ar when no host ar exists.  override
# also defeats stale values propagated through recursive MAKEFLAGS.
HOST_AR := $(strip $(shell command -v ar 2>/dev/null))
ifeq ($(HOST_AR),)
override AR := $(strip $(shell if [ -x "$(CROSS_COMPILE)ar" ]; then \
	printf '%s' "$(CROSS_COMPILE)ar"; fi))
ifeq ($(AR),)
$(error Neither $(CROSS_COMPILE)ar nor a host GNU ar was found)
endif
else
override AR := $(HOST_AR)
endif
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
OBJ_DIR=$(TARGET)/Debug/recovery_obj
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
INCLUDES += -I../../../component/common/audio/opus-1.3.1/include
INCLUDES += -I../../../component/common/audio/libopusenc-0.2.1/include
INCLUDES += -I../src/carbox
INCLUDES += -I../src/carbox/vfs_compat

CARBOX_EXPERIMENTAL_USB ?= 1

# Recovery build (make CARBOX_RECOVERY=1 CARBOX_LINK_FW=2): minimal
# WiFi + OTA image for the FW2 slot.  Excludes CarPlay, USB and the
# multimedia/audio/video codec stack.
CARBOX_RECOVERY ?= 0
ifeq ($(CARBOX_RECOVERY),1)
CARBOX_EXPERIMENTAL_USB := 0
CARBOX_USB_LIB := 0
endif

ifeq ($(CARBOX_EXPERIMENTAL_USB),1)
CARBOX_USB_LIB := 1
endif

ifeq ($(CARBOX_USB_LIB),1)
CARBOX_USB_VENDOR_ARCHIVE := usb_lib/build/lib_usbsmart.a
endif
# recovery 不直接编译 usb_lib 源码，但从 lib_usbsmart.a 按需链接 carbox_wifi_compat.o（热点开启）
ifeq ($(CARBOX_RECOVERY),1)
CARBOX_USB_VENDOR_ARCHIVE := usb_lib/build/lib_usbsmart_mp.a
endif




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
SRC_C += ../src/carbox/mp_calibration/recovery_atcmd_sys.c
SRC_C += ../../../component/common/api/at_cmd/atcmd_wifi.c
SRC_C += ../../../component/common/api/at_cmd/atcmd_qr_code.c
SRC_C += ../../../component/soc/realtek/8195b/app/shell/cmd_shell.c
SRC_C += ../../../component/soc/realtek/8195b/misc/driver/log_service.c
SRC_C += ../../../component/soc/realtek/8195b/misc/driver/low_level_io.c
SRC_C += ../../../component/soc/realtek/8195b/misc/driver/rtl_console.c

#multimedia
#mmfv2 (audio module kept for the AT command layer; the rest is pruned
# from the recovery build)
SRC_C += ../../../component/common/media/mmfv2/module_audio.c
ifneq ($(CARBOX_RECOVERY),1)
SRC_C += ../../../component/common/media/mmfv2/module_aac.c
SRC_C += ../../../component/common/media/mmfv2/module_aad.c
SRC_C += ../../../component/common/media/mmfv2/module_array.c
SRC_C += ../../../component/common/media/mmfv2/module_g711.c
SRC_C += ../../../component/common/media/mmfv2/module_i2s.c
SRC_C += ../../../component/common/media/mmfv2/module_mp4.c
SRC_C += ../../../component/common/media/mmfv2/module_rtp.c
SRC_C += ../../../component/common/media/mmfv2/module_rtsp2.c
SRC_C += ../../../component/common/media/mmfv2/module_uvcd.c
SRC_C += ../../../component/common/media/mmfv2/module_httpfs.c
SRC_C += ../../../component/common/media/mmfv2/module_dup.c
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
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/tcp_recovery.c
ITCM_C += ../../../component/common/network/lwip/lwip_v2.1.2/src/core/tcp_wake_trace.c
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


#FatFs
#disk_if
SRC_C += ../../../component/common/file_system/fatfs/disk_if/src/sdcard.c
##Debug
SRC_C += ../../../component/common/file_system/fatfs/disk_if/src/usbdisk.c

#fatfs_ext
SRC_C += ../../../component/common/file_system/fatfs/fatfs_ext/src/ff_driver.c

#r0.10c
#option
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
SRC_C += ../../../component/common/drivers/wlan/realtek/src/core/option/rtw_opt_skbuf.c

SRC_C += ../src/carbox/mp_calibration/recovery_main.c
# CONFIG_EXAMPLE_WLAN_FAST_CONNECT=1 (main.h) pulls these symbols from the
# AT/WiFi command layer; the example provides their definitions.
SRC_C += ../../../component/common/example/wlan_fast_connect/example_wlan_fast_connect.c
SRC_C += ../src/carbox/carbox_diag.c
SRC_C += ../src/carbox/pc_profiler.c
SRC_C += ../src/carbox/i2c_bitbang_pacing.c
SRC_C += ../src/carbox/memcheck.c
SRC_C += ../src/carbox/carbox_stubs.c
SRC_C += ../src/carbox/libusb_ref_compat/libusb_ref_compat_hal.c
SRC_C += ../src/carbox/libusb_ref_compat/libusb_ref_compat_delay.c
SRC_C += ../src/carbox/libusb_ref_compat/libusb_ref_compat_os.c
SRC_C += ../src/carbox/libusb_ref_compat/usb_ref_smart_compat.c
ifneq ($(CARBOX_RECOVERY),1)
SRC_C += ../src/carbox/libusb_ref_compat/carplay_smart_api_stubs.c
endif
SRC_C += ../../../component/soc/realtek/8195b/misc/driver/efuse_logical_api.c
SRC_C += ../src/carbox/vfs_compat/carbox_vfs_compat.c
SRC_C += ../src/carbox/vfs_compat/vfs_wrap.c
SRC_C += ../src/carbox/vfs_compat/vfs_fatfs.c
SRC_C += ../src/carbox/vfs_compat/vfs_littlefs.c
SRC_C += ../../../component/common/file_system/fatfs/fatfs_flash_api.c
SRC_C += ../../../component/common/file_system/littlefs/r2.41/lfs.c
SRC_C += ../../../component/common/file_system/littlefs/r2.41/lfs_util.c

#CINIT
# -------------------------------------------------------------------
#@CINIT
CINIT_C += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram/hal_timer.c
CINIT_C += ../src/carbox/atss_runtime_counter.c
CINIT_C += ../src/carbox/lpddr_re_wrap.c
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
SRAM_C += ../src/carbox/fault_dump.c
# The fatal handler must not trigger lazy FP stacking on a damaged stack.
$(OBJ_DIR)/fault_dump.o: CFLAGS += -mfloat-abi=soft
SRAM_C += ../src/carbox/spic_overclock.c
SRAM_C += ../src/carbox/nor_uuid.c
SRC_C += ../src/carbox/led_rgb_hp.c
# Scheduler-facing transaction API; keep it off XIP and available during
# crypto initialization. Only CarPlay AES/ChaCha archive members call it.
SRAM_C += ../src/carbox/crypto_priority_lock.c
SRAM_C += ../src/carbox/large_memcpy_gdma.c
SRAM_C += ../src/carbox/video_handover_zero_copy.c
# SRAM_C += ../src/carbox/screen_tx_direct_crypto.c
#ERAM
# -------------------------------------------------------------------
#@ERAM
ERAM_C +=

# Recovery pruning.  The audio/media HAL sources sit between the generic
# peripheral HAL entries, so drop them here with a single filter-out rather
# than scattering conditionals through the lists above.
ifeq ($(CARBOX_RECOVERY),1)
CARBOX_RECOVERY_EXCLUDE_C := \
	../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/audio_api.c \
	../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/i2s_api.c \
	../../../component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/pcm_api.c \
	../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/hal_audio.c \
	../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/hal_i2s.c \
	../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/source/ram_ns/hal_pcm.c \
	../../../component/common/file_system/fatfs/disk_if/src/usbdisk.c
SRC_C := $(filter-out $(CARBOX_RECOVERY_EXCLUDE_C),$(SRC_C))
CARBOX_RECOVERY_EXCLUDE_SRAM := \
	../src/carbox/crypto_priority_lock.c
SRAM_C := $(filter-out $(CARBOX_RECOVERY_EXCLUDE_SRAM),$(SRAM_C))

# Recovery OTA/HTTP server 来自 lib_usbsmart.a（由 usb_carbox.mk 编译），
# 这里只编译 recovery 特有的 Smart 兼容 stub。
SRC_C += ../src/carbox/recovery_ota_stubs.c
SRC_ASM += ../src/carbox/mp_calibration/recovery_ota_guard.S
LFLAGS += -Wl,--wrap=ota_update_init
SRC_C += ../src/carbox/mp_calibration/mp_calibration.c
SRC_C += ../src/carbox/mp_calibration/recovery_mp.c
INCLUDES += -I../src/carbox/mp_calibration
endif





# Generate obj list
# -------------------------------------------------------------------

# .o 统一输出到 OBJ_DIR（vpath 风格），不在源码树生成 .o
vpath %.c $(sort $(dir $(SRC_C) $(ERAM_C) $(SRAM_C) $(CINIT_C) $(ITCM_C) $(DTCM_C)))
vpath %.S $(sort $(dir $(SRC_ASM)))
vpath %.cpp $(sort $(dir $(SRC_CPP)))

ASM_O = $(addprefix $(OBJ_DIR)/,$(notdir $(SRC_ASM:.S=.o)))
SRC_O = $(addprefix $(OBJ_DIR)/,$(notdir $(SRC_C:.c=.o)))
CPP_O = $(addprefix $(OBJ_DIR)/,$(notdir $(SRC_CPP:.cpp=.o)))
ERAM_O = $(addprefix $(OBJ_DIR)/,$(notdir $(ERAM_C:.c=.o)))
SRAM_O = $(addprefix $(OBJ_DIR)/,$(notdir $(SRAM_C:.c=.o)))
CINIT_O = $(addprefix $(OBJ_DIR)/,$(notdir $(CINIT_C:.c=.o)))
ITCM_O = $(addprefix $(OBJ_DIR)/,$(notdir $(ITCM_C:.c=.o)))
DTCM_O = $(addprefix $(OBJ_DIR)/,$(notdir $(DTCM_C:.c=.o)))

# 同名源文件（vpath 会按字母序命中错误目录）从静态模式规则中排除，
# 用下方显式规则指定正确源文件。
OVERRIDE_O = $(OBJ_DIR)/libc_wrap.o $(OBJ_DIR)/log_service.o $(OBJ_DIR)/md4.o
SRC_O := $(filter-out $(OBJ_DIR)/log_service.o $(OBJ_DIR)/md4.o,$(SRC_O))
CINIT_O := $(filter-out $(OBJ_DIR)/libc_wrap.o,$(CINIT_O))

DEPENDENCY_LIST = $(ASM_O:.o=.d) $(SRC_O:.o=.d) $(CPP_O:.o=.d)
DEPENDENCY_LIST += $(ERAM_O:.o=.d) $(SRAM_O:.o=.d) $(CINIT_O:.o=.d) $(ITCM_O:.o=.d) $(DTCM_O:.o=.d)
DEPENDENCY_LIST += $(OVERRIDE_O:.o=.d)

OBJ_LIST = $(SRC_O) $(ERAM_O) $(SRAM_O) $(CINIT_O) $(ITCM_O) $(ASM_O) $(CPP_O) $(OVERRIDE_O)


# Compile options
# -------------------------------------------------------------------

GCCFLAGS =
GCCFLAGS += -march=armv8-m.main+dsp -mthumb -mcmse -mfloat-abi=softfp -mfpu=fpv5-sp-d16 -g -gdwarf-3 -Os -c -MMD --save-temps
GCCFLAGS += -nostartfiles -nodefaultlibs -nostdlib -fstack-usage -fdata-sections -ffunction-sections -fno-common
GCCFLAGS += -DCARBOX_X264_MEM_DIAG=1

# Define Macro
GCCFLAGS += -D__thumb2__ -DCONFIG_PLATFORM_8195B -DCONFIG_PLATFORM_8195BHP -D__FPU_PRESENT -D__ARM_ARCH_7M__=0 -D__ARM_ARCH_7EM__=0 -D__ARM_ARCH_8M_MAIN__=1 -D__ARM_ARCH_8M_BASE__=0
GCCFLAGS += -DCONFIG_BUILD_RAM=1 -DCONFIG_BUILD_LIB=1
GCCFLAGS += -DV8M_STKOVF -DARM_MATH_CM7 -DCONFIG_FATFS_WRAPPER=1
# Source-recovered rtl8195b_recv.o uses ordinary skb-backed DMA buffers.
# This is independent of the retired rtw_memcpy/kfree symbol-hook path.
WLAN_RX_DMA_SKB ?= 1

ifeq ($(CARBOX_RECOVERY),1)
SYSTEM_CLOCK_PROFILE ?= 300
else
SYSTEM_CLOCK_PROFILE ?= 400
endif
ifeq ($(SYSTEM_CLOCK_PROFILE),300)
SYS_PLL_OVERCLOCK ?= 0
SYS_PLL_TARGET_HZ ?= 300000000
else ifeq ($(SYSTEM_CLOCK_PROFILE),400)
SYS_PLL_OVERCLOCK ?= 1
SYS_PLL_TARGET_HZ ?= 400000000
else
$(error Unsupported SYSTEM_CLOCK_PROFILE=$(SYSTEM_CLOCK_PROFILE); use 300 or 400)
endif

# Reject contradictory legacy overrides instead of silently producing a build
# whose profile name and compiled PLL behavior disagree.
ifeq ($(SYSTEM_CLOCK_PROFILE),300)
ifneq ($(SYS_PLL_OVERCLOCK),0)
$(error SYSTEM_CLOCK_PROFILE=300 requires SYS_PLL_OVERCLOCK=0)
endif
ifneq ($(SYS_PLL_TARGET_HZ),300000000)
$(error SYSTEM_CLOCK_PROFILE=300 requires SYS_PLL_TARGET_HZ=300000000)
endif
else
ifneq ($(SYS_PLL_OVERCLOCK),1)
$(error SYSTEM_CLOCK_PROFILE=400 requires SYS_PLL_OVERCLOCK=1)
endif
ifneq ($(SYS_PLL_TARGET_HZ),400000000)
$(error SYSTEM_CLOCK_PROFILE=400 requires SYS_PLL_TARGET_HZ=400000000)
endif
endif



I2C_BITBANG_PACING ?= 1
I2C_BITBANG_BASELINE_HZ ?= 300000000
ifeq ($(CARBOX_RECOVERY),1)
CLOCK_CRITICAL_SYMBOLS :=
else
CLOCK_CRITICAL_SYMBOLS := carbox_system_overclock_early
endif
ifeq ($(SYS_PLL_OVERCLOCK),1)
ifneq ($(SYS_PLL_TARGET_HZ),300000000)
CLOCK_CRITICAL_SYMBOLS += carbox_spic_overclock_prepare \
	carbox_spic_overclock_calibrate carbox_spic_overclock_restore \
	spic_query_system_clk spic_set_delay_line spic_verify_calibration_para
endif
endif

GCCFLAGS += -DCONFIG_I2C_BITBANG_PACING=$(I2C_BITBANG_PACING)
GCCFLAGS += -DCONFIG_I2C_BITBANG_BASELINE_HZ=$(I2C_BITBANG_BASELINE_HZ)
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
ifneq ($(CARBOX_RECOVERY),1)
GCCFLAGS += -DCARBOX_EXPERIMENTAL_SMART_A_LINK=1
else
# Recovery 链接 CONFIG_MP_INCLUDED=1 编译的 MP 库，应用侧需匹配其 skb ABI。
GCCFLAGS += -DCONFIG_MP_INCLUDED
endif
GCCFLAGS += -DCARBOX_RECOVERY=$(CARBOX_RECOVERY)

ifeq ($(CARBOX_EXPERIMENTAL_USB),1)
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

LFLAGS += -march=armv8-m.main+dsp -mthumb -mcmse -mfloat-abi=softfp -mfpu=fpv5-sp-d16 -Os -nostartfiles -specs=nosys.specs -nodefaultlibs -nostdlib
LFLAGS += -Wl,--gc-sections -Wl,-Map=$(BIN_DIR)/$(TARGET).map -Wl,--cref -Wl,--build-id=none -Wl,--use-blx
ifeq ($(I2C_BITBANG_PACING),1)
LFLAGS += -Wl,--wrap=gpio_init -Wl,--wrap=gpio_write
endif
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
LFLAGS += -Wl,-wrap,usb_os_sema_give



LIBFLAGS =
LIBFLAGS += ../../../component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/lib/lib/hal_pmc_hs.a
LIBFLAGS += -L../../../component/soc/realtek/8195b/misc/bsp/lib/common/GCC/
WLAN_RX_HOOK_DIR := ../../../component/common/drivers/wlan/realtek/wlan_rx_gdma
WLAN_RX_HOOK_ARCHIVE := $(WLAN_RX_HOOK_DIR)/build/lib_wlan_zero_copy.a
WLAN_RX_HOOK_ORIGINAL := ../../../component/soc/realtek/8195b/misc/bsp/lib/common/GCC/lib_wlan.a
WLAN_RX_HOOK_MP_FIXED := $(WLAN_RX_HOOK_DIR)/build/lib_wlan_mp.a
WLAN_RX_HOOK_MP_ORIGINAL := ../../../component/soc/realtek/8195b/misc/bsp/lib/common/GCC/lib_wlan_mp.a
ifeq ($(WLAN_RX_DMA_SKB),1)
WLAN_RX_HOOK_LIBRARY := $(WLAN_RX_HOOK_ARCHIVE)
else
WLAN_RX_HOOK_LIBRARY := -l_wlan
endif
ifeq ($(CARBOX_RECOVERY),1)
# 使用同一原厂 MP 库作为输入，仅应用 MP 专用 A-MSDU 所有权/分配失败修复。
# 不替换 MP 的 RX/skbuff、校准算法或 adapter 布局。栈保护由 freertos_service.c 提供。
MP_LAYOUT_LIBRARY_SHA256 := cab792c356dfece332ea395784163a369144198917e95597564e00c0630c26dd
ifneq ($(shell sha256sum $(WLAN_RX_HOOK_MP_ORIGINAL) | cut -d ' ' -f 1),$(MP_LAYOUT_LIBRARY_SHA256))
$(error MP driver changed: review adapter layout before enabling MP readiness checks)
endif
GCCFLAGS += -DCARBOX_MP_DRIVER_LAYOUT_20261106=1
all: LIBFLAGS += -l_soc_is $(WLAN_RX_HOOK_MP_FIXED) -l_wps -l_qr_code -l_tftp -l_http
else
all: LIBFLAGS += -l_codec -l_dct -l_haac -l_hmp3 -l_http -l_mmf -l_muxer -l_p2p -l_rtsp -l_sdcard -l_soc_is -l_speex  -l_websocket $(WLAN_RX_HOOK_LIBRARY) -l_wps -l_qr_code -l_tftp -l_opusenc -l_opusfile -l_opus
mp: LIBFLAGS += -l_codec -l_dct -l_haac -l_hmp3 -l_http -l_mmf -l_muxer -l_p2p -l_rtsp -l_sdcard -l_soc_is -l_speex  -l_websocket -l_wlan_mp -l_wps -l_qr_code -l_tftp -l_opusenc -l_opusfile -l_opus
all: LIBFLAGS += -lrtstream -lrtscamkit -lrtsv4l2
endif
LIBFLAGS += -Wl,-u,ram_start -Wl,-u,cinit_start
ifeq ($(CARBOX_USB_LIB),1)
application: $(CARBOX_USB_VENDOR_ARCHIVE)
LIBFLAGS += -Wl,--whole-archive $(CARBOX_USB_VENDOR_ARCHIVE) -Wl,--no-whole-archive
else ifeq ($(CARBOX_RECOVERY),1)
# 按需链接 lib_usbsmart.a（只提取 carbox_wifi_compat.o 等被引用对象，不用 whole-archive）
application: $(CARBOX_USB_VENDOR_ARCHIVE)
LIBFLAGS += $(CARBOX_USB_VENDOR_ARCHIVE)
endif

ifneq ($(CARBOX_RECOVERY),1)
CARBOX_SMART_CARPLAY_LIB_DIR := carplay_app
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
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_Accessory2.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_jpeg.a
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_CarPlay.a
LIBFLAGS += -Wl,--no-whole-archive
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
	@# 与主线分区一致：FW1=0x40000，FW2=0x3c0000，均为0x370000。
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
	@if [ -f $(VFSDIR)/Version.txt ]; then \
		chmod 777 $(VFSDIR)/Version.txt; \
	fi
	@chmod 777 $(UTILITYDIR)/vfs.py
endif


# Compile
# -------------------------------------------------------------------

.PHONY: application
ifeq ($(CARBOX_RECOVERY),1)
# 每次链接前由受哈希保护的原厂库构建/检查 MP 专用修补库。
.PHONY: wlan_mp_fix
wlan_mp_fix:
	@$(MAKE) -C $(WLAN_RX_HOOK_DIR) CROSS_COMPILE=$(abspath $(CROSS_COMPILE)) \
		MP_ORIGINAL_ARCHIVE=$(abspath $(WLAN_RX_HOOK_MP_ORIGINAL)) mp-fix
	@python3 $(WLAN_RX_HOOK_DIR)/src/verify_amsdu_child_adapter.py \
		--prefix $(CROSS_COMPILE) --archive $(WLAN_RX_HOOK_MP_FIXED)
application: wlan_mp_fix
else ifeq ($(WLAN_RX_DMA_SKB),1)
.PHONY: wlan_rx_hook
wlan_rx_hook:
	@$(MAKE) -C $(WLAN_RX_HOOK_DIR) \
		CROSS_COMPILE=$(abspath $(CROSS_COMPILE)) \
		ORIGINAL_ARCHIVE=$(abspath $(WLAN_RX_HOOK_ORIGINAL)) zero-copy
application: wlan_rx_hook
endif
# build_info is phony and refreshes BOX_APP_VERSION on every build.  Keep the
# objects embedding that value ordered behind it so versions.json and the
# running firmware cannot get different version names during parallel builds.
$(OBJ_DIR)/recovery_main.o $(OBJ_DIR)/fault_dump.o: build_info
$(OBJ_LIST): | prerequirement prebuild
application: | prebuild

application: prerequirement $(SRC_O) $(ERAM_O) $(SRAM_O) $(CINIT_O) $(ASM_O) $(ITCM_O) $(CPP_O) $(OVERRIDE_O)
	@echo "  LD   linking..."
	@$(LD) $(LFLAGS) -o $(BIN_DIR)/$(TARGET).axf  $(OBJ_LIST) $(ROMIMG) $(LIBFLAGS) -T$(LDSCRIPT)
	@python3 $(WLAN_RX_HOOK_DIR)/src/verify_amsdu_child_adapter.py \
		--prefix $(CROSS_COMPILE) --archive $(WLAN_RX_HOOK_MP_FIXED) --elf $(BIN_DIR)/$(TARGET).axf
	@for sym in $(CLOCK_CRITICAL_SYMBOLS); do \
		addr=$$($(NM) -an $(BIN_DIR)/$(TARGET).axf | awk -v s=$$sym '$$3 == s { print $$1; exit }'); \
		case "$$addr" in \
			201*) ;; \
			*) echo "ERROR: SPIC/PLL critical symbol $$sym is not in internal SRAM (address=$$addr)"; exit 1 ;; \
		esac; \
	done
	@$(OBJDUMP) -d $(BIN_DIR)/$(TARGET).axf > $(BIN_DIR)/$(TARGET).asm



# Manipulate Image
# -------------------------------------------------------------------

CARBOX_FATFS_BIN ?= application_is/fatfs.bin
CARBOX_OTA_FATFS_BIN ?= application_is/ota_fatfs.bin
CARBOX_OTA_ALL_BIN ?= application_is/ota_all.bin
CARBOX_OTA_FW_VER ?= $(shell date +%y%m%d%H%M)
CARBOX_FW_SERIAL ?= $(shell date +%s)
# recovery（fw2 兑底槽）固定用最小 serial，保证 bootloader 默认加载 fw1（normal）。
# bootloader 按 serial_no 大小选槽（大者优先）：normal 用时间戳（大），recovery 固定为 1，
# 只有 fw1 无效（OTA 断电等）时才 fallback 到 fw2。
ifeq ($(CARBOX_RECOVERY),1)
CARBOX_FW_SERIAL := 1
endif
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
		$(VFSTOOL) -t FATFS -s 512 -c $(FATFS_SECTORS) -dir $(VFSDIR) -out application_is/fatfs.bin >/dev/null 2>&1; \
	fi
	@mkdir -p bootloader
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
	@if [ "$(CARBOX_RECOVERY)" = "1" ]; then \
		echo "  ELF2BIN  combine application_is/flash_is.bin (recovery: FW2 only)"; \
		$(ELF2BIN) combine application_is/flash_is.bin PTAB=partition.bin,BOOT=application_is/boot.bin,FW2=application_is/firmware_is.bin >/dev/null 2>&1; \
	elif [ "$(CARBOX_LINK_FW)" = "3" ]; then \
		if [ -f application_is/firmware_is_ota2.bin ]; then \
			echo "  ELF2BIN  combine application_is/flash_is.bin (normal + recovery FW2)"; \
			$(ELF2BIN) combine application_is/flash_is.bin PTAB=partition.bin,BOOT=application_is/boot.bin,FW1=application_is/firmware_is.bin,FW2=application_is/firmware_is_ota2.bin >/dev/null 2>&1; \
		else \
			echo "  ELF2BIN  combine application_is/flash_is.bin (FW1 only, NO recovery!)"; \
			$(ELF2BIN) combine application_is/flash_is.bin PTAB=partition.bin,BOOT=application_is/boot.bin,FW1=application_is/firmware_is.bin >/dev/null 2>&1; \
			echo "  WARNING: firmware_is_ota2.bin missing, fw2 slot empty. Run 'make flash_full' for a full image."; \
		fi; \
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
	@if [ -f .box_ver ]; then \
		BOX_VER="`cat .box_ver`"; \
	else \
		BOX_VER="`date +%Y%m%d%H%M%S`_$(shell od -An -N2 -tx1 /dev/urandom | tr -d ' \n' | tr 'a-f' 'A-F')"; \
		echo "$$BOX_VER" > .box_ver; \
	fi; \
	echo \#define BOX_APP_VERSION \"$$BOX_VER\" >> .ver; \
	mkdir -p $(TARGET); \
	echo "{ \"versions\": [\"$$BOX_VER\"] }" > $(TARGET)/versions.json; \
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
	@echo "Clock profile: $(SYSTEM_CLOCK_PROFILE) MHz (PLL overclock=$(SYS_PLL_OVERCLOCK), target=$(SYS_PLL_TARGET_HZ) Hz)"
	@echo ===========================================================
	@mkdir -p $(OBJ_DIR)
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(INFO_DIR)
	@mkdir -p $(BOOT_BIN_DIR)

$(ASM_O): $(OBJ_DIR)/%.o : %.S
	#$(AS) -march=armv8-m.main+dsp -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16 -g $< -o $@
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@mv $(notdir $*.s) $(INFO_DIR)

$(CPP_O): $(OBJ_DIR)/%.o : %.cpp
	@echo "  CC   $<"
	@$(CC) $(CPPFLAGS) $(INCLUDES) -c $< -o $@
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)

$(SRC_O): $(OBJ_DIR)/%.o : %.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS)  $(INCLUDES) -c $< -o $@
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)

$(SRAM_O): $(OBJ_DIR)/%.o : %.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@$(OBJCOPY) --prefix-alloc-sections .sram $@
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)

$(ERAM_O): $(OBJ_DIR)/%.o : %.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@$(OBJCOPY) --prefix-alloc-sections .eram $@
	# Keep only the frequently traversed TCP segment descriptors and their
	# free-list head in internal SRAM.  TCP payload buffers and ownership stay
	# unchanged; all other lwIP pools remain in LPDDR.
	@case "$(@F)" in \
		memp.o) \
			$(OBJCOPY) \
				--rename-section .eram.bss.memp_memory_TCP_SEG_base=.sram.bss.memp_memory_TCP_SEG_base \
				--rename-section .eram.bss.memp_tab_TCP_SEG=.sram.bss.memp_tab_TCP_SEG \
				$@ ;; \
	 esac
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)

$(CINIT_O): $(OBJ_DIR)/%.o : %.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@$(OBJCOPY) --prefix-alloc-sections .cinit $@
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)

$(ITCM_O): $(OBJ_DIR)/%.o : %.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@$(OBJCOPY) --prefix-alloc-sections .itcm $@
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)

$(DTCM_O): $(OBJ_DIR)/%.o : %.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@$(OBJCOPY) --prefix-alloc-sections .dtcm $@
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)

# 同名源文件显式规则（vpath 会命中错误目录，这里强制指定正确源文件）
$(OBJ_DIR)/libc_wrap.o: ../../../component/soc/realtek/8195b/misc/utilities/source/ram/libc_wrap.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@$(OBJCOPY) --prefix-alloc-sections .cinit $@
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)

$(OBJ_DIR)/log_service.o: ../../../component/soc/realtek/8195b/misc/driver/log_service.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)

$(OBJ_DIR)/md4.o: ../../../component/common/network/ssl/mbedtls-2.4.0/library/md4.c
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@mv $(notdir $*.i) $(INFO_DIR)
	@mv $(notdir $*.s) $(INFO_DIR)

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
