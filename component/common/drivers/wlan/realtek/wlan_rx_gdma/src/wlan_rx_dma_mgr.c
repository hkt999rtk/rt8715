/*
 * Recovered RTL8195B RX DMA-buffer ownership layer.
 *
 * This deliberately uses ordinary vendor sk_buffs as descriptor buffers.
 * Consequently every existing rtw_recv.o success, discard, and error path can
 * keep using kfree_skb() without a redirected free symbol.  lwIP zero-copy
 * keeps the same backing allocation alive with skb_clone().
 */

#include <basic_types.h>
#include <cmsis_gcc.h>
#include <osdep_service.h>
#include <skbuff.h>

#include "wlan_rx_dma_mgr.h"

#define WLAN_RX_DMA_CONTEXTS       2U
#define WLAN_RX_DMA_MAX_SLOTS     32U
#define WLAN_RX_DMA_ALIGNMENT     32U
#define WLAN_RX_DMA_SHARED_FLAG    2
/* freertos_skbuff.o stores its shared-data reference count at head+1668.
 * Keep that vendor-private word outside the DMA-writeable range. */
#define WLAN_RX_DMA_SKB_PREFIX   1672U

struct wlan_rx_dma_slot {
	struct sk_buff *skb;
	u8 *dma_base;
	u8 *original_base;
	volatile u32 *descriptor;
	volatile u32 *shadow_entry;
};

struct wlan_rx_dma_context {
	void *adapter;
	struct wlan_rx_dma_slot slots[WLAN_RX_DMA_MAX_SLOTS];
	struct wlan_rx_dma_stats stats;
	u32 next_report;
};

static struct wlan_rx_dma_context g_wlan_rx_dma[WLAN_RX_DMA_CONTEXTS];

/* Minimal ABI view of the ROM cache-stub table; avoids pulling all of cmsis.h. */
struct wlan_rx_cache_stubs {
	void (*icache_enable)(void);
	void (*icache_disable)(void);
	void (*icache_invalidate)(void);
	void (*dcache_enable)(void);
	void (*dcache_disable)(void);
	void (*dcache_invalidate)(void);
	void (*dcache_clean)(void);
	void (*dcache_clean_invalidate)(void);
	void (*dcache_invalidate_by_addr)(u32 *addr, int32_t size);
	void (*dcache_clean_by_addr)(u32 *addr, int32_t size);
	void (*dcache_clean_invalidate_by_addr)(u32 *addr, int32_t size);
	u32 reserved[4];
};
extern const struct wlan_rx_cache_stubs hal_cache_stubs;
extern int printf(const char *format, ...);

static u8 *wlan_rx_dma_align(u8 *address)
{
	uintptr_t value = (uintptr_t)address;

	value = (value + WLAN_RX_DMA_ALIGNMENT - 1U) &
		~(uintptr_t)(WLAN_RX_DMA_ALIGNMENT - 1U);
	return (u8 *)value;
}

static struct wlan_rx_dma_context *
wlan_rx_dma_find_context_locked(void *adapter, int create)
{
	struct wlan_rx_dma_context *empty = NULL;
	unsigned int i;

	for (i = 0; i < WLAN_RX_DMA_CONTEXTS; i++) {
		if (g_wlan_rx_dma[i].adapter == adapter) {
			return &g_wlan_rx_dma[i];
		}
		if (empty == NULL && g_wlan_rx_dma[i].adapter == NULL) {
			empty = &g_wlan_rx_dma[i];
		}
	}
	if (create && empty != NULL) {
		empty->adapter = adapter;
		empty->stats.adopted = 0U;
		empty->stats.zero_copy = 0U;
		empty->stats.fallback_alloc = 0U;
		empty->stats.fallback_layout = 0U;
		empty->stats.released = 0U;
		empty->stats.live_slots = 0U;
		empty->next_report = 1024U;
	}
	return create ? empty : NULL;
}

static struct sk_buff *
wlan_rx_dma_alloc_skb(unsigned int ring_size, u8 **dma_base)
{
	struct sk_buff *skb;
	u8 *base;
	u32 allocation_size;

	/* Extra bytes protect the embedded vendor refcount and permit alignment. */
	if (ring_size > 0xffffffffU - WLAN_RX_DMA_SKB_PREFIX -
			WLAN_RX_DMA_ALIGNMENT + 1U) {
		return NULL;
	}
	allocation_size = ring_size + WLAN_RX_DMA_SKB_PREFIX +
		WLAN_RX_DMA_ALIGNMENT - 1U;
	skb = dev_alloc_skb(allocation_size, 0U);
	if (skb == NULL || skb->data == NULL || skb->end == NULL) {
		if (skb != NULL) {
			kfree_skb(skb);
		}
		return NULL;
	}
	base = wlan_rx_dma_align(skb->head + WLAN_RX_DMA_SKB_PREFIX);
	if (base < skb->head || base > skb->end ||
	    ring_size > (unsigned int)(skb->end - base)) {
		kfree_skb(skb);
		return NULL;
	}

	/* Manager-owned descriptor skbs are empty until ownership transfers. */
	skb->data = base;
	skb->tail = base;
	skb->len = 0U;
	/* Ordinary dynamic skbs retain vendor flag 1 semantics.  Only descriptor
	 * storage is refcounted across skb_clone()/kfree_skb(). */
	skb->dyalloc_flag = WLAN_RX_DMA_SHARED_FLAG;
	*dma_base = base;
	return skb;
}

static void wlan_rx_dma_publish(volatile u32 *descriptor,
				volatile u32 *shadow_entry, u8 *dma_base,
				unsigned int ring_size)
{
	/* Remove dirty CPU lines before the device becomes the writer. */
	hal_cache_stubs.dcache_clean_invalidate_by_addr((u32 *)dma_base,
						   (int32_t)ring_size);
	*shadow_entry = (u32)(uintptr_t)dma_base;
	descriptor[1] = (u32)(uintptr_t)dma_base;
	hal_cache_stubs.dcache_clean_by_addr(
		(u32 *)((uintptr_t)descriptor &
			~(uintptr_t)(WLAN_RX_DMA_ALIGNMENT - 1U)),
		(int32_t)WLAN_RX_DMA_ALIGNMENT);
	__DSB();
}

void wlan_rx_dma_invalidate_completed(void *dma_base, unsigned int bytes)
{
	if (dma_base == NULL || bytes == 0U) {
		return;
	}
	hal_cache_stubs.dcache_invalidate_by_addr((u32 *)dma_base,
						     (int32_t)bytes);
	__DSB();
}

int wlan_rx_dma_rotate(void *adapter, unsigned int slot_index,
		       volatile u32 *descriptor, volatile u32 *shadow_entry,
		       unsigned int ring_size, unsigned int packet_offset,
		       unsigned int packet_len, struct sk_buff **packet_skb)
{
	struct wlan_rx_dma_context *context;
	struct wlan_rx_dma_slot *slot;
	struct sk_buff *replacement;
	struct sk_buff *completed;
	u8 *replacement_base;
	u8 *completed_base;
	u8 *observed_base;
	int result;
	int first_adoption = 0;
	int report = 0;
	struct wlan_rx_dma_stats snapshot;

	if (packet_skb == NULL) {
		return WLAN_RX_DMA_ROTATE_INVALID;
	}
	*packet_skb = NULL;
	if (adapter == NULL || descriptor == NULL || shadow_entry == NULL ||
	    slot_index >= WLAN_RX_DMA_MAX_SLOTS || ring_size == 0U ||
	    packet_offset > ring_size || packet_len > ring_size - packet_offset) {
		return WLAN_RX_DMA_ROTATE_INVALID;
	}

	replacement = wlan_rx_dma_alloc_skb(ring_size, &replacement_base);
	if (replacement == NULL) {
		save_and_cli();
		context = wlan_rx_dma_find_context_locked(adapter, 0);
		if (context != NULL) {
			context->stats.fallback_alloc++;
		}
		restore_flags();
		return WLAN_RX_DMA_ROTATE_COPY;
	}

	observed_base = (u8 *)(uintptr_t)descriptor[1];
	if (observed_base == NULL || observed_base !=
	    (u8 *)(uintptr_t)*shadow_entry) {
		kfree_skb(replacement);
		save_and_cli();
		context = wlan_rx_dma_find_context_locked(adapter, 1);
		if (context != NULL) {
			context->stats.fallback_layout++;
		}
		restore_flags();
		return WLAN_RX_DMA_ROTATE_COPY;
	}

	save_and_cli();
	context = wlan_rx_dma_find_context_locked(adapter, 1);
	if (context == NULL) {
		restore_flags();
		kfree_skb(replacement);
		return WLAN_RX_DMA_ROTATE_COPY;
	}
	slot = &context->slots[slot_index];
	if (slot->skb != NULL && (slot->dma_base != observed_base ||
	    slot->descriptor != descriptor || slot->shadow_entry != shadow_entry)) {
		context->stats.fallback_layout++;
		restore_flags();
		kfree_skb(replacement);
		return WLAN_RX_DMA_ROTATE_COPY;
	}

	completed = slot->skb;
	completed_base = slot->dma_base;
	if (completed == NULL) {
		slot->original_base = observed_base;
		context->stats.adopted++;
		context->stats.live_slots++;
		first_adoption = context->stats.adopted == 1U;
	}
	slot->skb = replacement;
	slot->dma_base = replacement_base;
	slot->descriptor = descriptor;
	slot->shadow_entry = shadow_entry;
	if (completed != NULL) {
		context->stats.zero_copy++;
		if (context->stats.zero_copy >= context->next_report) {
			context->next_report += 1024U;
			snapshot = context->stats;
			report = 1;
		}
	}
	restore_flags();

	wlan_rx_dma_publish(descriptor, shadow_entry, replacement_base,
			    ring_size);
	if (first_adoption) {
		printf("[WLAN_RX_DMA] active adapter=%p ring=%u first=%p replacement=%p\n",
		       adapter, ring_size, observed_base, replacement_base);
	}
	if (report) {
		printf("[WLAN_RX_DMA] zero_copy=%u adopted=%u live=%u fallback=%u/%u\n",
		       (unsigned int)snapshot.zero_copy,
		       (unsigned int)snapshot.adopted,
		       (unsigned int)snapshot.live_slots,
		       (unsigned int)snapshot.fallback_alloc,
		       (unsigned int)snapshot.fallback_layout);
	}

	if (completed == NULL) {
		/* First visit adopts this slot; its vendor raw buffer is copied once. */
		return WLAN_RX_DMA_ROTATE_COPY;
	}

	completed->data = completed_base + packet_offset;
	/* Match the vendor RX convention: recv_frame_put(), not skb_put(),
	 * advances the logical packet tail after this handoff. */
	completed->tail = completed->data;
	completed->len = packet_len;
	*packet_skb = completed;
	result = WLAN_RX_DMA_ROTATE_ZERO_COPY;
	return result;
}

void wlan_rx_dma_deinit(void *adapter)
{
	struct wlan_rx_dma_context *context;
	struct sk_buff *release[WLAN_RX_DMA_MAX_SLOTS];
	unsigned int release_count = 0U;
	unsigned int i;
	struct wlan_rx_dma_stats snapshot;

	if (adapter == NULL) {
		return;
	}

	/* Caller guarantees RX DMA is stopped, so descriptors cannot race us. */
	save_and_cli();
	context = wlan_rx_dma_find_context_locked(adapter, 0);
	if (context == NULL) {
		restore_flags();
		return;
	}
	for (i = 0; i < WLAN_RX_DMA_MAX_SLOTS; i++) {
		struct wlan_rx_dma_slot *slot = &context->slots[i];

		if (slot->skb == NULL) {
			continue;
		}
		if (slot->descriptor != NULL && slot->shadow_entry != NULL &&
		    slot->original_base != NULL) {
			*slot->shadow_entry = (u32)(uintptr_t)slot->original_base;
			slot->descriptor[1] = (u32)(uintptr_t)slot->original_base;
			hal_cache_stubs.dcache_clean_by_addr(
				(u32 *)((uintptr_t)slot->descriptor &
					~(uintptr_t)(WLAN_RX_DMA_ALIGNMENT - 1U)),
				(int32_t)WLAN_RX_DMA_ALIGNMENT);
		}
		release[release_count++] = slot->skb;
		context->stats.released++;
	}
	__DSB();
	snapshot = context->stats;
	context->adapter = NULL;
	context->stats.live_slots = 0U;
	for (i = 0; i < WLAN_RX_DMA_MAX_SLOTS; i++) {
		struct wlan_rx_dma_slot *slot = &context->slots[i];

		slot->skb = NULL;
		slot->dma_base = NULL;
		slot->original_base = NULL;
		slot->descriptor = NULL;
		slot->shadow_entry = NULL;
	}
	restore_flags();

	for (i = 0; i < release_count; i++) {
		kfree_skb(release[i]);
	}
	printf("[WLAN_RX_DMA] deinit zero_copy=%u adopted=%u released=%u fallback=%u/%u\n",
	       (unsigned int)snapshot.zero_copy,
	       (unsigned int)snapshot.adopted,
	       (unsigned int)snapshot.released,
	       (unsigned int)snapshot.fallback_alloc,
	       (unsigned int)snapshot.fallback_layout);
}

int wlan_rx_dma_get_stats(void *adapter, struct wlan_rx_dma_stats *stats)
{
	struct wlan_rx_dma_context *context;

	if (adapter == NULL || stats == NULL) {
		return -1;
	}
	save_and_cli();
	context = wlan_rx_dma_find_context_locked(adapter, 0);
	if (context == NULL) {
		restore_flags();
		return -1;
	}
	*stats = context->stats;
	restore_flags();
	return 0;
}
