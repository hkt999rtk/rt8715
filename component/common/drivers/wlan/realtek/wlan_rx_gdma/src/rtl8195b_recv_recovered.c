/*
 * Functional C recovery of the RTL8195B AXI RX-ring object.
 *
 * Only this object is replaced.  The remaining vendor WLAN objects, including
 * rtw_recv.o, stay unchanged.  Completed descriptors are rotated onto fresh
 * ordinary sk_buffs so every original free/drop/lwIP ownership path remains
 * valid without symbol hooks.
 */

#include <basic_types.h>
#include <skbuff.h>

#include "wlan_rx_dma_mgr.h"

#ifndef CONFIG_WLAN_RX_DMA_FORCE_COPY
#define CONFIG_WLAN_RX_DMA_FORCE_COPY 0
#endif

#if CONFIG_WLAN_RX_DMA_FORCE_COPY
typedef struct recovered_hal_cache_stubs {
	void (*whole_cache_ops[8])(void);
	void (*dcache_invalidate_by_addr)(u32 *address, int bytes);
} recovered_hal_cache_stubs_t;
extern const recovered_hal_cache_stubs_t hal_cache_stubs;
#endif

#define ADAPTER_DVOBJ                 0x0008U
#define ADAPTER_BH_LOCK               0x000cU
#define ADAPTER_FREE_RECV_QUEUE       0x0c44U
#define ADAPTER_RECVFRAME_FAIL        0x0ca0U
#define ADAPTER_SKB_FAIL              0x0ca2U
#define ADAPTER_RECVBUF_RAW           0x0cb8U
#define ADAPTER_RECVBUF_ALIGNED       0x0cbcU
#define ADAPTER_RECVBUF_QUEUE         0x0cc0U
#define ADAPTER_RECVBUF_COUNT         0x0cccU
#define ADAPTER_RX_DESC_BASE          0x0cd0U
#define ADAPTER_RX_RING_INDEX         0x0cd8U
#define ADAPTER_RX_SHADOW_BASE        0x0cdcU
#define ADAPTER_RX_RING_COUNT         0x0d3cU
#define ADAPTER_RX_RING_SIZE          0x0d40U
#define ADAPTER_LINK_STATE            0x0e5cU
#define ADAPTER_HAL_DATA              0x31b8U
#define ADAPTER_NETDEV                0x3350U
#define ADAPTER_BUDDY                 0x3378U

#define HAL_RX_OVERFLOW_B8            0x3c44U
#define HAL_RX_OVERFLOW_3EC           0x3c48U

#define RECVFRAME_PACKET              0x0008U
#define RECVFRAME_ADAPTER             0x0010U
#define RECVFRAME_ATTR                0x001cU
#define RECVFRAME_PKT_LEN             0x001cU
#define RECVFRAME_PHY_STATUS          0x001eU
#define RECVFRAME_DRVINFO_SIZE        0x001fU
#define RECVFRAME_SHIFT_SIZE          0x0020U
#define RECVFRAME_QOS                 0x0024U
#define RECVFRAME_SPECIAL             0x002aU
#define RECVFRAME_CRC_ERR             0x0032U
#define RECVFRAME_ICV_ERR             0x0033U
#define RECVFRAME_PACKET_TYPE         0x005cU
#define RECVFRAME_LEN                 0x00a0U
#define RECVFRAME_HEAD                0x00a4U
#define RECVFRAME_DATA                0x00a8U
#define RECVFRAME_TAIL                0x00acU
#define RECVFRAME_END                 0x00b0U
#define RECVFRAME_CLONE_LEN           0x00b4U
#define RECVFRAME_ATTR_SIZE           132U

#define REG_RX_RING_INDEX             0x03b4U
#define RX_DESC_LENGTH_MASK           0x00003fffU
#define RX_MAX_PACKET_LENGTH          1578U
#define RX_SPECIAL_LENGTH             1650U
#define RX_SPECIAL_BUFFER_SIZE        1664U
#define RX_DESCRIPTOR_OFFSET          24U

/* Preserve the vendor archive's section ABI.  The application linker places
 * rtl8195b_recv_tasklet in ITCM by this exact section name. */
#define WLAN_TEXT(symbol) \
	__attribute__((section(".wlan.text." #symbol)))

#define LOAD_U8(base, offset) \
	(*(volatile u8 *)((u8 *)(base) + (offset)))
#define LOAD_U16(base, offset) \
	(*(volatile u16 *)((u8 *)(base) + (offset)))
#define LOAD_U32(base, offset) \
	(*(volatile u32 *)((u8 *)(base) + (offset)))
#define LOAD_PTR(base, offset) \
	(*(void **)((u8 *)(base) + (offset)))
#define STORE_U16(base, offset, value) \
	(*(volatile u16 *)((u8 *)(base) + (offset)) = (u16)(value))
#define STORE_U32(base, offset, value) \
	(*(volatile u32 *)((u8 *)(base) + (offset)) = (u32)(value))
#define STORE_PTR(base, offset, value) \
	(*(void **)((u8 *)(base) + (offset)) = (void *)(value))

extern u32 rtw_read32(void *adapter, u32 address);
extern int rtw_write16(void *adapter, u32 address, u16 value);
extern void rtw_write32(void *adapter, u32 address, u32 value);
extern void rtw_enter_critical(void *lock, unsigned long *flags);
extern void rtw_exit_critical(void *lock, unsigned long *flags);
extern void rtw_enter_critical_bh(void *lock, unsigned long *flags);
extern void rtw_exit_critical_bh(void *lock, unsigned long *flags);
extern void *rtw_alloc_recvframe(void *queue);
extern void rtw_free_recvframe(void *frame, void *queue);
extern void rtw_init_listhead(void *list);
extern void rtw_init_queue(void *queue);
extern void rtl8195b_query_rx_desc_status(void *frame, u8 *descriptor);
extern void rtl8195b_query_rx_phy_status(void *frame, u8 *phy_status);
extern int rtw_buddy_adapter_up(void *adapter);
extern int rtw_recv_entry(void *frame);
extern int rtw_memcmp(const void *left, const void *right, u32 bytes);
extern void *rtw_memcpy(void *destination, const void *source, u32 bytes);
extern void rtw_disassoc_cmd(void *adapter);
extern void rtw_indicate_disconnect(void *adapter);
extern void rtw_free_assoc_resources(void *adapter, int lock_scanned_queue);
extern void c2h_pre_handler_rtl8195b(void *adapter, u8 *buffer, u32 length);
extern void rtw_msleep_os(int milliseconds);
extern struct sk_buff *skb_copy(struct sk_buff *skb, int gfp_mask,
				       unsigned int length);
extern void *rtw_zmalloc(u32 bytes);
extern void rtw_mfree(void *memory, u32 bytes);
extern int rtw_os_recvbuf_resource_alloc(void *adapter, void *recvbuf);
extern void rtw_os_recvbuf_resource_free(void *adapter, void *recvbuf);
extern void rtw_list_insert_tail(void *list, void *queue);
extern void rtw_list_delete(void *list);
extern int printf(const char *format, ...);
extern u32 GlobalDebugEnable;

static void recvframe_bind_skb(void *frame, void *adapter,
			       struct sk_buff *skb, unsigned int packet_len)
{
	u8 *data = skb->data;
	u8 *end = skb_end_pointer(skb);
	u8 *tail = data + packet_len;

	skb->dev = LOAD_PTR(adapter, ADAPTER_NETDEV);
	skb->len = packet_len;
	STORE_PTR(frame, RECVFRAME_PACKET, skb);
	STORE_PTR(frame, RECVFRAME_HEAD, skb->head);
	STORE_PTR(frame, RECVFRAME_DATA, data);
	STORE_PTR(frame, RECVFRAME_TAIL, data);
	STORE_PTR(frame, RECVFRAME_END, end);
	if (tail <= end) {
		STORE_PTR(frame, RECVFRAME_TAIL, tail);
		STORE_U32(frame, RECVFRAME_LEN,
			  LOAD_U32(frame, RECVFRAME_LEN) + packet_len);
	}
}

static void recvframe_rearm_descriptor(void *adapter,
				       volatile u32 *descriptor)
{
	u32 ring_size = LOAD_U16(adapter, ADAPTER_RX_RING_SIZE) &
			RX_DESC_LENGTH_MASK;

	descriptor[0] = (descriptor[0] & ~RX_DESC_LENGTH_MASK) | ring_size;
}

static void recvframe_advance_ring(void *adapter)
{
	u32 count = LOAD_U32(adapter, ADAPTER_RX_RING_COUNT);
	u32 index = LOAD_U32(adapter, ADAPTER_RX_RING_INDEX) + 1U;

	if (count != 0U) {
		index %= count;
	} else {
		index = 0U;
	}
	STORE_U32(adapter, ADAPTER_RX_RING_INDEX, index);
	while (rtw_write16(adapter, REG_RX_RING_INDEX, (u16)index) == 0) {
		rtw_msleep_os(1);
	}
}

static void recvframe_release(void *frame, void *free_queue)
{
	rtw_free_recvframe(frame, free_queue);
}

static void recvframe_handle_icv_error(void *adapter)
{
	unsigned long flags = 0U;
	u32 state;
	void *lock = (u8 *)adapter + ADAPTER_BH_LOCK;

	rtw_enter_critical_bh(lock, &flags);
	state = LOAD_U32(adapter, ADAPTER_LINK_STATE) & ~4U;
	if (state == 1U) {
		rtw_disassoc_cmd(adapter);
		rtw_indicate_disconnect(adapter);
		rtw_free_assoc_resources(adapter, 1);
	}
	rtw_exit_critical_bh(lock, &flags);
}

static void recvframe_process_buddy(void *adapter, void *buddy, void *frame,
				    void *free_queue, u8 *completed_base,
				    u8 *phy_status)
{
	struct sk_buff *skb = (struct sk_buff *)LOAD_PTR(frame, RECVFRAME_PACKET);
	u8 packet_type = LOAD_U8(frame, RECVFRAME_PACKET_TYPE);
	u8 *data = (u8 *)LOAD_PTR(frame, RECVFRAME_DATA);
	u32 packet_len = LOAD_U16(frame, RECVFRAME_PKT_LEN);
	void *clone_frame;
	struct sk_buff *clone_skb;

	if (packet_type != 0U) {
		if (packet_type == 4U) {
			c2h_pre_handler_rtl8195b(adapter, completed_base,
				packet_len + RX_DESCRIPTOR_OFFSET);
		} else if (packet_type == 2U) {
			printf("rx TX RPT\n\r");
		}
		recvframe_release(frame, free_queue);
		return;
	}

	/*
	 * The vendor dual-interface path duplicates multicast/broadcast packets:
	 * the clone is delivered to the buddy and the original remains on this
	 * adapter.  rtw_memcmp returns true on equality, so matching unicast is
	 * redirected to the buddy interface.
	 */
	if ((data[4] & 1U) == 0U) {
		if (rtw_memcmp(data + 4U, (u8 *)buddy + 0x2fa9U, 6U) != 0) {
			skb->dev = LOAD_PTR(buddy, ADAPTER_NETDEV);
			STORE_PTR(frame, RECVFRAME_ADAPTER, buddy);
		}
		goto original;
	}

	clone_frame = rtw_alloc_recvframe(free_queue);
	if (clone_frame == NULL) {
		recvframe_release(frame, free_queue);
		return;
	}
	/* Match the vendor call exactly: reserve one packet length in the clone. */
	clone_skb = skb_copy(skb, 1, packet_len);
	if (clone_skb == NULL) {
		recvframe_release(frame, free_queue);
		recvframe_release(clone_frame, free_queue);
		return;
	}

	STORE_PTR(clone_frame, RECVFRAME_ADAPTER, buddy);
	rtw_init_listhead(clone_frame);
	STORE_U32(clone_frame, RECVFRAME_LEN, 0U);
	STORE_U32(clone_frame, RECVFRAME_CLONE_LEN, 0U);
	rtw_memcpy((u8 *)clone_frame + RECVFRAME_ATTR,
		   (u8 *)frame + RECVFRAME_ATTR, RECVFRAME_ATTR_SIZE);
	recvframe_bind_skb(clone_frame, buddy, clone_skb, clone_skb->len);
	if (LOAD_U8(clone_frame, RECVFRAME_PHY_STATUS) != 0U &&
	    LOAD_U8(clone_frame, RECVFRAME_PACKET_TYPE) == 0U) {
		rtl8195b_query_rx_phy_status(clone_frame, phy_status);
	}
	rtw_recv_entry(clone_frame);

original:
	if (LOAD_U8(frame, RECVFRAME_PHY_STATUS) != 0U) {
		rtl8195b_query_rx_phy_status(frame, phy_status);
	}
	rtw_recv_entry(frame);
}

WLAN_TEXT(CheckRxTgRtl8195b) int CheckRxTgRtl8195b(void)
{
	return 1;
}

WLAN_TEXT(rtl8192ee_check_rxdesc_remain)
u16 rtl8192ee_check_rxdesc_remain(void *adapter)
{
	u32 value = rtw_read32(adapter, REG_RX_RING_INDEX);
	u16 producer = (u16)((value >> 16) & 0x7ffU);
	u16 consumer = (u16)(value & 0x7ffU);

	if (producer == consumer) {
		return 0;
	}
	if (producer > consumer) {
		return (u16)(producer - consumer);
	}
	return (u16)(LOAD_U32(adapter, ADAPTER_RX_RING_COUNT) +
			producer - consumer);
}

WLAN_TEXT(rtl8195b_recv_tasklet) void rtl8195b_recv_tasklet(void *adapter)
{
	void *hal_data = LOAD_PTR(adapter, ADAPTER_HAL_DATA);
	void *buddy = LOAD_PTR(adapter, ADAPTER_BUDDY);
	void *free_queue = (u8 *)adapter + ADAPTER_FREE_RECV_QUEUE;
	u16 remaining = rtl8192ee_check_rxdesc_remain(adapter);

	while (remaining != 0U) {
		u32 index = LOAD_U32(adapter, ADAPTER_RX_RING_INDEX);
		volatile u32 *descriptor =
			(volatile u32 *)LOAD_PTR(adapter, ADAPTER_RX_DESC_BASE) +
			index * 2U;
		volatile u32 *shadow_entry =
			(volatile u32 *)((u8 *)adapter + ADAPTER_RX_SHADOW_BASE) +
			index;
		u8 *completed_base = (u8 *)(uintptr_t)*shadow_entry;
		u32 completed_bytes = descriptor[0] & RX_DESC_LENGTH_MASK;
		void *frame;
		u8 *phy_status = NULL;
		u8 *packet_source;
		u32 packet_offset;
		u32 packet_len;
		struct sk_buff *skb = NULL;
		int dma_result;

		/* The vendor object synchronizes descriptor word1, while descriptor
		 * parsing and payload addressing use the shadow table.  Keep that
		 * distinction so a layout mismatch safely falls back to copying. */
#if CONFIG_WLAN_RX_DMA_FORCE_COPY
		/* Match the vendor object's indirect hal_cache_stubs + 0x20 call. */
		hal_cache_stubs.dcache_invalidate_by_addr(
			(u32 *)(uintptr_t)descriptor[1], (int)completed_bytes);
#else
		wlan_rx_dma_invalidate_completed(
			(void *)(uintptr_t)descriptor[1], completed_bytes);
#endif
		frame = rtw_alloc_recvframe(free_queue);
		if (frame == NULL) {
			STORE_U16(adapter, ADAPTER_RECVFRAME_FAIL,
				  LOAD_U16(adapter, ADAPTER_RECVFRAME_FAIL) + 1U);
			goto rearm;
		}

		rtw_init_listhead(frame);
		STORE_U32(frame, RECVFRAME_LEN, 0U);
		rtl8195b_query_rx_desc_status(frame, completed_base);
		packet_len = LOAD_U16(frame, RECVFRAME_PKT_LEN);
		if (packet_len == 0U || packet_len > RX_MAX_PACKET_LENGTH) {
			if (GlobalDebugEnable != 0U) {
				printf("RX invalid len=%lu crc=%u icv=%u max=%u\n",
				       (unsigned long)packet_len,
				       LOAD_U8(frame, RECVFRAME_CRC_ERR),
				       LOAD_U8(frame, RECVFRAME_ICV_ERR),
				       RX_MAX_PACKET_LENGTH);
			}
			recvframe_release(frame, free_queue);
			goto rearm;
		}
		if (LOAD_U8(frame, RECVFRAME_CRC_ERR) != 0U) {
			recvframe_release(frame, free_queue);
			goto rearm;
		}
		if (LOAD_U8(frame, RECVFRAME_ICV_ERR) != 0U) {
			recvframe_handle_icv_error(adapter);
		}

		if (LOAD_U8(frame, RECVFRAME_PHY_STATUS) != 0U) {
			phy_status = completed_base + RX_DESCRIPTOR_OFFSET;
		}
		packet_offset = RX_DESCRIPTOR_OFFSET +
			LOAD_U8(frame, RECVFRAME_DRVINFO_SIZE) +
			LOAD_U8(frame, RECVFRAME_SHIFT_SIZE);
		packet_source = completed_base + packet_offset;

		if (CONFIG_WLAN_RX_DMA_FORCE_COPY) {
			dma_result = WLAN_RX_DMA_ROTATE_COPY;
		} else {
			dma_result = wlan_rx_dma_rotate(adapter, index, descriptor,
							 shadow_entry,
							 LOAD_U16(adapter,
								  ADAPTER_RX_RING_SIZE) &
							 RX_DESC_LENGTH_MASK,
							 packet_offset, packet_len, &skb);
		}
		if (dma_result == WLAN_RX_DMA_ROTATE_ZERO_COPY) {
			recvframe_bind_skb(frame, adapter, skb, packet_len);
		} else {
			u32 buffer_size = packet_len + 14U;
			u32 reserve = LOAD_U8(frame, RECVFRAME_QOS) ? 6U : 0U;

			if (LOAD_U16(frame, RECVFRAME_SPECIAL) == 0x100U &&
			    packet_len <= RX_SPECIAL_LENGTH) {
				buffer_size = RX_SPECIAL_BUFFER_SIZE;
			}
			skb = dev_alloc_skb(buffer_size, 0U);
			if (skb == NULL) {
				recvframe_release(frame, free_queue);
				STORE_U16(adapter, ADAPTER_SKB_FAIL,
					  LOAD_U16(adapter, ADAPTER_SKB_FAIL) + 1U);
				goto rearm;
			}
			skb->dev = LOAD_PTR(adapter, ADAPTER_NETDEV);
			skb->len = packet_len;
			STORE_PTR(frame, RECVFRAME_PACKET, skb);
			skb_reserve(skb, reserve);
			rtw_memcpy(skb->data, packet_source, packet_len);
			recvframe_bind_skb(frame, adapter, skb, packet_len);
		}

		if (rtw_buddy_adapter_up(adapter) != 0) {
			recvframe_process_buddy(adapter, buddy, frame, free_queue,
						completed_base, phy_status);
		} else if (LOAD_U8(frame, RECVFRAME_PACKET_TYPE) == 0U) {
			if (LOAD_U8(frame, RECVFRAME_PHY_STATUS) != 0U) {
				rtl8195b_query_rx_phy_status(frame, phy_status);
			}
			rtw_recv_entry(frame);
		} else if (LOAD_U8(frame, RECVFRAME_PACKET_TYPE) == 4U) {
			c2h_pre_handler_rtl8195b(adapter, completed_base,
						packet_len + RX_DESCRIPTOR_OFFSET);
			recvframe_release(frame, free_queue);
		} else {
			if (LOAD_U8(frame, RECVFRAME_PACKET_TYPE) == 2U) {
				printf("rx TX RPT\n\r");
			}
			recvframe_release(frame, free_queue);
		}

rearm:
		recvframe_rearm_descriptor(adapter, descriptor);
		recvframe_advance_ring(adapter);
		remaining--;
	}

	/* Preserve the vendor no-descriptor overflow-recovery sequence. */
	{
		unsigned long flags = 0U;
		void *lock = (u8 *)LOAD_PTR(adapter, ADAPTER_DVOBJ) + 0xf4U;
		u32 value_b8;
		u32 value_3ec;

		rtw_enter_critical(lock, &flags);
		value_3ec = LOAD_U32(hal_data, HAL_RX_OVERFLOW_3EC) | 1U;
		STORE_U32(hal_data, HAL_RX_OVERFLOW_3EC, value_3ec);
		value_b8 = LOAD_U32(hal_data, HAL_RX_OVERFLOW_B8) | 0x100U;
		STORE_U32(hal_data, HAL_RX_OVERFLOW_B8, value_b8);
		rtw_write32(adapter, 0xb8U, value_b8);
		rtw_write32(adapter, 0x3ecU, value_3ec);
		rtw_exit_critical(lock, &flags);
	}
}

WLAN_TEXT(rtl8195ba_init_recv_priv) int rtl8195ba_init_recv_priv(void *adapter)
{
	void *queue = (u8 *)adapter + ADAPTER_RECVBUF_QUEUE;
	u8 *raw;
	u8 *recvbuf;
	int result;

	rtw_init_queue(queue);
	raw = (u8 *)rtw_zmalloc(40U);
	STORE_PTR(adapter, ADAPTER_RECVBUF_RAW, raw);
	if (raw == NULL) {
		return 0;
	}
	recvbuf = (u8 *)(((uintptr_t)raw + 3U) & ~(uintptr_t)3U);
	STORE_PTR(adapter, ADAPTER_RECVBUF_ALIGNED, recvbuf);
	rtw_init_listhead(recvbuf);
	STORE_PTR(recvbuf, 8U, adapter);
	result = rtw_os_recvbuf_resource_alloc(adapter, recvbuf);
	if (result != 0) {
		rtw_list_insert_tail(recvbuf, queue);
		STORE_U32(adapter, ADAPTER_RECVBUF_COUNT, 1U);
		return result;
	}

	STORE_U32(adapter, ADAPTER_RECVBUF_COUNT, 0U);
	STORE_PTR(adapter, ADAPTER_RECVBUF_ALIGNED, NULL);
	rtw_mfree(raw, 40U);
	STORE_PTR(adapter, ADAPTER_RECVBUF_RAW, NULL);
	return 0;
}

WLAN_TEXT(rtl8195ba_free_recv_priv) void rtl8195ba_free_recv_priv(void *adapter)
{
	void *recvbuf;
	void *raw;

#if !CONFIG_WLAN_RX_DMA_FORCE_COPY
	/* At this point the vendor teardown has stopped RX DMA. */
	wlan_rx_dma_deinit(adapter);
#endif
	recvbuf = LOAD_PTR(adapter, ADAPTER_RECVBUF_ALIGNED);
	if (recvbuf != NULL) {
		STORE_U32(adapter, ADAPTER_RECVBUF_COUNT, 0U);
		rtw_list_delete(recvbuf);
		rtw_os_recvbuf_resource_free(adapter, recvbuf);
		STORE_PTR(adapter, ADAPTER_RECVBUF_ALIGNED, NULL);
	}
	raw = LOAD_PTR(adapter, ADAPTER_RECVBUF_RAW);
	if (raw != NULL) {
		rtw_mfree(raw, 40U);
		STORE_PTR(adapter, ADAPTER_RECVBUF_RAW, NULL);
	}
}

WLAN_TEXT(rtl8195ba_rxhandler) void rtl8195ba_rxhandler(void *adapter)
{
	(void)adapter;
}
