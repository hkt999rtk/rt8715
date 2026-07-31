# Cortex-M33 ChaCha20-Poly1305 replacement for the RTL8195B CarPlay archive.

PROJECT_TOOLCHAIN := ../../../../../tools/arm-none-eabi-gcc/asdk/linux/newlib/bin

ifeq ($(wildcard $(PROJECT_TOOLCHAIN)/arm-none-eabi-gcc),)
CROSS_COMPILE ?= arm-none-eabi-
else
CROSS_COMPILE ?= $(PROJECT_TOOLCHAIN)/arm-none-eabi-
endif

CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar
NM := $(CROSS_COMPILE)nm

OUTPUT_DIR ?= build
VENDOR_CARPLAY_LIB ?= ../lib_CarPlay.a
REPLACEMENT_CARPLAY_LIB := $(OUTPUT_DIR)/lib_CarPlay_chacha_m33.a
STANDALONE_LIB := $(OUTPUT_DIR)/libchacha_m33.a
CHACHA_ENABLE_CLEAR ?= 0
CHACHA_MODE ?= 0
CHACHA_HW_MIN_LEN ?= 4096
CHACHA_NONALIGNED_SW_POLY ?= 0
CHACHA_STATS_INTERVAL_MS ?= 5000
CHACHA_HW_SELFTEST ?= 0
OPT_FLAGS ?= -O3

OPT_TAG := $(patsubst -%,%,$(firstword $(OPT_FLAGS)))
OBJ_DIR := $(OUTPUT_DIR)/obj-mode$(CHACHA_MODE)-clear$(CHACHA_ENABLE_CLEAR)-min$(CHACHA_HW_MIN_LEN)-naswp$(CHACHA_NONALIGNED_SW_POLY)-stats$(CHACHA_STATS_INTERVAL_MS)-selftest$(CHACHA_HW_SELFTEST)-$(OPT_TAG)
OBJS := $(OBJ_DIR)/ChaCha20Poly1305.o \
	$(OBJ_DIR)/ChaCha20Poly1305_rtl8195b.o

ARCH_FLAGS := -march=armv8-m.main+dsp -mthumb -mcmse \
	-mfloat-abi=softfp -mfpu=fpv5-sp-d16
SDK_ROOT := $(abspath ../../../../..)
RTL_INCLUDES := \
	-I$(SDK_ROOT)/project/realtek_amebapro_v0_example/inc \
	-I$(SDK_ROOT)/component/soc/realtek/8195b/cmsis/rtl8195b-hp/include \
	-I$(SDK_ROOT)/component/soc/realtek/8195b/cmsis/cmsis-core/include \
	-I$(SDK_ROOT)/component/soc/realtek/8195b/app/rtl_printf/include \
	-I$(SDK_ROOT)/component/soc/realtek/8195b/app/stdio_port \
	-I$(SDK_ROOT)/component/soc/realtek/8195b/misc/utilities/include \
	-I$(SDK_ROOT)/component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/include \
	-I$(SDK_ROOT)/component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/lib/include \
	-I$(SDK_ROOT)/component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/include/usb_otg \
	-I$(SDK_ROOT)/component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/lib/include/usb_otg \
	-I$(SDK_ROOT)/component/common/mbed/hal_ext \
	-I$(SDK_ROOT)/component/common/api/platform \
	-I$(SDK_ROOT)/component/os/freertos \
	-I$(SDK_ROOT)/component/os/freertos/freertos_v10.0.0/include \
	-I$(SDK_ROOT)/component/os/freertos/freertos_v10.0.0/portable/GCC/ARM_RTL8195B \
	-I$(SDK_ROOT)/component/os/os_dep/include
COMMON_FLAGS := $(OPT_FLAGS) -Wall -Wextra -ffreestanding -fno-builtin \
	-ffunction-sections -fdata-sections -fomit-frame-pointer \
	-mno-unaligned-access -fno-lto $(ARCH_FLAGS)
CFLAGS ?=
CFLAGS += $(COMMON_FLAGS) -DCHACHA_ENABLE_CLEAR=$(CHACHA_ENABLE_CLEAR) \
	-DCARBOX_CHACHA_MODE=$(CHACHA_MODE) \
	-DCARBOX_CHACHA_HW_MIN_LEN=$(CHACHA_HW_MIN_LEN) \
	-DCARBOX_CHACHA_NONALIGNED_SW_POLY=$(CHACHA_NONALIGNED_SW_POLY) \
	-DCARBOX_CHACHA_STATS_INTERVAL_MS=$(CHACHA_STATS_INTERVAL_MS) \
	-DCARBOX_CHACHA_HW_SELFTEST=$(CHACHA_HW_SELFTEST) \
	$(RTL_INCLUDES)

.PHONY: all replacement standalone check clean print-config host-check FORCE

all: replacement

replacement: $(REPLACEMENT_CARPLAY_LIB)

standalone: $(STANDALONE_LIB)

$(OUTPUT_DIR) $(OBJ_DIR):
	mkdir -p $@

$(OBJ_DIR)/ChaCha20Poly1305.o: ChaCha20Poly1305.c ChaCha20Poly1305.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/ChaCha20Poly1305_rtl8195b.o: ChaCha20Poly1305_rtl8195b.c ChaCha20Poly1305_rtl8195b.h ChaCha20Poly1305.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -Wno-unused-parameter -c $< -o $@

$(STANDALONE_LIB): $(OBJS)
	$(AR) rcs $@ $^

$(REPLACEMENT_CARPLAY_LIB): $(VENDOR_CARPLAY_LIB) $(OBJS) FORCE | $(OUTPUT_DIR)
	cp -f $(VENDOR_CARPLAY_LIB) $@
	chmod u+w $@
	$(AR) d $@ ChaCha20Poly1305.o chacha_m33_asm.o
	$(AR) rcs $@ $(OBJS)

FORCE:

check: $(REPLACEMENT_CARPLAY_LIB)
	@set -e; \
	test "$$($(AR) t $< | grep -c '^ChaCha20Poly1305[.]o$$')" -eq 1; \
	test "$$($(AR) t $< | grep -c '^chacha_m33_asm[.]o$$')" -eq 0; \
	for symbol in \
		chacha20_poly1305_init_64x64 \
		chacha20_poly1305_add_aad \
		chacha20_poly1305_encrypt \
		chacha20_poly1305_decrypt \
		chacha20_poly1305_final \
		chacha20_poly1305_verify \
		chacha20_poly1305_encrypt_all_64x64 \
		chacha20_poly1305_decrypt_all_64x64 \
		chacha_rtl8195b_encrypt \
		chacha_rtl8195b_decrypt \
		chacha_rtl8195b_chacha_xor \
		chacha_rtl8195b_poly1305 \
		chacha_rtl8195b_precheck_context; do \
		$(NM) -g --defined-only $< | grep -q "[[:space:]]$$symbol$$" || { \
			echo "Missing required symbol: $$symbol"; \
			exit 1; \
		}; \
	done
	@echo "CarPlay ChaCha20-Poly1305 archive replacement: OK"

print-config:
	@echo "CC=$(CC)"
	@echo "AR=$(AR)"
	@echo "CFLAGS=$(CFLAGS)"
	@echo "CHACHA_MODE=$(CHACHA_MODE)"
	@echo "CHACHA_HW_MIN_LEN=$(CHACHA_HW_MIN_LEN)"
	@echo "CHACHA_NONALIGNED_SW_POLY=$(CHACHA_NONALIGNED_SW_POLY)"
	@echo "CHACHA_STATS_INTERVAL_MS=$(CHACHA_STATS_INTERVAL_MS)"
	@echo "CHACHA_HW_SELFTEST=$(CHACHA_HW_SELFTEST)"
	@echo "VENDOR_CARPLAY_LIB=$(VENDOR_CARPLAY_LIB)"
	@echo "REPLACEMENT_CARPLAY_LIB=$(REPLACEMENT_CARPLAY_LIB)"

HOST_CC ?= cc
HOST_TEST_FLAGS := -O2 -Wall -Wextra -I.

host-check: | $(OUTPUT_DIR)
	@set -e; \
	for swpoly in 0 1; do \
	  for mode in 0 1 2; do \
		echo "HOST TEST CHACHA_MODE=$$mode NONALIGNED_SW_POLY=$$swpoly"; \
		$(HOST_CC) $(HOST_TEST_FLAGS) -DCARBOX_CHACHA_MODE=$$mode \
			-DCARBOX_CHACHA_NONALIGNED_SW_POLY=$$swpoly \
			ChaCha20Poly1305.c tests/rtl_backend_openssl_mock.c \
			tests/host_modes.c -lcrypto \
			-o $(OUTPUT_DIR)/host-mode$$mode-swpoly$$swpoly; \
		$(OUTPUT_DIR)/host-mode$$mode-swpoly$$swpoly; \
	  done; \
	done

clean:
	rm -rf $(OUTPUT_DIR)
