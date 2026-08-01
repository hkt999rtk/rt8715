CROSS_COMPILE ?= arm-none-eabi-
AR := $(CROSS_COMPILE)ar
NM := $(CROSS_COMPILE)nm
OBJCOPY := $(CROSS_COMPILE)objcopy

BUILD_DIR := build
PATCH_MEMBER := rtl8195b_recv.o
PATCHED_ARCHIVE := $(BUILD_DIR)/lib_wlan_rx_gdma.a
PATCHED_MEMBER := $(BUILD_DIR)/$(PATCH_MEMBER)

.NOTPARALLEL:
.PHONY: all clean verify

all: check-input verify

.PHONY: check-input
check-input:
	@test -n "$(ORIGINAL_ARCHIVE)" || \
		{ echo "ERROR: ORIGINAL_ARCHIVE is required"; exit 1; }

# Keep the vendor SDK archive immutable.  Only rtl8195b_recv.o is extracted,
# and only its undefined rtw_memcpy reference is renamed.  The replacement
# symbol is supplied by lwip_intf.c in the application build.
$(PATCHED_ARCHIVE): $(ORIGINAL_ARCHIVE) Makefile
	@mkdir -p $(BUILD_DIR)
	@cp $(ORIGINAL_ARCHIVE) $@
	@cd $(BUILD_DIR) && $(AR) x $(abspath $(ORIGINAL_ARCHIVE)) $(PATCH_MEMBER)
	@$(OBJCOPY) --redefine-sym rtw_memcpy=rtw_rx_ring_memcpy $(PATCHED_MEMBER)
	@$(AR) rcs $@ $(PATCHED_MEMBER)
	@echo "  WLAN  patched $(PATCH_MEMBER) -> $@"

verify: $(PATCHED_ARCHIVE)
	@$(NM) -A $(PATCHED_MEMBER) | grep -q ' U rtw_rx_ring_memcpy$$' || \
		{ echo "ERROR: $(PATCH_MEMBER) does not reference rtw_rx_ring_memcpy"; exit 1; }
	@if $(NM) -A $(PATCHED_MEMBER) | grep -q ' U rtw_memcpy$$'; then \
		echo "ERROR: unredirected rtw_memcpy remains in $(PATCH_MEMBER)"; exit 1; \
	fi

clean:
	@rm -f $(PATCHED_ARCHIVE) $(PATCHED_MEMBER)
