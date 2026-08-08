/*
 * RTL8195B WLAN RX fixed-pool buffer-swap reference implementation.
 *
 * This is an integration skeleton.  rtw_port_* hooks must be replaced by
 * Realtek's private descriptor, skb, cache, locking and RX rearm primitives.
 * No DMA backing buffer is allocated or freed in this hot path.
 */

#include "rtw_rx_zc_reference.h"

#include <stddef.h>
#include <stdint.h>

#define RTW_RX_BUFFER_CAPACITY 2112U

struct rtw_wlan_private {
	void *adapter;
	rtw_rx_buffer_swap_fn rx_buffer_swap;
	struct rtw_rx_swap_stats stats;
};

/* Realtek-private integration hooks. */
extern struct sk_buff *rtw_port_dev_alloc_skb(uint32_t capacity,
					       uint32_t headroom);
extern struct rtw_rx_buffer *rtw_port_desc_backing(struct rtw_rx_desc *desc);
extern struct rtw_rx_buffer *rtw_port_skb_backing(struct sk_buff *skb);
extern int rtw_port_buffer_dma_compatible(const struct rtw_rx_buffer *buffer);
extern int rtw_port_frame_swap_eligible(struct rtw_rx_desc *desc,
					uint32_t offset, uint32_t length);
extern void rtw_port_rx_complete_for_cpu(uint8_t *base, uint32_t capacity);
extern void rtw_port_rx_prepare_for_device(uint8_t *base, uint32_t capacity);
extern void rtw_port_rx_lock(void *adapter);
extern void rtw_port_rx_unlock(void *adapter);
extern void rtw_port_desc_set_backing(struct rtw_rx_desc *desc,
				      struct rtw_rx_buffer *buffer);
extern void rtw_port_skb_set_backing(struct sk_buff *skb,
				     struct rtw_rx_buffer *buffer,
				     uint32_t offset, uint32_t length);
extern void rtw_port_dma_publish_barrier(void);
extern void rtw_port_rx_rearm_descriptor(struct rtw_rx_desc *desc);
extern void rtw_port_rx_rearm_same_buffer(struct rtw_rx_desc *desc);
extern void rtw_port_skb_put(struct sk_buff *skb, uint32_t length);
extern void rtw_memcpy(void *dst, const void *src, uint32_t length);
extern uint8_t *rtw_port_desc_frame_data(struct rtw_rx_desc *desc,
					 uint32_t offset);
extern uint8_t *rtw_port_skb_data(struct sk_buff *skb);
extern int rtw_port_existing_rx_processing(void *adapter,
					   struct sk_buff *skb);
extern int rtw_port_fixed_pool_is_swap_compatible(void *adapter);

/*
 * Success means the swap and descriptor rearm are complete.
 * Failure means no descriptor, skb or ownership state has changed, so the
 * caller can safely execute the exact legacy memcpy path.
 */
static int rtw_rx_fixed_pool_swap(void *adapter,
				  struct rtw_rx_desc *desc,
				  struct sk_buff *skb,
				  uint32_t offset,
				  uint32_t length)
{
	struct rtw_rx_buffer *received = rtw_port_desc_backing(desc);
	struct rtw_rx_buffer *replacement = rtw_port_skb_backing(skb);

	/* Complete every operation that can reject before changing ownership. */
	if (received == NULL || replacement == NULL ||
	    received->owner != RTW_RX_BUFFER_RING ||
	    replacement->owner != RTW_RX_BUFFER_STACK ||
	    offset > received->capacity ||
	    length > received->capacity - offset ||
	    replacement->capacity < received->capacity ||
	    !rtw_port_buffer_dma_compatible(replacement) ||
	    !rtw_port_frame_swap_eligible(desc, offset, length)) {
		return -1;
	}

	rtw_port_rx_complete_for_cpu(received->base, received->capacity);
	rtw_port_rx_prepare_for_device(replacement->base,
				       replacement->capacity);

	rtw_port_rx_lock(adapter);
	/* The real driver should assert these states again while holding its lock. */
	if (received->owner != RTW_RX_BUFFER_RING ||
	    replacement->owner != RTW_RX_BUFFER_STACK) {
		rtw_port_rx_unlock(adapter);
		return -1;
	}

	rtw_port_desc_set_backing(desc, replacement);
	rtw_port_skb_set_backing(skb, received, offset, length);
	replacement->owner = RTW_RX_BUFFER_RING;
	received->owner = RTW_RX_BUFFER_STACK;
	rtw_port_dma_publish_barrier();
	rtw_port_rx_rearm_descriptor(desc);
	rtw_port_rx_unlock(adapter);
	return 0;
}

/* Reference shape for the original rtl8195b_recv_tasklet() copy site. */
static int rtw_rx_receive_one(struct rtw_wlan_private *priv,
			      struct rtw_rx_desc *desc,
			      uint32_t offset,
			      uint32_t length,
			      uint32_t headroom)
{
	struct sk_buff *skb;

	skb = rtw_port_dev_alloc_skb(RTW_RX_BUFFER_CAPACITY, headroom);
	if (skb == NULL)
		return -1;

	if (priv->rx_buffer_swap != NULL) {
		if (priv->rx_buffer_swap(priv->adapter, desc, skb,
					 offset, length) == 0) {
			priv->stats.swap_packets++;
			priv->stats.swap_bytes += length;
			return rtw_port_existing_rx_processing(priv->adapter, skb);
		}
		priv->stats.callback_reject++;
	} else {
		priv->stats.callback_null++;
	}

	/* Unmodified legacy behavior. */
	rtw_memcpy(rtw_port_skb_data(skb),
		   rtw_port_desc_frame_data(desc, offset), length);
	rtw_port_skb_put(skb, length);
	rtw_port_rx_rearm_same_buffer(desc);
	priv->stats.legacy_copy_packets++;
	priv->stats.legacy_copy_bytes += length;
	return rtw_port_existing_rx_processing(priv->adapter, skb);
}

static void rtw_rx_swap_init(struct rtw_wlan_private *priv)
{
	/* NULL is both the safe default and the runtime legacy flag. */
#if CONFIG_RTW_RX_BUFFER_SWAP
	if (rtw_port_fixed_pool_is_swap_compatible(priv->adapter))
		priv->rx_buffer_swap = rtw_rx_fixed_pool_swap;
	else
		priv->rx_buffer_swap = NULL;
#else
	priv->rx_buffer_swap = NULL;
#endif
}

/* Keep the reference entry points visible to source reviewers. */
static void rtw_rx_swap_reference_keep(void)
{
	(void)rtw_rx_receive_one;
	(void)rtw_rx_swap_init;
}
