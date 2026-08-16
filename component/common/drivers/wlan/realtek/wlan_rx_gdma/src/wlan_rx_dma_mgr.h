#ifndef WLAN_RX_DMA_MGR_H
#define WLAN_RX_DMA_MGR_H

#include <basic_types.h>
#include <skbuff.h>

enum wlan_rx_dma_rotate_result {
	WLAN_RX_DMA_ROTATE_COPY = 0,
	WLAN_RX_DMA_ROTATE_ZERO_COPY = 1,
	WLAN_RX_DMA_ROTATE_INVALID = -1
};

/*
 * Rotate one completed RX descriptor onto a fresh, standard skb-backed DMA
 * buffer.  The caller still owns the descriptor and must not set OWN until
 * this function returns.
 *
 * COPY means the current descriptor buffer remains caller-owned and must be
 * copied through the vendor fallback path.  ZERO_COPY returns an skb whose
 * data/tail/len describe the completed packet; ownership of that skb has left
 * the DMA manager and transfers to the normal vendor RX pipeline.
 */
int wlan_rx_dma_rotate(void *adapter, unsigned int slot_index,
		       volatile u32 *descriptor, volatile u32 *shadow_entry,
		       unsigned int ring_size, unsigned int packet_offset,
		       unsigned int packet_len, struct sk_buff **packet_skb);

/* Synchronize a device-written completed buffer before descriptor parsing. */
void wlan_rx_dma_invalidate_completed(void *dma_base, unsigned int bytes);

/* Hardware must be stopped and retained lwIP pbufs drained before this call. */
void wlan_rx_dma_deinit(void *adapter);

struct wlan_rx_dma_stats {
	u32 adopted;
	u32 zero_copy;
	u32 fallback_alloc;
	u32 fallback_layout;
	u32 released;
	u32 live_slots;
};

int wlan_rx_dma_get_stats(void *adapter, struct wlan_rx_dma_stats *stats);

#endif
