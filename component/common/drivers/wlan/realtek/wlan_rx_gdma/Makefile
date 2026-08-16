CROSS_COMPILE ?= arm-none-eabi-
CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar
NM := $(CROSS_COMPILE)nm
OBJDUMP := $(CROSS_COMPILE)objdump

SDK_ROOT := $(abspath ../../../../../..)
# Keep the recovered driver sources with their archive integration so a clean
# clone contains every input needed to rebuild the replacement members.
RECOVERED_DIR := $(abspath src)
BUILD_DIR := build
RECOVERED_RX_SOURCE := $(RECOVERED_DIR)/rtl8195b_recv_recovered.c
RECOVERED_SKBUFF_SOURCE := $(RECOVERED_DIR)/freertos_skbuff_recovered.c
DMA_MANAGER_SOURCE := $(RECOVERED_DIR)/wlan_rx_dma_mgr.c
DMA_MANAGER_HEADER := $(RECOVERED_DIR)/wlan_rx_dma_mgr.h
RECOVERED_RX_OBJECT := $(BUILD_DIR)/rtl8195b_recv.o
COPY_PARITY_RX_OBJECT := $(BUILD_DIR)/copy_parity/rtl8195b_recv.o
RECOVERED_SKBUFF_OBJECT := $(BUILD_DIR)/freertos_skbuff.o
VENDOR_PARITY_SKBUFF_OBJECT := $(BUILD_DIR)/copy_parity/freertos_skbuff.o
DMA_MANAGER_OBJECT := $(BUILD_DIR)/wlan_rx_dma_mgr.o
ZERO_COPY_ARCHIVE := $(BUILD_DIR)/lib_wlan_zero_copy.a
COPY_PARITY_ARCHIVE := $(BUILD_DIR)/lib_wlan_rx_recovered_copy_parity.a
SKBUFF_PARITY_ARCHIVE := $(BUILD_DIR)/lib_wlan_recovered_skb_copy_parity.a
DROPIN_ARCHIVE := $(BUILD_DIR)/lib_wlan.a
VENDOR_RX_OBJECT := $(BUILD_DIR)/vendor_rtl8195b_recv.o
VENDOR_SKBUFF_OBJECT := $(BUILD_DIR)/vendor_freertos_skbuff.o
VENDOR_RX_GLOBALS := $(BUILD_DIR)/vendor_rtl8195b_recv.globals
RECOVERED_RX_GLOBALS := $(BUILD_DIR)/recovered_rtl8195b_recv.globals
VENDOR_SKBUFF_GLOBALS := $(BUILD_DIR)/vendor_freertos_skbuff.globals
RECOVERED_SKBUFF_GLOBALS := $(BUILD_DIR)/recovered_freertos_skbuff.globals
VENDOR_RX_DATA_ABI := $(BUILD_DIR)/vendor_rtl8195b_recv.data-abi
RECOVERED_RX_DATA_ABI := $(BUILD_DIR)/recovered_rtl8195b_recv.data-abi
VENDOR_SKBUFF_DATA_ABI := $(BUILD_DIR)/vendor_freertos_skbuff.data-abi
RECOVERED_SKBUFF_DATA_ABI := $(BUILD_DIR)/recovered_freertos_skbuff.data-abi
VENDOR_ARCHIVE_GLOBALS := $(BUILD_DIR)/vendor_archive.globals
RECOVERED_ARCHIVE_GLOBALS := $(BUILD_DIR)/recovered_archive.globals
VENDOR_ARCHIVE_RAW_UNDEFINED := $(BUILD_DIR)/vendor_archive.raw-undefined
RECOVERED_ARCHIVE_RAW_UNDEFINED := $(BUILD_DIR)/recovered_archive.raw-undefined
VENDOR_ARCHIVE_EXTERNALS := $(BUILD_DIR)/vendor_archive.externals
RECOVERED_ARCHIVE_EXTERNALS := $(BUILD_DIR)/recovered_archive.externals

CFLAGS := -march=armv8-m.main+dsp -mthumb -mcmse -mfloat-abi=softfp
CFLAGS += -mfpu=fpv5-sp-d16 -Os -c -ffunction-sections -fdata-sections
CFLAGS += -Wall -Wextra -Werror
CFLAGS += -DCONFIG_PLATFORM_8195B -DCONFIG_PLATFORM_8195BHP
CFLAGS += -DCONFIG_BUILD_RAM=1 -DCONFIG_BUILD_LIB=1
CFLAGS += -I$(SDK_ROOT)/project/realtek_amebapro_v0_example/inc
CFLAGS += -I$(SDK_ROOT)/component/common/drivers/wlan/realtek/include
CFLAGS += -I$(SDK_ROOT)/component/common/drivers/wlan/realtek/src/osdep
CFLAGS += -I$(SDK_ROOT)/component/common/drivers/wlan/realtek/src/osdep/freertos
CFLAGS += -I$(SDK_ROOT)/component/common/api
CFLAGS += -I$(SDK_ROOT)/component/common/api/platform
CFLAGS += -I$(SDK_ROOT)/component/os/os_dep/include
CFLAGS += -I$(SDK_ROOT)/component/os/freertos
CFLAGS += -I$(SDK_ROOT)/component/soc/realtek/8195b/cmsis/rtl8195b-hp/include
CFLAGS += -I$(SDK_ROOT)/component/soc/realtek/8195b/cmsis/cmsis-core/include
CFLAGS += -I$(SDK_ROOT)/component/os/freertos/freertos_v10.0.0/include
CFLAGS += -I$(SDK_ROOT)/component/os/freertos/freertos_v10.0.0/portable/GCC/ARM_RTL8195B
CFLAGS += -I$(RECOVERED_DIR)
RX_EXTRA_CFLAGS ?=

.NOTPARALLEL:
.PHONY: all check-input verify zero-copy copy-parity skbuff-parity clean

all: check-input verify $(DROPIN_ARCHIVE)

check-input:
	@test -n "$(ORIGINAL_ARCHIVE)" || \
		{ echo "ERROR: ORIGINAL_ARCHIVE is required"; exit 1; }
	@test -f "$(ORIGINAL_ARCHIVE)" || \
		{ echo "ERROR: missing vendor archive $(ORIGINAL_ARCHIVE)"; exit 1; }
	@test -f "$(RECOVERED_RX_SOURCE)" || \
		{ echo "ERROR: missing recovered source $(RECOVERED_RX_SOURCE)"; exit 1; }
	@test -f "$(RECOVERED_SKBUFF_SOURCE)" || \
		{ echo "ERROR: missing recovered source $(RECOVERED_SKBUFF_SOURCE)"; exit 1; }
	@test -f "$(DMA_MANAGER_SOURCE)" || \
		{ echo "ERROR: missing DMA manager $(DMA_MANAGER_SOURCE)"; exit 1; }

$(BUILD_DIR):
	@mkdir -p $@

$(RECOVERED_RX_OBJECT): $(RECOVERED_RX_SOURCE) $(DMA_MANAGER_HEADER) | $(BUILD_DIR)
	@echo "  CC    recovered rtl8195b_recv.c"
	@$(CC) $(CFLAGS) $(RX_EXTRA_CFLAGS) $< -o $@

$(COPY_PARITY_RX_OBJECT): $(RECOVERED_RX_SOURCE) $(DMA_MANAGER_HEADER) | $(BUILD_DIR)
	@echo "  CC    recovered rtl8195b_recv.c (vendor-copy parity)"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -DCONFIG_WLAN_RX_DMA_FORCE_COPY=1 $< -o $@

copy-parity: check-input $(COPY_PARITY_ARCHIVE)

skbuff-parity: check-input $(SKBUFF_PARITY_ARCHIVE)

zero-copy: check-input verify

$(COPY_PARITY_ARCHIVE): $(ORIGINAL_ARCHIVE) $(COPY_PARITY_RX_OBJECT) Makefile
	@cp $(ORIGINAL_ARCHIVE) $@
	@$(AR) rcsD $@ $(COPY_PARITY_RX_OBJECT)
	@test "$$($(AR) t $@ | grep -c '^rtl8195b_recv.o$$')" = 1 || \
		{ echo "ERROR: recovered RX member missing or duplicated"; exit 1; }
	@test "$$($(AR) p $@ freertos_skbuff.o | sha256sum | cut -d' ' -f1)" = \
		"$$($(AR) p $(ORIGINAL_ARCHIVE) freertos_skbuff.o | sha256sum | cut -d' ' -f1)" || \
		{ echo "ERROR: vendor freertos_skbuff.o changed"; exit 1; }
	@if $(NM) $(COPY_PARITY_RX_OBJECT) | grep -q 'wlan_rx_dma_'; then \
		echo "ERROR: copy-parity RX still references DMA manager"; exit 1; \
	fi
	@echo "  WLAN  verified recovered-RX/vendor-copy parity archive -> $@"

# Diagnostic split: vendor-copy RX semantics plus recovered skbuff ownership.
# This isolates freertos_skbuff.o from descriptor rotation and the DMA manager.
$(SKBUFF_PARITY_ARCHIVE): $(ORIGINAL_ARCHIVE) $(COPY_PARITY_RX_OBJECT) \
		$(VENDOR_PARITY_SKBUFF_OBJECT) Makefile
	@cp $(ORIGINAL_ARCHIVE) $@
	@$(AR) rcsD $@ $(COPY_PARITY_RX_OBJECT) $(VENDOR_PARITY_SKBUFF_OBJECT)
	@test "$$($(AR) t $@ | grep -c '^rtl8195b_recv.o$$')" = 1 || \
		{ echo "ERROR: recovered RX member missing or duplicated"; exit 1; }
	@test "$$($(AR) t $@ | grep -c '^freertos_skbuff.o$$')" = 1 || \
		{ echo "ERROR: recovered skbuff member missing or duplicated"; exit 1; }
	@if $(NM) $@ 2>/dev/null | grep -q 'wlan_rx_dma_'; then \
		echo "ERROR: skbuff-parity archive unexpectedly references DMA manager"; exit 1; \
	fi
	@echo "  WLAN  recovered-skb/copy-RX parity archive -> $@"

$(RECOVERED_SKBUFF_OBJECT): $(RECOVERED_SKBUFF_SOURCE) | $(BUILD_DIR)
	@echo "  CC    recovered freertos_skbuff.c (vendor + DMA-shared only)"
	@$(CC) $(CFLAGS) -DCONFIG_RECOVERED_SKBUFF_VENDOR_PARITY=1 \
		-DCONFIG_RECOVERED_SKBUFF_DMA_SHARED_ONLY=1 $< -o $@

$(VENDOR_PARITY_SKBUFF_OBJECT): $(RECOVERED_SKBUFF_SOURCE) | $(BUILD_DIR)
	@echo "  CC    recovered freertos_skbuff.c (vendor parity)"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -DCONFIG_RECOVERED_SKBUFF_VENDOR_PARITY=1 $< -o $@

$(DMA_MANAGER_OBJECT): $(DMA_MANAGER_SOURCE) $(DMA_MANAGER_HEADER) | $(BUILD_DIR)
	@echo "  CC    WLAN RX DMA buffer manager"
	@$(CC) $(CFLAGS) $< -o $@

# Keep all unmodified vendor members.  ar replaces rtl8195b_recv.o and
# freertos_skbuff.o by member name and adds the C DMA manager; rtw_recv.o
# remains byte-identical to vendor.
$(ZERO_COPY_ARCHIVE): $(ORIGINAL_ARCHIVE) $(RECOVERED_RX_OBJECT) \
		$(RECOVERED_SKBUFF_OBJECT) $(DMA_MANAGER_OBJECT) Makefile
	@cp $(ORIGINAL_ARCHIVE) $@
	@$(AR) rcsD $@ $(RECOVERED_RX_OBJECT) $(RECOVERED_SKBUFF_OBJECT) \
		$(DMA_MANAGER_OBJECT)
	@test "$$($(AR) t $@ | grep -c '^rtl8195b_recv.o$$')" = 1 || \
		{ echo "ERROR: recovered RX member missing or duplicated"; exit 1; }
	@test "$$($(AR) t $@ | grep -c '^freertos_skbuff.o$$')" = 1 || \
		{ echo "ERROR: recovered skbuff member missing or duplicated"; exit 1; }
	@test "$$($(AR) t $@ | grep -c '^wlan_rx_dma_mgr.o$$')" = 1 || \
		{ echo "ERROR: DMA manager member missing or duplicated"; exit 1; }
	@$(NM) -A $@ | grep -q ' T wlan_rx_dma_rotate$$' || \
		{ echo "ERROR: zero-copy archive missing DMA rotation"; exit 1; }
	@echo "  WLAN  vendor-parity + DMA-shared-only zero-copy archive -> $@"

# Same archive under the vendor filename for consumers that require a direct
# lib_wlan.a replacement.  Keep the descriptive filename for SDK integration.
$(DROPIN_ARCHIVE): verify
	@cp $(ZERO_COPY_ARCHIVE) $@
	@cmp -s $(ZERO_COPY_ARCHIVE) $@ || \
		{ echo "ERROR: drop-in lib_wlan.a differs from verified archive"; exit 1; }
	@echo "  WLAN  $@ (drop-in)"

verify: $(ZERO_COPY_ARCHIVE)
	@$(AR) p $(ORIGINAL_ARCHIVE) rtl8195b_recv.o > $(VENDOR_RX_OBJECT)
	@$(AR) p $(ORIGINAL_ARCHIVE) freertos_skbuff.o > $(VENDOR_SKBUFF_OBJECT)
	@$(NM) -g --defined-only -P $(VENDOR_RX_OBJECT) | \
		cut -d' ' -f1,2 | sort -u > $(VENDOR_RX_GLOBALS)
	@$(NM) -g --defined-only -P $(RECOVERED_RX_OBJECT) | \
		cut -d' ' -f1,2 | sort -u > $(RECOVERED_RX_GLOBALS)
	@cmp -s $(VENDOR_RX_GLOBALS) $(RECOVERED_RX_GLOBALS) || \
		{ echo "ERROR: recovered RX global symbol ABI differs from vendor"; \
		  diff -u $(VENDOR_RX_GLOBALS) $(RECOVERED_RX_GLOBALS); exit 1; }
	@$(NM) -g --defined-only -P $(VENDOR_SKBUFF_OBJECT) | \
		cut -d' ' -f1,2 | sort -u > $(VENDOR_SKBUFF_GLOBALS)
	@$(NM) -g --defined-only -P $(RECOVERED_SKBUFF_OBJECT) | \
		cut -d' ' -f1,2 | sort -u > $(RECOVERED_SKBUFF_GLOBALS)
	@cmp -s $(VENDOR_SKBUFF_GLOBALS) $(RECOVERED_SKBUFF_GLOBALS) || \
		{ echo "ERROR: recovered skbuff global symbol ABI differs from vendor"; \
		  diff -u $(VENDOR_SKBUFF_GLOBALS) $(RECOVERED_SKBUFF_GLOBALS); exit 1; }
	@$(NM) -g -S -P $(VENDOR_RX_OBJECT) | \
		awk '$$2 ~ /^[BbDdRrVv]$$/ {print $$1, $$2, $$4}' | \
		sort -u > $(VENDOR_RX_DATA_ABI)
	@$(NM) -g -S -P $(RECOVERED_RX_OBJECT) | \
		awk '$$2 ~ /^[BbDdRrVv]$$/ {print $$1, $$2, $$4}' | \
		sort -u > $(RECOVERED_RX_DATA_ABI)
	@cmp -s $(VENDOR_RX_DATA_ABI) $(RECOVERED_RX_DATA_ABI) || \
		{ echo "ERROR: recovered RX global data ABI differs from vendor"; \
		  diff -u $(VENDOR_RX_DATA_ABI) $(RECOVERED_RX_DATA_ABI); exit 1; }
	@$(NM) -g -S -P $(VENDOR_SKBUFF_OBJECT) | \
		awk '$$2 ~ /^[BbDdRrVv]$$/ {print $$1, $$2, $$4}' | \
		sort -u > $(VENDOR_SKBUFF_DATA_ABI)
	@$(NM) -g -S -P $(RECOVERED_SKBUFF_OBJECT) | \
		awk '$$2 ~ /^[BbDdRrVv]$$/ {print $$1, $$2, $$4}' | \
		sort -u > $(RECOVERED_SKBUFF_DATA_ABI)
	@cmp -s $(VENDOR_SKBUFF_DATA_ABI) $(RECOVERED_SKBUFF_DATA_ABI) || \
		{ echo "ERROR: recovered skbuff global data ABI differs from vendor"; \
		  diff -u $(VENDOR_SKBUFF_DATA_ABI) $(RECOVERED_SKBUFF_DATA_ABI); exit 1; }
	@set -e; for symbol in $$($(NM) -g --defined-only -P $(VENDOR_SKBUFF_OBJECT) | \
		awk '$$2 ~ /^[TtWw]$$/ {print $$1}'); do \
		$(OBJDUMP) -h $(RECOVERED_SKBUFF_OBJECT) | \
			grep -q "[.]wlan[.]text[.]$$symbol" || \
			{ echo "ERROR: recovered skbuff function $$symbol lost .wlan.text ABI"; exit 1; }; \
	done
	@$(NM) -g --defined-only -P $(ORIGINAL_ARCHIVE) 2>/dev/null | \
		awk '$$2 ~ /^[A-Za-z]$$/ {print $$1}' | sort -u > $(VENDOR_ARCHIVE_GLOBALS)
	@$(NM) -g --defined-only -P $(ZERO_COPY_ARCHIVE) 2>/dev/null | \
		awk '$$2 ~ /^[A-Za-z]$$/ {print $$1}' | sort -u > $(RECOVERED_ARCHIVE_GLOBALS)
	@test -z "$$(comm -23 $(VENDOR_ARCHIVE_GLOBALS) $(RECOVERED_ARCHIVE_GLOBALS))" || \
		{ echo "ERROR: zero-copy archive lost vendor global symbols"; \
		  comm -23 $(VENDOR_ARCHIVE_GLOBALS) $(RECOVERED_ARCHIVE_GLOBALS); exit 1; }
	@test "$$(comm -13 $(VENDOR_ARCHIVE_GLOBALS) $(RECOVERED_ARCHIVE_GLOBALS) | tr '\n' ' ')" = \
		"wlan_rx_dma_deinit wlan_rx_dma_get_stats wlan_rx_dma_invalidate_completed wlan_rx_dma_rotate " || \
		{ echo "ERROR: unexpected zero-copy archive global-symbol additions"; \
		  comm -13 $(VENDOR_ARCHIVE_GLOBALS) $(RECOVERED_ARCHIVE_GLOBALS); exit 1; }
	@$(NM) -g -u -P $(ORIGINAL_ARCHIVE) 2>/dev/null | \
		awk '$$2 == "U" {print $$1}' | sort -u > $(VENDOR_ARCHIVE_RAW_UNDEFINED)
	@$(NM) -g -u -P $(ZERO_COPY_ARCHIVE) 2>/dev/null | \
		awk '$$2 == "U" {print $$1}' | sort -u > $(RECOVERED_ARCHIVE_RAW_UNDEFINED)
	@comm -23 $(VENDOR_ARCHIVE_RAW_UNDEFINED) $(VENDOR_ARCHIVE_GLOBALS) > \
		$(VENDOR_ARCHIVE_EXTERNALS)
	@comm -23 $(RECOVERED_ARCHIVE_RAW_UNDEFINED) $(RECOVERED_ARCHIVE_GLOBALS) > \
		$(RECOVERED_ARCHIVE_EXTERNALS)
	@cmp -s $(VENDOR_ARCHIVE_EXTERNALS) $(RECOVERED_ARCHIVE_EXTERNALS) || \
		{ echo "ERROR: zero-copy archive external dependency ABI differs from vendor"; \
		  diff -u $(VENDOR_ARCHIVE_EXTERNALS) $(RECOVERED_ARCHIVE_EXTERNALS); exit 1; }
	@$(NM) -A $(RECOVERED_RX_OBJECT) | \
		grep -q ' U wlan_rx_dma_rotate$$' || \
		{ echo "ERROR: recovered RX object does not use DMA rotation"; exit 1; }
	@$(NM) -A $(DMA_MANAGER_OBJECT) | \
		grep -q ' T wlan_rx_dma_rotate$$' || \
		{ echo "ERROR: DMA manager missing wlan_rx_dma_rotate"; exit 1; }
	@$(NM) -A $(RECOVERED_SKBUFF_OBJECT) | \
		grep -q ' T skb_clone$$' || \
		{ echo "ERROR: recovered skbuff object missing skb_clone"; exit 1; }
	@$(NM) -A $(RECOVERED_SKBUFF_OBJECT) | \
		grep -q ' T kfree_skb$$' || \
		{ echo "ERROR: recovered skbuff object missing kfree_skb"; exit 1; }
	@if $(NM) -A $(ZERO_COPY_ARCHIVE) 2>/dev/null | \
		grep -q ' U rtw_rx_.*\(memcpy\|kfree\)'; then \
		echo "ERROR: legacy RX symbol hook remains"; exit 1; \
	fi
	@test "$$($(AR) t $(ZERO_COPY_ARCHIVE) | grep -c '^rtl8195b_recv.o$$')" = 1 || \
		{ echo "ERROR: duplicate rtl8195b_recv.o members"; exit 1; }
	@test "$$($(AR) t $(ZERO_COPY_ARCHIVE) | grep -c '^freertos_skbuff.o$$')" = 1 || \
		{ echo "ERROR: duplicate freertos_skbuff.o members"; exit 1; }
	@test "$$($(AR) t $(ZERO_COPY_ARCHIVE) | grep -c '^wlan_rx_dma_mgr.o$$')" = 1 || \
		{ echo "ERROR: DMA manager member missing or duplicated"; exit 1; }
	@test "$$($(AR) p $(ZERO_COPY_ARCHIVE) rtw_recv.o | sha256sum | cut -d' ' -f1)" = \
		"$$($(AR) p $(ORIGINAL_ARCHIVE) rtw_recv.o | sha256sum | cut -d' ' -f1)" || \
		{ echo "ERROR: vendor rtw_recv.o changed"; exit 1; }
	@set -e; for member in $$($(AR) t $(ORIGINAL_ARCHIVE)); do \
		case "$$member" in rtl8195b_recv.o|freertos_skbuff.o) continue ;; esac; \
		original_hash=$$($(AR) p $(ORIGINAL_ARCHIVE) "$$member" | sha256sum | cut -d' ' -f1); \
		rebuilt_hash=$$($(AR) p $(ZERO_COPY_ARCHIVE) "$$member" | sha256sum | cut -d' ' -f1); \
		test "$$original_hash" = "$$rebuilt_hash" || \
			{ echo "ERROR: vendor member $$member changed"; exit 1; }; \
	done
	@echo "  WLAN  verified source-recovered zero-copy archive"

clean:
	@rm -f $(ZERO_COPY_ARCHIVE) $(DROPIN_ARCHIVE) $(RECOVERED_RX_OBJECT) \
		$(BUILD_DIR)/lib_wlan_zero_copy_minimal.a \
		$(BUILD_DIR)/zero_copy_minimal/freertos_skbuff.o \
		$(COPY_PARITY_ARCHIVE) $(SKBUFF_PARITY_ARCHIVE) \
		$(COPY_PARITY_RX_OBJECT) \
		$(RECOVERED_SKBUFF_OBJECT) $(VENDOR_PARITY_SKBUFF_OBJECT) \
		$(DMA_MANAGER_OBJECT) \
		$(VENDOR_RX_OBJECT) $(VENDOR_SKBUFF_OBJECT) \
		$(VENDOR_RX_GLOBALS) $(RECOVERED_RX_GLOBALS) \
		$(VENDOR_SKBUFF_GLOBALS) $(RECOVERED_SKBUFF_GLOBALS) \
		$(VENDOR_RX_DATA_ABI) $(RECOVERED_RX_DATA_ABI) \
		$(VENDOR_SKBUFF_DATA_ABI) $(RECOVERED_SKBUFF_DATA_ABI) \
		$(VENDOR_ARCHIVE_GLOBALS) $(RECOVERED_ARCHIVE_GLOBALS) \
		$(VENDOR_ARCHIVE_RAW_UNDEFINED) $(RECOVERED_ARCHIVE_RAW_UNDEFINED) \
		$(VENDOR_ARCHIVE_EXTERNALS) $(RECOVERED_ARCHIVE_EXTERNALS)
