/**
 * @file
 * Ethernet Interface Skeleton
 *
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

/*
 * This file is a skeleton for developing Ethernet network interface
 * drivers for lwIP. Add code to the low_level functions and do a
 * search-and-replace for the word "ethernetif" to replace it with
 * something that better describes your network interface.
 */

#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/tcpip.h"
#include "lwip/icmp.h"
#include "netif/etharp.h"
#include "lwip/err.h"
#include "ethernetif.h"
#include "lwip_netconf.h"

#include "lwip/ethip6.h" //Add for ipv6

#include "wifi_conf.h"
#include "platform_stdlib.h"
#include "basic_types.h"

#if CONFIG_WLAN
#include <lwip_intf.h>
#endif

#define netifMTU                                (1500)
#define netifINTERFACE_TASK_STACK_SIZE        ( 350 )
#define netifINTERFACE_TASK_PRIORITY        ( configMAX_PRIORITIES - 1 )
#define netifGUARD_BLOCK_TIME            ( 250 )
/* The time to block waiting for input. */
#define emacBLOCK_TIME_WAITING_FOR_INPUT    ( ( portTickType ) 100 )

#define IF2NAME0 'e'
#define IF2NAME1 'n'


struct netif eth_netif;


static void arp_timer(void *arg);

static const char *TAG = "ETHERNET";
#define ETHERNET_DEBUG		(0)
#define RTK_LOG_ETHERNET(format, ...) do {               \
        if ( ETHERNET_DEBUG ) printf(format, ##__VA_ARGS__); \
    } while(0);

#if defined(CONFIG_ETHERNET) && CONFIG_ETHERNET
#define MAX_BUFFER_SIZE		(1536)
#define DST_MAC_LEN			(6)
#define SRC_MAC_LEN			(6)
#define PROTO_TYPE_LEN		(2)  // protocol type
#define IP_LEN_OFFSET		(2)  // offset of total length field in IP packet
#define ETHERNET_REASSEMBLE_PACKET	(0)


static u8 TX_BUFFER[MAX_BUFFER_SIZE] __attribute__((aligned(32)));
static u8 RX_BUFFER[MAX_BUFFER_SIZE];

#if defined(CONFIG_USBH_CDC_ECM)
extern int usbh_cdc_ecm_send_data(u8 *buf, u32 len);
#elif defined(CONFIG_USBH_CDC_NCM)
extern int usbh_cdc_ncm_send_data(u8 *buf, u32 len);
#endif

#if defined(CONFIG_USBH_CDC_ECM)
extern u16 usbh_cdc_ecm_get_receive_mps(void);
#elif defined(CONFIG_USBH_CDC_NCM)
extern u16 usbh_cdc_ncm_get_receive_mps(void);
#endif

#endif

/**
 * In this function, the hardware should be initialized.
 * Called from ethernetif_init().
 *
 * @param netif the already initialized lwip network interface structure
 *        for this ethernetif
 */
static void low_level_init(struct netif *netif)
{
	(void) netif;

	/* set MAC hardware address length */
	netif->hwaddr_len = ETHARP_HWADDR_LEN;
	netif->mtu = 1500;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
#if LWIP_IGMP
	/* make LwIP_Init do igmp_start to add group 224.0.0.1 */
	netif->flags |= NETIF_FLAG_IGMP;
#endif

#if LWIP_IPV6
#if LWIP_IPV6_MLD
	netif->flags |= NETIF_FLAG_MLD6;
#endif
#endif


}

/**
 * This function should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @param p the MAC packet to send (e.g. IP packet including MAC addresses and type)
 * @return ERR_OK if the packet could be sent
 *         an err_t value if the packet couldn't be sent
 *
 * @note Returning ERR_MEM here if a DMA queue of your MAC is full can lead to
 *       strange results. You might consider waiting for space in the DMA queue
 *       to become availale since the stack doesn't retry to send a packet
 *       dropped because of memory failure (except for the TCP timers).
 */

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
	int ret;
	struct eth_drv_sg sg_list[MAX_ETH_DRV_SG];
	int sg_len = 0;
	struct pbuf *q;

	for (q = p; q != NULL && sg_len < MAX_ETH_DRV_SG; q = q->next) {
		sg_list[sg_len].buf = (unsigned int) q->payload;
		sg_list[sg_len++].len = q->len;
	}
	if (sg_len) {
		ret = rltk_wlan_send(netif_get_idx(netif), sg_list, sg_len, p->tot_len);
		if (ret) {
			return ERR_BUF;
		}
	}
	return ERR_OK;
}

/*for ethernet mii interface*/
static err_t low_level_output_mii(struct netif *netif, struct pbuf *p)
{
	(void) netif;
	(void) p;
#if (defined(CONFIG_ETHERNET) && CONFIG_ETHERNET)
	// printf("[NCM] tx len=%d\n", p->tot_len);
	struct pbuf *q;
	u8 *pdata = TX_BUFFER;
	u32 size = 0;
	int ret = 0;

	memset(TX_BUFFER, 0, MAX_BUFFER_SIZE);
	for (q = p; q != NULL; q = q->next) {
		memcpy((unsigned int *)pdata, (unsigned int *) q->payload, q->len);
		pdata += q->len;
		size += q->len;
	}

#if defined(CONFIG_USBH_CDC_ECM)
	ret = usbh_cdc_ecm_send_data(TX_BUFFER, size);
#elif defined(CONFIG_USBH_CDC_NCM)
	ret = usbh_cdc_ncm_send_data(TX_BUFFER, size);
#endif
	if (ret != 0) {
		printf("%s error = %d\n", __func__, ret);
		return ERR_BUF;
	}
#endif
	return ERR_OK;
}


/**
 * Should allocate a pbuf and transfer the bytes of the incoming
 * packet from the interface into the pbuf.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return a pbuf filled with the received packet (including MAC header)
 *         NULL on memory error
 */

#if defined(ETHERNET_REASSEMBLE_PACKET) && ETHERNET_REASSEMBLE_PACKET
static u32 pkt_total_len = 0;
static u32 rx_buffer_saved_data_len = 0;
static u16 eth_type = 0;

u8 rltk_mii_recv_data(u8 *buf, u32 frame_length, u32 *total_len)
{
	RTK_LOG_ETHERNET("enter %s %d\n", __func__, __LINE__);

#if defined(CONFIG_ETHERNET) && CONFIG_ETHERNET
	u8 *pbuf;
	u32 pkt_len_index = DST_MAC_LEN + SRC_MAC_LEN + PROTO_TYPE_LEN;
#if defined(CONFIG_USBH_CDC_ECM)
	u16 usb_receive_mps = usbh_cdc_ecm_get_receive_mps();
#elif defined(CONFIG_USBH_CDC_NCM)
	u16 usb_receive_mps = usbh_cdc_ncm_get_receive_mps();
#else
	u16 usb_receive_mps = 512;
#endif

	if (0 == pkt_total_len) {
		pbuf = RX_BUFFER;
		if (frame_length > 0) {
			memcpy((void *)pbuf, buf, frame_length);
		}
		if ((0 == frame_length) || (frame_length % usb_receive_mps != 0)) {
			*total_len = frame_length;
			rx_buffer_saved_data_len = 0;
			pkt_total_len = 0;
			return 1;
		} else {
			rx_buffer_saved_data_len = frame_length;
			eth_type = buf[DST_MAC_LEN + SRC_MAC_LEN] * 256 + buf[DST_MAC_LEN + SRC_MAC_LEN + 1];

			if (eth_type == ETH_P_IP) {
				pkt_total_len =  buf[pkt_len_index + IP_LEN_OFFSET] * 256 + buf[pkt_len_index + IP_LEN_OFFSET + 1];
			}
		}
	} else {
		if (rx_buffer_saved_data_len + frame_length > MAX_BUFFER_SIZE) {
			printf("frame_length(%d) and rx_buffer_saved_data_len(%d) is too long\n", frame_length, rx_buffer_saved_data_len);
			rx_buffer_saved_data_len = 0;
		}

		pbuf = RX_BUFFER + rx_buffer_saved_data_len;
		if (frame_length > 0) {
			memcpy((void *)pbuf, buf, frame_length);
		}
		rx_buffer_saved_data_len += frame_length;
		if ((0 == frame_length) || (frame_length % usb_receive_mps != 0)) {
			*total_len = rx_buffer_saved_data_len;
			rx_buffer_saved_data_len = 0;
			pkt_total_len = 0;
			return 1;
		}
	}
#endif
	return 0;
}
#endif

u8 rltk_mii_recv_data_check(u8 *mac)
{
	(void) mac;
	u8 check_res = 1;
	return check_res;
#if defined(CONFIG_ETHERNET) && CONFIG_ETHERNET
	{
		u8 *pbuf = RX_BUFFER;
		u8 multi_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

		if (memcmp(mac, pbuf, ETH_ALEN) == 0 || memcmp(multi_mac, pbuf, ETH_ALEN) == 0) {
			check_res = 1;
		} else {
			check_res = 0;
		}
	}
	return check_res;
#endif
}
void rltk_mii_recv(struct eth_drv_sg *sg_list, int sg_len){
	struct eth_drv_sg *last_sg;
	u8* pbuf = RX_BUFFER;

	for (last_sg = &sg_list[sg_len]; sg_list < last_sg; ++sg_list) {
		if (sg_list->buf != 0) {
			rtw_memcpy((void *)(sg_list->buf), pbuf, sg_list->len);
			pbuf+=sg_list->len;
		}			 
	}
}
void ethernetif_mii_recv(u8 *buf, u32 frame_len)
{
	(void) buf;
	(void) frame_len;
#if (defined(CONFIG_ETHERNET) && CONFIG_ETHERNET)
	// printf("[NCM] ethernetif_mii_recv len=%d\n", frame_len);
	struct eth_drv_sg sg_list[MAX_ETH_DRV_SG];
	struct pbuf *p, *q;
	int sg_len = 0;
	u32 total_len = 0;

	struct netif *netif = &eth_netif;
	u8 *macstr = (u8 *)(netif->hwaddr);

	if (frame_len > MAX_BUFFER_SIZE) {
		printf("recv data len is %d\n", frame_len);
		return;
	}

#if defined(ETHERNET_REASSEMBLE_PACKET) && ETHERNET_REASSEMBLE_PACKET
	RTK_LOG_ETHERNET("%s %d will rltk_mii_recv_data\n", __func__, __LINE__);
	if (0 == rltk_mii_recv_data(buf, frame_len, &total_len)) {
		return;
	}
#else
	if (0 == frame_len) {
		printf("recv data len is 0\n");
		return;
	}
	total_len = frame_len;
	memcpy((u8 *)RX_BUFFER, buf, frame_len);
#endif
	if (0 == rltk_mii_recv_data_check(macstr)) {
		RTK_LOG_ETHERNET("rltk_mii_recv_data_check fail\n");
		return;
	}

	RTK_LOG_ETHERNET("%s %d rltk_mii_recv_data_check ok\n", __func__, __LINE__);

	p = pbuf_alloc(PBUF_RAW, total_len, PBUF_POOL);
	if (p == NULL) {
		printf("\n\r[%s]Cannot allocate pbuf to receive packet(%d)\n", __func__, total_len);
		return;
	}

	for (q = p; q != NULL && sg_len < MAX_ETH_DRV_SG; q = q->next) {
		sg_list[sg_len].buf = (unsigned int) q->payload;
		sg_list[sg_len++].len = q->len;
	}
	rltk_mii_recv(sg_list, sg_len);

	if (ERR_OK != netif->input(p, netif)) {
		pbuf_free(p);
	}
#endif
}

/**
 * Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 * This function should be passed as a parameter to netif_add().
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if the loopif is initialized
 *         ERR_MEM if private data couldn't be allocated
 *         any other err_t on error
 */
err_t ethernetif_init(struct netif *netif)
{
	LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
	if (netif->name[1] == '0') {
		netif->hostname = "lwip0";
	} else if (netif->name[1] == '1') {
		netif->hostname = "lwip1";
	}
#endif /* LWIP_NETIF_HOSTNAME */

	netif->output = etharp_output;
#if LWIP_IPV6
	netif->output_ip6 = ethip6_output;
	netif->ip6_autoconfig_enabled = 1;
#endif
	netif->linkoutput = low_level_output;

	/* initialize the hardware */
	low_level_init(netif);

	etharp_init();

	return ERR_OK;
}

void rltk_mii_init(void)
{

}

err_t ethernetif_mii_init(struct netif *netif)
{
	LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
	netif->hostname = "lwip2";
#endif /* LWIP_NETIF_HOSTNAME */

	netif->output = etharp_output;
#if LWIP_IPV6
	netif->output_ip6 = ethip6_output;
#endif
	netif->linkoutput = low_level_output_mii;

	/* initialize the hardware */
	low_level_init(netif);

	etharp_init();

	return ERR_OK;
}

static u32_t arp_next_time = 0;

static void arp_timer(void *arg)
{
	(void) arg;
	etharp_tmr();
	arp_next_time = sys_now() + ARP_TMR_INTERVAL;
	sys_timeout(ARP_TMR_INTERVAL, arp_timer, NULL);
}

/*
 * For FreeRTOS tickless
 */
int lwip_tickless_used = 0;

int arp_timeout_exist(void)
{
	return (arp_next_time != 0) ? 1 : 0;
}

long long wakeup_tick_arptimer(void)
{
	return (long long)arp_next_time;
}

int rltk_mii_recv_memcpy(u8 *buf, u32 frame_len)
{
#if defined(CONFIG_ETHERNET) && CONFIG_ETHERNET
	memcpy((u8 *)RX_BUFFER, (u8 *)buf, frame_len);
#endif
	return 0;
}

void ethernetif_recv(struct netif *netif, int total_len)
{
	struct eth_drv_sg sg_list[MAX_ETH_DRV_SG];
	struct pbuf *p, *q;
	int sg_len = 0;

	// Allocate buffer to store received packet
	p = pbuf_alloc(PBUF_RAW, total_len, PBUF_POOL);
	if (p == NULL) {
		return;
	}

	// Create scatter list
	for (q = p; q != NULL && sg_len < MAX_ETH_DRV_SG; q = q->next) {
		sg_list[sg_len].buf = (unsigned int) q->payload;
		sg_list[sg_len++].len = q->len;
	}
	if (rltk_wlan_recv(netif_get_idx(netif), sg_list, sg_len) != 0) {
		/* A timed-out GDMA destination may contain only part of the frame. */
		pbuf_free(p);
		return;
	}

	// Pass received packet to the interface
	if (ERR_OK != netif->input(p, netif)) {
		pbuf_free(p);
	}
}

void lwip_PRE_SLEEP_PROCESSING(void)
{
}

void lwip_POST_SLEEP_PROCESSING(void)
{
}
