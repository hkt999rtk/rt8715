# Cortex-M33 ChaCha20-Poly1305 replacement for the RTL8195B CarPlay archive.

PROJECT_TOOLCHAIN := ../../../../../tools/arm-none-eabi-gcc/asdk/linux/newlib/bin

ifeq ($(wildcard $(PROJECT_TOOLCHAIN)/arm-none-eabi-gcc),)
CROSS_COMPILE ?= arm-none-eabi-
else
CROSS_COMPILE ?= $(PROJECT_TOOLCHAIN)/arm-none-eabi-
endif

CC := $(CROSS_COMPILE)gcc
# GNU ar only manages the archive container; it does not generate target
# machine code.  Fall back to host ar for customer toolchains that omit the
# cross binary, and override stale recursive command-line values.
CROSS_AR := $(strip $(shell if [ -x "$(CROSS_COMPILE)ar" ]; then \
	printf '%s' "$(CROSS_COMPILE)ar"; else \
	command -v "$(CROSS_COMPILE)ar" 2>/dev/null; fi))
ifeq ($(CROSS_AR),)
override AR := $(strip $(shell command -v ar 2>/dev/null))
ifeq ($(AR),)
$(error Neither $(CROSS_COMPILE)ar nor a host GNU ar was found)
endif
else
override AR := $(CROSS_AR)
endif
NM := $(CROSS_COMPILE)nm
OBJCOPY := $(CROSS_COMPILE)objcopy

OUTPUT_DIR ?= build
VENDOR_CARPLAY_LIB ?= ../lib_CarPlay.a
REPLACEMENT_CARPLAY_LIB := $(OUTPUT_DIR)/lib_CarPlay_chacha_m33.a
STANDALONE_LIB := $(OUTPUT_DIR)/libchacha_m33.a
VENDOR_PRE_RX_LIB := $(OUTPUT_DIR)/libchacha_vendor_pre_rx.a
VENDOR_PRIVATE_MEM_LIB := $(OUTPUT_DIR)/lib_CarPlay_vendor_private_mem.a
VENDOR_PRIVATE_SW_LIB := $(OUTPUT_DIR)/lib_CarPlay_vendor_private_sw.a
CHACHA_ENABLE_CLEAR ?= 0
CHACHA_MODE ?= 0
CHACHA_HW_MIN_LEN ?= 4096
SCREEN_TX_DIRECT_CRYPTO ?= 1
CHACHA_NONALIGNED_SW_POLY ?= 0
CHACHA_STATS_INTERVAL_MS ?= 5000
CHACHA_HW_SELFTEST ?= 0
CHACHA_COMBINED_PARTIAL_SELFTEST ?= 0
CHACHA_TRANSACTION_TRACE ?= 0
CHACHA_PRIVATE_MEMORY ?= 0
CRYPTO_OWNER_BOOST_PRIORITY ?= 11
OPT_FLAGS ?= -O3

OPT_TAG := $(patsubst -%,%,$(firstword $(OPT_FLAGS)))
OBJ_DIR := $(OUTPUT_DIR)/obj-mode$(CHACHA_MODE)-clear$(CHACHA_ENABLE_CLEAR)-min$(CHACHA_HW_MIN_LEN)-direct$(SCREEN_TX_DIRECT_CRYPTO)-naswp$(CHACHA_NONALIGNED_SW_POLY)-stats$(CHACHA_STATS_INTERVAL_MS)-selftest$(CHACHA_HW_SELFTEST)-partialtest$(CHACHA_COMBINED_PARTIAL_SELFTEST)-trace$(CHACHA_TRANSACTION_TRACE)-privmem$(CHACHA_PRIVATE_MEMORY)-prio$(CRYPTO_OWNER_BOOST_PRIORITY)-$(OPT_TAG)
CORE_OBJ := $(OBJ_DIR)/ChaCha20Poly1305.o
RTL_OBJ := $(OBJ_DIR)/ChaCha20Poly1305_rtl8195b.o
AES_PATCHED_OBJ := $(OBJ_DIR)/AESUtils.o
NETTRANSPORT_PATCHED_OBJ := $(OBJ_DIR)/NetTransportChaCha20Poly1305.o
OBJS := $(CORE_OBJ) \
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
	-I$(SDK_ROOT)/project/realtek_amebapro_v0_example/src/carbox \
	-I$(SDK_ROOT)/component/os/freertos \
	-I$(SDK_ROOT)/component/os/freertos/freertos_v10.0.0/include \
	-I$(SDK_ROOT)/component/os/freertos/freertos_v10.0.0/portable/GCC/ARM_RTL8195B \
	-I$(SDK_ROOT)/component/os/os_dep/include
COMMON_FLAGS := $(OPT_FLAGS) -Wall -Wextra -ffreestanding -fno-builtin \
	-ffunction-sections -fdata-sections -fomit-frame-pointer \
	-fno-lto $(ARCH_FLAGS)
CFLAGS ?=
CFLAGS += $(COMMON_FLAGS) -DCHACHA_ENABLE_CLEAR=$(CHACHA_ENABLE_CLEAR) \
	-DCARBOX_CHACHA_MODE=$(CHACHA_MODE) \
	-DCARBOX_CHACHA_HW_MIN_LEN=$(CHACHA_HW_MIN_LEN) \
	-DCARBOX_CHACHA_NONALIGNED_SW_POLY=$(CHACHA_NONALIGNED_SW_POLY) \
	-DCARBOX_CHACHA_STATS_INTERVAL_MS=$(CHACHA_STATS_INTERVAL_MS) \
	-DCARBOX_CHACHA_HW_SELFTEST=$(CHACHA_HW_SELFTEST) \
	-DCARBOX_CHACHA_COMBINED_PARTIAL_SELFTEST=$(CHACHA_COMBINED_PARTIAL_SELFTEST) \
	-DCARBOX_CHACHA_TRANSACTION_TRACE=$(CHACHA_TRANSACTION_TRACE) \
	-DCARBOX_SCREEN_TX_DIRECT_CRYPTO=$(SCREEN_TX_DIRECT_CRYPTO) \
	-DCARBOX_CRYPTO_OWNER_BOOST_PRIORITY=$(CRYPTO_OWNER_BOOST_PRIORITY) \
	$(RTL_INCLUDES)
ifeq ($(CHACHA_PRIVATE_MEMORY),1)
CFLAGS += -Dmemcpy=carbox_vendor_chacha_memcpy \
	-Dmemset=carbox_vendor_chacha_memset
endif

.PHONY: all replacement standalone vendor-pre-rx vendor-private-mem vendor-private-sw check clean print-config host-check FORCE

all: replacement

replacement: $(REPLACEMENT_CARPLAY_LIB)

standalone: $(STANDALONE_LIB)

vendor-pre-rx: $(VENDOR_PRE_RX_LIB)

vendor-private-mem: $(VENDOR_PRIVATE_MEM_LIB)

vendor-private-sw: $(VENDOR_PRIVATE_SW_LIB)

$(OUTPUT_DIR) $(OBJ_DIR):
	mkdir -p $@

$(OBJ_DIR)/ChaCha20Poly1305.o: ChaCha20Poly1305.c ChaCha20Poly1305.h Makefile \
	$(SDK_ROOT)/project/realtek_amebapro_v0_example/src/carbox/screen_tx_direct_crypto.h \
	$(SDK_ROOT)/project/realtek_amebapro_v0_example/src/carbox/crypto_priority_lock.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
	@if [ "$(CHACHA_PRIVATE_MEMORY)" = 1 ]; then \
		$(OBJCOPY) --redefine-sym memcpy=carbox_vendor_chacha_memcpy \
			--redefine-sym memset=carbox_vendor_chacha_memset $@; \
	fi

$(OBJ_DIR)/ChaCha20Poly1305_rtl8195b.o: ChaCha20Poly1305_rtl8195b.c ChaCha20Poly1305_rtl8195b.h ChaCha20Poly1305.h Makefile | $(OBJ_DIR)
	$(CC) $(CFLAGS) -Wno-unused-parameter -c $< -o $@
	@if [ "$(CHACHA_PRIVATE_MEMORY)" = 1 ]; then \
		$(OBJCOPY) --redefine-sym memcpy=carbox_vendor_chacha_memcpy \
			--redefine-sym memset=carbox_vendor_chacha_memset $@; \
	fi

# The customer archive contains AESUtils.o built from aes_rtl8195b/AESUtils.c,
# but its proprietary support headers are not shipped in this workspace. Keep
# that exact object and redirect only its crypto lock calls to the API-level
# priority-aware lock used by the matching source. Other members are unchanged.
$(AES_PATCHED_OBJ): $(VENDOR_CARPLAY_LIB) Makefile | $(OBJ_DIR)
	cd $(OBJ_DIR) && $(abspath $(AR)) x $(abspath $(VENDOR_CARPLAY_LIB)) AESUtils.o
	$(OBJCOPY) \
		--redefine-sym device_mutex_lock=carbox_crypto_aes_device_lock \
		--redefine-sym device_mutex_unlock=carbox_crypto_aes_device_unlock \
		--redefine-sym rtl_crypto_irq_enable=carbox_crypto_irq_controller_vendor_enable \
		--globalize-symbol=gAESRTLHWState \
		--globalize-symbol=_AES_RTL_SW_ECB $@

# NetTransport owns the record-level copies surrounding ChaCha.  Redirect only
# this member's memory calls to a pre-RX dispatcher; no other CarPlay object is
# changed, and the dispatcher resumes the normal global wrappers in RX.
$(NETTRANSPORT_PATCHED_OBJ): $(VENDOR_CARPLAY_LIB) Makefile | $(OBJ_DIR)
	cd $(OBJ_DIR) && $(abspath $(AR)) x \
		$(abspath $(VENDOR_CARPLAY_LIB)) NetTransportChaCha20Poly1305.o
	$(OBJCOPY) \
		--redefine-sym memcpy=carbox_chacha_pre_rx_memcpy \
		--redefine-sym memset=carbox_chacha_pre_rx_memset $@

$(STANDALONE_LIB): $(OBJS)
	$(AR) rcs $@ $^

# Keep a private copy of only the customer's two ChaCha objects.  Every public
# and backend symbol is namespaced so this archive can coexist with the normal
# RX implementation.  Redirect memory primitives only inside these two
# objects; the rest of lib_CarPlay.a and the RX loop retain the global wrappers.
$(VENDOR_PRE_RX_LIB): $(VENDOR_CARPLAY_LIB) Makefile | $(OUTPUT_DIR)
	rm -rf $(OUTPUT_DIR)/vendor-pre-rx
	mkdir -p $(OUTPUT_DIR)/vendor-pre-rx
	cd $(OUTPUT_DIR)/vendor-pre-rx && \
		$(abspath $(AR)) x $(abspath $(VENDOR_CARPLAY_LIB)) \
			ChaCha20Poly1305.o ChaCha20Poly1305_rtl8195b.o
	$(OBJCOPY) \
		--redefine-sym chacha20_xor=carbox_vendor_chacha20_xor \
		--redefine-sym chacha20_poly1305_init_64x64=carbox_vendor_chacha20_poly1305_init_64x64 \
		--redefine-sym chacha20_poly1305_add_aad=carbox_vendor_chacha20_poly1305_add_aad \
		--redefine-sym chacha20_poly1305_encrypt=carbox_vendor_chacha20_poly1305_encrypt \
		--redefine-sym chacha20_poly1305_decrypt=carbox_vendor_chacha20_poly1305_decrypt \
		--redefine-sym chacha20_poly1305_final=carbox_vendor_chacha20_poly1305_final \
		--redefine-sym chacha20_poly1305_verify=carbox_vendor_chacha20_poly1305_verify \
		--redefine-sym chacha20_poly1305_encrypt_all_64x64=carbox_vendor_chacha20_poly1305_encrypt_all_64x64 \
		--redefine-sym chacha20_poly1305_decrypt_all_64x64=carbox_vendor_chacha20_poly1305_decrypt_all_64x64 \
		--redefine-sym carbox_chacha_aad_realloc=carbox_vendor_chacha_aad_realloc \
		--redefine-sym carbox_chacha_hw_fatal=carbox_vendor_chacha_hw_fatal \
		--redefine-sym chacha_rtl8195b_chacha_xor=carbox_vendor_chacha_rtl8195b_chacha_xor \
		--redefine-sym chacha_rtl8195b_decrypt=carbox_vendor_chacha_rtl8195b_decrypt \
		--redefine-sym chacha_rtl8195b_encrypt=carbox_vendor_chacha_rtl8195b_encrypt \
		--redefine-sym chacha_rtl8195b_poly1305=carbox_vendor_chacha_rtl8195b_poly1305 \
		--redefine-sym chacha_rtl8195b_precheck_context=carbox_vendor_chacha_rtl8195b_precheck_context \
		--redefine-sym chacha_rtl8195b_status_string=carbox_vendor_chacha_rtl8195b_status_string \
		--redefine-sym memcpy=carbox_vendor_chacha_memcpy \
		--redefine-sym memset=carbox_vendor_chacha_memset \
		$(OUTPUT_DIR)/vendor-pre-rx/ChaCha20Poly1305.o
	$(OBJCOPY) \
		--redefine-sym chacha_rtl8195b_chacha_xor=carbox_vendor_chacha_rtl8195b_chacha_xor \
		--redefine-sym chacha_rtl8195b_decrypt=carbox_vendor_chacha_rtl8195b_decrypt \
		--redefine-sym chacha_rtl8195b_encrypt=carbox_vendor_chacha_rtl8195b_encrypt \
		--redefine-sym chacha_rtl8195b_poly1305=carbox_vendor_chacha_rtl8195b_poly1305 \
		--redefine-sym chacha_rtl8195b_precheck_context=carbox_vendor_chacha_rtl8195b_precheck_context \
		--redefine-sym chacha_rtl8195b_status_string=carbox_vendor_chacha_rtl8195b_status_string \
		--redefine-sym carbox_chacha_hw_fatal=carbox_vendor_chacha_hw_fatal \
		--redefine-sym memcpy=carbox_vendor_chacha_memcpy \
		--redefine-sym memset=carbox_vendor_chacha_memset \
		$(OUTPUT_DIR)/vendor-pre-rx/ChaCha20Poly1305_rtl8195b.o
	$(AR) rcs $@ \
		$(OUTPUT_DIR)/vendor-pre-rx/ChaCha20Poly1305.o \
		$(OUTPUT_DIR)/vendor-pre-rx/ChaCha20Poly1305_rtl8195b.o

# Minimal diagnostic archive: retain the customer's exact ChaCha and
# NetTransport implementations, changing only their memory symbol bindings.
$(VENDOR_PRIVATE_MEM_LIB): $(VENDOR_CARPLAY_LIB) Makefile | $(OUTPUT_DIR)
	rm -rf $(OUTPUT_DIR)/vendor-private-mem
	mkdir -p $(OUTPUT_DIR)/vendor-private-mem
	cp -f $(VENDOR_CARPLAY_LIB) $@
	chmod u+w $@
	cd $(OUTPUT_DIR)/vendor-private-mem && \
		$(abspath $(AR)) x $(abspath $(VENDOR_CARPLAY_LIB)) \
			ChaCha20Poly1305.o ChaCha20Poly1305_rtl8195b.o \
			NetTransportChaCha20Poly1305.o
	for obj in \
		$(OUTPUT_DIR)/vendor-private-mem/ChaCha20Poly1305.o \
		$(OUTPUT_DIR)/vendor-private-mem/ChaCha20Poly1305_rtl8195b.o \
		$(OUTPUT_DIR)/vendor-private-mem/NetTransportChaCha20Poly1305.o; do \
		$(OBJCOPY) \
			--redefine-sym memcpy=carbox_vendor_chacha_memcpy \
			--redefine-sym memset=carbox_vendor_chacha_memset $$obj; \
	done
	$(AR) d $@ ChaCha20Poly1305.o ChaCha20Poly1305_rtl8195b.o \
		NetTransportChaCha20Poly1305.o
	$(AR) rcs $@ \
		$(OUTPUT_DIR)/vendor-private-mem/ChaCha20Poly1305.o \
		$(OUTPUT_DIR)/vendor-private-mem/ChaCha20Poly1305_rtl8195b.o \
		$(OUTPUT_DIR)/vendor-private-mem/NetTransportChaCha20Poly1305.o

# Preserve the customer's exact ChaCha object, but redirect its one hardware
# eligibility call so the software implementation already compiled into that
# object is authoritative.  Memory redirection remains local to the same three
# customer objects.
$(VENDOR_PRIVATE_SW_LIB): $(VENDOR_CARPLAY_LIB) Makefile | $(OUTPUT_DIR)
	rm -rf $(OUTPUT_DIR)/vendor-private-sw
	mkdir -p $(OUTPUT_DIR)/vendor-private-sw
	cp -f $(VENDOR_CARPLAY_LIB) $@
	chmod u+w $@
	cd $(OUTPUT_DIR)/vendor-private-sw && \
		$(abspath $(AR)) x $(abspath $(VENDOR_CARPLAY_LIB)) \
			ChaCha20Poly1305.o ChaCha20Poly1305_rtl8195b.o \
			NetTransportChaCha20Poly1305.o
	$(OBJCOPY) \
		--redefine-sym memcpy=carbox_vendor_chacha_memcpy \
		--redefine-sym memset=carbox_vendor_chacha_memset \
		--redefine-sym chacha_rtl8195b_precheck_context=carbox_vendor_chacha_force_software_precheck \
		$(OUTPUT_DIR)/vendor-private-sw/ChaCha20Poly1305.o
	for obj in \
		$(OUTPUT_DIR)/vendor-private-sw/ChaCha20Poly1305_rtl8195b.o \
		$(OUTPUT_DIR)/vendor-private-sw/NetTransportChaCha20Poly1305.o; do \
		$(OBJCOPY) \
			--redefine-sym memcpy=carbox_vendor_chacha_memcpy \
			--redefine-sym memset=carbox_vendor_chacha_memset $$obj; \
	done
	$(AR) d $@ ChaCha20Poly1305.o ChaCha20Poly1305_rtl8195b.o \
		NetTransportChaCha20Poly1305.o
	$(AR) rcs $@ \
		$(OUTPUT_DIR)/vendor-private-sw/ChaCha20Poly1305.o \
		$(OUTPUT_DIR)/vendor-private-sw/ChaCha20Poly1305_rtl8195b.o \
		$(OUTPUT_DIR)/vendor-private-sw/NetTransportChaCha20Poly1305.o

$(REPLACEMENT_CARPLAY_LIB): $(VENDOR_CARPLAY_LIB) $(CORE_OBJ) $(RTL_OBJ) \
		$(AES_PATCHED_OBJ) $(NETTRANSPORT_PATCHED_OBJ) Makefile | $(OUTPUT_DIR)
	cp -f $(VENDOR_CARPLAY_LIB) $@
	chmod u+w $@
	# Replace only the CarPlay crypto API members; no global lock wrapper.
	$(AR) d $@ ChaCha20Poly1305.o ChaCha20Poly1305_rtl8195b.o \
		AESUtils.o NetTransportChaCha20Poly1305.o chacha_m33_asm.o
	$(AR) rcs $@ $(CORE_OBJ) $(RTL_OBJ) $(AES_PATCHED_OBJ) \
		$(NETTRANSPORT_PATCHED_OBJ)

FORCE:

check: $(REPLACEMENT_CARPLAY_LIB)
	@set -e; \
	test "$$($(AR) t $< | grep -c '^ChaCha20Poly1305[.]o$$')" -eq 1; \
	test "$$($(AR) t $< | grep -c '^chacha_m33_asm[.]o$$')" -eq 0; \
	$(NM) -A -u $< | grep 'AESUtils[.]o:.* carbox_crypto_aes_device_lock$$' >/dev/null || { \
		echo "Missing priority-aware AES lock"; exit 1; \
	}; \
	$(NM) -A -u $< | grep 'ChaCha20Poly1305_rtl8195b[.]o:.* carbox_crypto_chacha_device_lock$$' >/dev/null || { \
		echo "Missing priority-aware ChaCha lock"; exit 1; \
	}; \
	$(NM) -A -u $< | grep 'NetTransportChaCha20Poly1305[.]o:.* carbox_chacha_pre_rx_memcpy$$' >/dev/null || { \
		echo "Missing pre-RX NetTransport memcpy redirect"; exit 1; \
	}; \
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
	@echo "CRYPTO_OWNER_BOOST_PRIORITY=$(CRYPTO_OWNER_BOOST_PRIORITY)"
	@echo "VENDOR_CARPLAY_LIB=$(VENDOR_CARPLAY_LIB)"
	@echo "REPLACEMENT_CARPLAY_LIB=$(REPLACEMENT_CARPLAY_LIB)"

HOST_CC ?= cc
HOST_TEST_FLAGS := -O2 -Wall -Wextra -I.

host-check: | $(OUTPUT_DIR)
	@set -e; \
	for swpoly in 0 1; do \
	  for mode in 0 1 2 3; do \
		echo "HOST TEST CHACHA_MODE=$$mode NONALIGNED_SW_POLY=$$swpoly"; \
		$(HOST_CC) $(HOST_TEST_FLAGS) -DCARBOX_CHACHA_MODE=$$mode \
			-DCARBOX_SCREEN_TX_DIRECT_CRYPTO=0 \
			-DCARBOX_CHACHA_NONALIGNED_SW_POLY=$$swpoly \
			ChaCha20Poly1305.c tests/rtl_backend_openssl_mock.c \
			tests/host_modes.c -lcrypto \
			-o $(OUTPUT_DIR)/host-mode$$mode-swpoly$$swpoly; \
		$(OUTPUT_DIR)/host-mode$$mode-swpoly$$swpoly; \
	  done; \
	done

clean:
	rm -rf $(OUTPUT_DIR)
