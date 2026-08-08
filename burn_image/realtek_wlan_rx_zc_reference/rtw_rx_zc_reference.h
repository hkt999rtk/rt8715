/*
 * RTL8195B WLAN RX fixed-pool buffer-swap reference.
 *
 * This is a review interface, not a public ABI requirement.  Realtek should
 * keep the callback and descriptor types private to the WLAN archive.
 */
#ifndef RTW_RX_ZC_REFERENCE_H
#define RTW_RX_ZC_REFERENCE_H

#include <stdint.h>

#ifndef CONFIG_RTW_RX_BUFFER_SWAP
#define CONFIG_RTW_RX_BUFFER_SWAP 0
#endif

struct sk_buff;
struct rtw_rx_desc;

enum rtw_rx_buffer_owner {
	RTW_RX_BUFFER_FREE = 0,
	RTW_RX_BUFFER_RING,
	RTW_RX_BUFFER_STACK
};

struct rtw_rx_buffer {
	uint8_t *base;
	uint32_t capacity;
	enum rtw_rx_buffer_owner owner;
};

typedef int (*rtw_rx_buffer_swap_fn)(void *adapter,
				     struct rtw_rx_desc *desc,
				     struct sk_buff *skb,
				     uint32_t frame_offset,
				     uint32_t frame_length);

struct rtw_rx_swap_stats {
	uint64_t swap_packets;
	uint64_t swap_bytes;
	uint64_t legacy_copy_packets;
	uint64_t legacy_copy_bytes;
	uint32_t callback_null;
	uint32_t callback_reject;
	uint32_t ownership_error;
	uint32_t recycle_error;
};

#endif /* RTW_RX_ZC_REFERENCE_H */
