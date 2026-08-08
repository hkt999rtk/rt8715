CROSS_COMPILE ?= arm-none-eabi-
AR := $(CROSS_COMPILE)ar
NM := $(CROSS_COMPILE)nm
OBJCOPY := $(CROSS_COMPILE)objcopy

BUILD_DIR := build
PATCH_MEMBER := rtl8195b_recv.o
FREE_PATCH_MEMBER := rtw_recv.o
PATCHED_ARCHIVE := $(BUILD_DIR)/lib_wlan_rx_gdma.a
PATCHED_MEMBER := $(BUILD_DIR)/$(PATCH_MEMBER)
FREE_PATCHED_MEMBER := $(BUILD_DIR)/$(FREE_PATCH_MEMBER)

.NOTPARALLEL:
.PHONY: all clean verify

all: check-input verify

.PHONY: check-input
check-input:
	@test -n "$(ORIGINAL_ARCHIVE)" || \
		{ echo "ERROR: ORIGINAL_ARCHIVE is required"; exit 1; }

# Keep the vendor SDK archive immutable.  rtl8195b_recv.o redirects the two
# closed RX memcpy calls.  rtw_recv.o redirects skb destruction so packets
# consumed by non-lwIP receive branches restore a temporarily rebound skb
# before the vendor free path runs.  Both replacement symbols are supplied by
# lwip_intf.c in the application build.
$(PATCHED_ARCHIVE): $(ORIGINAL_ARCHIVE) Makefile
	@mkdir -p $(BUILD_DIR)
	@cp $(ORIGINAL_ARCHIVE) $@
	@cd $(BUILD_DIR) && $(AR) x $(abspath $(ORIGINAL_ARCHIVE)) \
		$(PATCH_MEMBER) $(FREE_PATCH_MEMBER)
	@$(OBJCOPY) --redefine-sym rtw_memcpy=rtw_rx_ring_memcpy $(PATCHED_MEMBER)
	@$(OBJCOPY) --redefine-sym kfree_skb_chk_key=rtw_rx_swap_kfree_skb_chk_key \
		$(FREE_PATCHED_MEMBER)
	@$(AR) rcs $@ $(PATCHED_MEMBER) $(FREE_PATCHED_MEMBER)
	@echo "  WLAN  patched $(PATCH_MEMBER), $(FREE_PATCH_MEMBER) -> $@"

verify: $(PATCHED_ARCHIVE)
	@$(NM) -A $(PATCHED_MEMBER) | grep -q ' U rtw_rx_ring_memcpy$$' || \
		{ echo "ERROR: $(PATCH_MEMBER) does not reference rtw_rx_ring_memcpy"; exit 1; }
	@if $(NM) -A $(PATCHED_MEMBER) | grep -q ' U rtw_memcpy$$'; then \
		echo "ERROR: unredirected rtw_memcpy remains in $(PATCH_MEMBER)"; exit 1; \
	fi
	@$(NM) -A $(FREE_PATCHED_MEMBER) | \
		grep -q ' U rtw_rx_swap_kfree_skb_chk_key$$' || \
		{ echo "ERROR: $(FREE_PATCH_MEMBER) does not reference swap-aware free"; exit 1; }
	@if $(NM) -A $(FREE_PATCHED_MEMBER) | grep -q ' U kfree_skb_chk_key$$'; then \
		echo "ERROR: unredirected kfree remains in $(FREE_PATCH_MEMBER)"; exit 1; \
	fi

clean:
	@rm -f $(PATCHED_ARCHIVE) $(PATCHED_MEMBER) $(FREE_PATCHED_MEMBER)
