/*
 *  Hello World
 *
 *  Copyright (c) 2013 Realtek Semiconductor Corp.
 *
 *  This module is a confidential and proprietary property of RealTek and
 *  possession or use of this module requires written permission of RealTek.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h" 

#include "main.h"
#include "main_test.h"
#if CONFIG_WLAN
#include "wifi_conf.h"
#include "wlan_intf.h"
#include "wifi_constants.h"
#endif
#include "lwip_netconf.h"
#if CARBOX_RECOVERY
#include "recovery_mp.h"
extern void sta_ota_wifi_init(void);
#endif
#include <platform/platform_stdlib.h>
#include "etharp.h"

#ifndef CONFIG_INIT_NET
#define CONFIG_INIT_NET             1
#endif
#ifndef CONFIG_INTERACTIVE_MODE
#define CONFIG_INTERACTIVE_MODE     1
#endif

#define STACKSIZE                   (512 + 768)

xSemaphoreHandle uart_rx_interrupt_sema = NULL;
#if CONFIG_FAST_DHCP
extern struct netif xnetif[NET_IF_NUM];   
#endif
   
void init_thread(void *param)
{
#if CARBOX_RECOVERY
    /* MP=4已通过重启选择单STA校准分支。即使误调用wlan_network，
     * 也必须在LwIP/Wi-Fi/AP/STA_OTA/自动重连初始化之前退出。
     * 不可只屏蔽promisc，留下后台任务以后再次启用AP或修改RX过滤器。 */
    if (!recovery_mp_ota_allowed()) {
        printf("[REC_MP] AP/STA_OTA startup blocked in MP/transition mode\r\n");
        vTaskDelete(NULL);
        return;
    }
#endif

#if CONFIG_INIT_NET
#if CONFIG_LWIP_LAYER
	/* Initilaize the LwIP stack */
	LwIP_Init();
#endif
#endif
#if CONFIG_WIFI_IND_USE_THREAD
	wifi_manager_init();
#endif
#if CONFIG_WLAN
#if CONFIG_CARBOX_WIFI_STA_AP_COMPAT
	wifi_on(RTW_MODE_STA_AP);
#else
	wifi_on(RTW_MODE_STA);
#endif
#if CARBOX_RECOVERY
	sta_ota_wifi_init();
#endif
#if CONFIG_AUTO_RECONNECT
	//setup reconnection flag
	wifi_set_autoreconnect(1);
#endif
	printf("\n\r%s(%d), Available heap 0x%x", __FUNCTION__, __LINE__, xPortGetFreeHeapSize());	
#endif

#if CONFIG_INTERACTIVE_MODE
 	/* Initial uart rx swmaphore*/
	vSemaphoreCreateBinary(uart_rx_interrupt_sema);
	xSemaphoreTake(uart_rx_interrupt_sema, 1/portTICK_RATE_MS);
	start_interactive_mode();
#endif	
        
        /* Delivery four arp_request every 500 milliseconds to protect ip conflict */
#if CONFIG_FAST_DHCP           
        uint32_t IPaddress;
        if(wifi_is_connected_to_ap() == RTW_SUCCESS){
            for(int i = 0;i<4;i++){
#if LWIP_VERSION_MAJOR >= 2
                IPaddress = ip_addr_get_ip4_u32(netif_ip_addr4(&xnetif[0]));
#else
                IPaddress = xnetif[0]->ip_addr.addr;
#endif
                if(IPaddress == 0){
                  break;
                }
                etharp_request(&xnetif[0], netif_ip4_addr(&xnetif[0]));
                vTaskDelay(500);
            }
        }
#endif
        
	/* Kill init thread after all init tasks done */
	vTaskDelete(NULL);
}

void wlan_network()
{
	if(xTaskCreate(init_thread, ((const char*)"init"), STACKSIZE, NULL, tskIDLE_PRIORITY + 3 + PRIORITIE_OFFSET, NULL) != pdPASS)
		printf("\n\r%s xTaskCreate(init_thread) failed", __FUNCTION__);
}
