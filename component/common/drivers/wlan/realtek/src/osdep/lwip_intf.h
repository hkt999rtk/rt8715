/* mbed Microcontroller Library
 * Copyright (c) 2013-2016 Realtek Semiconductor Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __LWIP_INTF_H__
#define __LWIP_INTF_H__

#ifdef	__cplusplus
extern "C" {
#endif

#include <wireless.h>
#include <skbuff.h>

struct netif;

//----- ------------------------------------------------------------------
// Ethernet Buffer
//----- ------------------------------------------------------------------
#if defined(CONFIG_MBED_ENABLED)
struct eth_drv_sg {
    unsigned int			buf;
    unsigned int			len;
};

#define MAX_ETH_DRV_SG		32
#define MAX_ETH_MSG			1540
#else
#include "ethernetif.h"  // moved to ethernetif.h by jimmy 12/2/2015
#endif
//----- ------------------------------------------------------------------
// Wlan Interface Provided
//----- ------------------------------------------------------------------
unsigned char rltk_wlan_check_isup(int idx);
void rltk_wlan_tx_inc(int idx);
void rltk_wlan_tx_dec(int idx);
struct sk_buff * rltk_wlan_get_recv_skb(int idx);
struct sk_buff * rltk_wlan_alloc_skb(unsigned int total_len);
void rltk_wlan_set_netif_info(int idx_wlan, void * dev, unsigned char * dev_addr);
void rltk_wlan_send_skb(int idx, struct sk_buff *skb);	//struct sk_buff as defined above comment line
int rltk_wlan_send(int idx, struct eth_drv_sg *sg_list, int sg_len, int total_len);
int rltk_wlan_recv(int idx, struct eth_drv_sg *sg_list, int sg_len);
/*
 * Hold the closed WLAN driver's current RX skb after its receive callback
 * returns.  The handle is deliberately opaque to keep skb ownership rules out
 * of lwIP.  A successful acquire must be paired with exactly one release.
 */
int rltk_wlan_rx_ref_acquire(int idx, unsigned int expected_len,
			     void **handle, void **payload);
void rltk_wlan_rx_ref_release(void *handle);
int rltk_wlan_rx_ref_selftest(void);
/* Finish a synchronous netif_rx callback that used the swapped ring buffer. */
void rltk_wlan_rx_callback_complete(int idx);
int rltk_wlan_rx_swap_quiesce(unsigned int timeout_ms);
void rltk_wlan_rx_swap_resume(void);
void rltk_wlan_rx_swap_deinit_complete(void);
#if defined(CONFIG_PLATFORM_8195BHP)
typedef struct rltk_network_gdma_bench_sample_s {
	unsigned int dma_bytes;
	unsigned int submit_cycles;
	unsigned int poll_cycles;
	unsigned int finish_cycles;
	unsigned int dma_total_cycles;
	unsigned int poll_count;
	unsigned int yield_count;
} rltk_network_gdma_bench_sample_t;

int rltk_network_gdma_copy_tx(void *dst, const void *src, unsigned int len,
			      const void *allocation_end);
int rltk_network_gdma_copy_rx(void *dst, const void *src, unsigned int len,
			      const void *allocation_end);
int rltk_network_gdma_copy_socket_rx(void *dst, const void *src,
				     unsigned int len);
int rltk_network_gdma_copy_tcp_tx(void *dst, const void *src,
				  unsigned int len, unsigned int *dma_len);
unsigned int rltk_tcp_perf_now_us(void);
void rltk_tcp_perf_rx_complete(unsigned int start_us, unsigned int bytes,
			       unsigned int checksum_us);
void rltk_tcp_perf_tx_complete(unsigned int start_us, unsigned int bytes);
void rltk_tcp_perf_tx_copy(unsigned int bytes, unsigned int dma_bytes,
			   unsigned int elapsed_us);

/* tcp_output() profiling categories. Keep these numeric values stable so the
 * lwIP core and the WLAN glue can share a small, dependency-free interface. */
#define RLTK_TCP_OUTPUT_NOOP       0U
#define RLTK_TCP_OUTPUT_EMPTY_ACK  1U
#define RLTK_TCP_OUTPUT_DATA       2U
#define RLTK_TCP_OUTPUT_DEFERRED   3U
#define RLTK_TCP_OUTPUT_ERROR      4U
void rltk_tcp_output_profile_begin(void);
void rltk_tcp_output_profile_end(unsigned int kind, unsigned int segments,
				 unsigned int bytes, unsigned int cycles);
int rltk_tcp_output_profile_active(void);
void rltk_tcp_output_profile_lwip(unsigned int control,
				  unsigned int bytes,
				  unsigned int prepare_cycles,
				  unsigned int checksum_cycles,
				  unsigned int ip_cycles);
void rltk_tcp_output_profile_wlan(unsigned int bytes,
				  unsigned int alloc_cycles,
				  unsigned int copy_cycles,
				  unsigned int submit_cycles,
				  int result);
void rltk_wlan_rx_swap_profile_report(unsigned int sequence);
int rltk_network_gdma_benchmark_init(void);
void rltk_network_gdma_benchmark_print_status(unsigned int sequence);
int rltk_network_gdma_benchmark_copy(
	void *dst, const void *src, unsigned int len, const void *allocation_end,
	rltk_network_gdma_bench_sample_t *sample);
#endif
unsigned char rltk_wlan_running(unsigned char idx);		// interface is up. 0: interface is down

#if defined(CONFIG_MBED_ENABLED)
typedef void (*emac_callback)(void *param, struct netif *netif, unsigned int len);
void set_callback_func(emac_callback p, void *data);
#endif

//----- ------------------------------------------------------------------
// Network Interface provided
//----- ------------------------------------------------------------------

int netif_is_valid_IP(int idx,unsigned char * ip_dest);
int netif_get_idx(struct netif *pnetif);
unsigned char *netif_get_hwaddr(int idx_wlan);
void netif_rx(int idx, unsigned int len);
void netif_post_sleep_processing(void);
void netif_pre_sleep_processing(void);
#if (CONFIG_LWIP_LAYER == 1)
#if !defined(CONFIG_MBED_ENABLED)
extern void ethernetif_recv(struct netif *netif, int total_len);
#endif
extern void lwip_PRE_SLEEP_PROCESSING(void);
extern void lwip_POST_SLEEP_PROCESSING(void);
#endif //CONFIG_LWIP_LAYER == 1

#ifdef CONFIG_WOWLAN
extern unsigned char *rltk_wlan_get_ip(int idx);
extern unsigned char *rltk_wlan_get_gw(int idx);
extern unsigned char *rltk_wlan_get_gwmask(int idx);
#endif

#ifdef	__cplusplus
}
#endif

#endif //#ifndef __LWIP_INTF_H__
