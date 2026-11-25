#include "wcam_config.h"
#include "wcam_net.h"
#include "wcam_rtp.h"
#include "wcam_uvc_control.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#if WCAM_ROLE_TX
#include "semphr.h"
#include "lwip/netifapi.h"
#endif
#include "wifi_conf.h"
#include "osdep_service.h"
#include "lwip_netconf.h"
#include "lwip/sockets.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static int rtp_fd=-1,rtcp_fd=-1;
static struct sockaddr_in peer;
static volatile int idr_requested;
#if WCAM_ROLE_TX
extern struct netif xnetif[];
static SemaphoreHandle_t tx_packet_lock;
static volatile uint32_t tx_generation, disconnect_epoch;
static uint32_t last_tx_generation, frame_generation;
#else
static volatile int link_ready;
#endif
static volatile uint32_t wanted_generation, requested_idr_epoch;
static uint32_t active_generation,pli_count,send_errors,pool_drops;
static QueueHandle_t ready_queue,free_queue;
#if WCAM_ROLE_TX
static struct wcam_rtp_tx tx;
#endif
static struct wcam_rtp_rx *rx;
static uint32_t now_ms(void) { return (uint32_t)((uint64_t)xTaskGetTickCount()*1000/configTICK_RATE_HZ); }
uint64_t wcam_net_now_ms(void)
{
    TimeOut_t snapshot;
    /* FreeRTOS samples the overflow counter and current tick together. */
    vTaskSetTimeOutState(&snapshot);
    uint64_t ticks = ((uint64_t)(uint32_t)snapshot.xOverflowCount << (8 * sizeof(TickType_t)))
                   | snapshot.xTimeOnEntering;
    return ticks / configTICK_RATE_HZ * 1000
         + ticks % configTICK_RATE_HZ * 1000 / configTICK_RATE_HZ;
}
static int udp_open(unsigned port) {
    struct sockaddr_in local; int fd=socket(AF_INET,SOCK_DGRAM,0), one=1;
    if(fd<0)return -1;
    memset(&local,0,sizeof local);local.sin_family=AF_INET;local.sin_port=htons(port);local.sin_addr.s_addr=INADDR_ANY;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    if(bind(fd,(struct sockaddr*)&local,sizeof local)<0) {close(fd);return -1;}
    return fd;
}
#if WCAM_ROLE_TX
static int send_packet(void *arg,const uint8_t *p,size_t n) {
    const uint32_t generation = *(const uint32_t *)arg;
    int rc = -1;
    xSemaphoreTake(tx_packet_lock, portMAX_DELAY);
    if (generation && generation == tx_generation) {
        rc = sendto(rtp_fd,p,n,0,(struct sockaddr*)&peer,sizeof peer);
        if (rc != (int)n) ++send_errors;
    }
    xSemaphoreGive(tx_packet_lock);
    if (rc != (int)n) return -1;
    /* Yield between packet bursts so Wi-Fi and RTCP service can run. */
    if((tx.packets&7u)==7u)vTaskDelay(1);
    return 0;
}
#endif
static uint8_t *frame_ready(void *arg,uint8_t *p,size_t n,uint32_t ts) {
    struct wcam_net_frame f={p,n,ts,active_generation,wcam_net_now_ms()}; uint8_t *next;
    (void)arg;
    if(!active_generation)return p;
    if(xQueueReceive(free_queue,&next,0)!=pdTRUE) { ++pool_drops; wcam_rtp_request_idr(rx); return p; }
    if(xQueueSend(ready_queue,&f,0)!=pdTRUE) {
        xQueueSend(free_queue,&next,0); ++pool_drops;wcam_rtp_request_idr(rx); return p;
    }
    return next;
}
#if WCAM_ROLE_TX
static void tx_disconnected(char *buf, int len, int flags, void *arg) {
    (void)buf; (void)len; (void)flags; (void)arg;
    taskENTER_CRITICAL();
    ++disconnect_epoch;
    tx_generation = 0;
    taskEXIT_CRITICAL();
}

/* A timed-out blocking join may still complete on the radio during backoff. */
static void wait_for_late_join(void) {
    TickType_t start = xTaskGetTickCount();
    TickType_t elapsed;
    while (wifi_is_connected_to_ap() != RTW_SUCCESS &&
           (elapsed = xTaskGetTickCount() - start) < pdMS_TO_TICKS(1000)) {
        TickType_t remaining = pdMS_TO_TICKS(1000) - elapsed;
        TickType_t poll = pdMS_TO_TICKS(20);
        vTaskDelay(remaining < poll ? remaining : poll);
    }
}

static int service_tx_link(uint32_t *tries) {
    int associated = wifi_is_connected_to_ap() == RTW_SUCCESS;
    uint32_t epoch;
    rtw_wifi_setting_t setting;
    if (associated && tx_generation) return 1;
    tx_generation = 0;
    /* Drain any send already inside lwIP. Taking the netif down clears ARP
     * entries and pending datagrams before the next association can use it. */
    xSemaphoreTake(tx_packet_lock, portMAX_DELAY);
    int down = netifapi_netif_set_down(&xnetif[0]);
    xSemaphoreGive(tx_packet_lock);
    if (down != ERR_OK || netifapi_netif_set_link_down(&xnetif[0]) != ERR_OK) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        return 0;
    }
    if (!associated) {
        printf("[wcam][wifi] connect ssid=%s attempt=%lu\r\n", WCAM_WIFI_SSID,
               (unsigned long)++(*tries));
        wifi_connect(WCAM_WIFI_SSID, RTW_SECURITY_WPA2_AES_PSK, WCAM_WIFI_PASSWORD,
                     sizeof(WCAM_WIFI_SSID)-1, sizeof(WCAM_WIFI_PASSWORD)-1, -1, NULL);
        if (wifi_is_connected_to_ap() != RTW_SUCCESS) {
            wait_for_late_join();
            return 0;
        }
    }

    epoch = disconnect_epoch;
    if (wifi_get_setting(WLAN0_NAME, &setting) != RTW_SUCCESS ||
        memcmp(setting.ssid, WCAM_WIFI_SSID, sizeof(WCAM_WIFI_SSID)) ||
        setting.channel != WCAM_WIFI_CHANNEL) {
        printf("[wcam][wifi] reject association; expected ssid=%s channel=%d\r\n",
               WCAM_WIFI_SSID, WCAM_WIFI_CHANNEL);
        wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(1000));
        return 0;
    }
    ip4_addr_t addr = {inet_addr(WCAM_TX_IP)};
    ip4_addr_t mask = {inet_addr("255.255.255.0")};
    ip4_addr_t gateway = {inet_addr(WCAM_RX_IP)};

    if (netifapi_netif_set_addr(&xnetif[0], &addr, &mask, &gateway) != ERR_OK ||
        netifapi_netif_set_up(&xnetif[0]) != ERR_OK ||
        netifapi_netif_set_link_up(&xnetif[0]) != ERR_OK) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        return 0;
    }
    wifi_disable_powersave();
    if (wifi_is_connected_to_ap() != RTW_SUCCESS) return 0;
    taskENTER_CRITICAL();
    if (epoch == disconnect_epoch) {
        if (!++last_tx_generation) ++last_tx_generation;
        idr_requested = 1;
        tx_generation = last_tx_generation;
    }
    taskEXIT_CRITICAL();
    if (!tx_generation) return 0;
    printf("[wcam][wifi] connected ssid=%s channel=%u ip=%s peer=%s generation=%lu\r\n",
           WCAM_WIFI_SSID, setting.channel, WCAM_TX_IP, WCAM_RX_IP,
           (unsigned long)last_tx_generation);
    return 1;
}
#else
static void static_ip(void) {
    LwIP_SetIP(0,ntohl(inet_addr(WCAM_ROLE_TX?WCAM_TX_IP:WCAM_RX_IP)),ntohl(inet_addr("255.255.255.0")),ntohl(inet_addr(WCAM_RX_IP)));
}
#endif
static void network_task(void *unused) {
    uint8_t packet[WCAM_RTP_MTU+1],control[128];
    uint32_t last_report=0,last_pli=0,last_log=0,self=0,tries=0;
#if WCAM_ROLE_RX
    uint32_t seen_idr_epoch=0,sent_feedback_epoch=0;
#endif
    (void)unused;
    (void)tries; (void)last_pli; (void)frame_ready;
    LwIP_Init();
    while(wifi_on(WCAM_ROLE_TX?RTW_MODE_STA:RTW_MODE_AP)<0) {
        printf("[wcam][wifi] role=%s wifi_on failed; retry\r\n",WCAM_ROLE_NAME);vTaskDelay(pdMS_TO_TICKS(1000));
    }
    wifi_set_autoreconnect(0); /* one explicit infinite retry owner */
    wifi_disable_powersave();
#if WCAM_ROLE_RX
    for(;;) {
        rtw_wifi_setting_t setting;
        if(wifi_start_ap(WCAM_WIFI_SSID,RTW_SECURITY_WPA2_AES_PSK,WCAM_WIFI_PASSWORD,
              sizeof(WCAM_WIFI_SSID)-1,sizeof(WCAM_WIFI_PASSWORD)-1,WCAM_WIFI_CHANNEL)>=0 &&
           wifi_get_setting(WLAN0_NAME,&setting)==0 && setting.channel==WCAM_WIFI_CHANNEL)
            break;
        printf("[wcam][wifi] 5 GHz AP channel %d unavailable; retry\r\n",WCAM_WIFI_CHANNEL);
        wifi_off();vTaskDelay(pdMS_TO_TICKS(1000));
        while(wifi_on(RTW_MODE_AP)<0)vTaskDelay(pdMS_TO_TICKS(1000));
        wifi_set_autoreconnect(0);wifi_disable_powersave();
    }
    static_ip();
    printf("[wcam][wifi] AP ssid=%s channel=%d ip=%s\r\n",WCAM_WIFI_SSID,WCAM_WIFI_CHANNEL,WCAM_RX_IP);
#endif
    rtw_get_random_bytes(&self,sizeof self); if(!self)self=0x5743414d;
#if WCAM_ROLE_TX
    tx_packet_lock = xSemaphoreCreateMutex();
    if (!tx_packet_lock) goto failed;
    wifi_reg_event_handler(WIFI_EVENT_DISCONNECT, tx_disconnected, NULL);
    if(wcam_rtp_tx_init(&tx,self,(uint16_t)self,send_packet,&frame_generation)<0)goto failed;
#else
    rx=calloc(1,sizeof *rx);
    if(!rx)goto failed;
    for(unsigned i=0;i<3;i++) {
        void *allocation=malloc(WCAM_ENCODED_SIZE+31);uint8_t *b;
        if(!allocation)goto failed;
        b=(uint8_t*)(((uintptr_t)allocation+31)&~(uintptr_t)31);
        /* Lifetime allocations: retained until reset, including USB failure. */
        if(!i) { if(wcam_rtp_rx_init(rx,b,WCAM_ENCODED_SIZE,frame_ready,NULL))goto failed; }
        else xQueueSend(free_queue,&b,0);
    }
#endif
    while((rtp_fd=udp_open(WCAM_RTP_PORT))<0)vTaskDelay(pdMS_TO_TICKS(1000));
    while((rtcp_fd=udp_open(WCAM_RTCP_PORT))<0)vTaskDelay(pdMS_TO_TICKS(1000));
    memset(&peer,0,sizeof peer);peer.sin_family=AF_INET;peer.sin_port=htons(WCAM_RTP_PORT);
    peer.sin_addr.s_addr=inet_addr(WCAM_ROLE_TX?WCAM_RX_IP:WCAM_TX_IP);
#if WCAM_ROLE_RX
    link_ready=1;
#endif
    for(;;) {
        uint32_t now;fd_set reads;struct timeval tv={0,5000};struct sockaddr_in from; socklen_t flen;
#if WCAM_ROLE_TX
        if (!service_tx_link(&tries)) continue;
#else
        if(active_generation!=wanted_generation) {
            active_generation=wanted_generation;wcam_rtp_resync(rx);
            last_pli=now_ms()-WCAM_PLI_MS;
        }
        if(seen_idr_epoch!=requested_idr_epoch) {
            seen_idr_epoch=requested_idr_epoch;
            wcam_rtp_request_idr(rx);
            ++rx->feedback_epoch;
        }
#endif
        FD_ZERO(&reads);FD_SET(rtcp_fd,&reads);
#if WCAM_ROLE_RX
        FD_SET(rtp_fd,&reads);
#endif
        if(select((rtp_fd>rtcp_fd?rtp_fd:rtcp_fd)+1,&reads,NULL,NULL,&tv)>0) {
            if(FD_ISSET(rtcp_fd,&reads)) {
                flen=sizeof from;int n=recvfrom(rtcp_fd,packet,sizeof packet,0,(struct sockaddr*)&from,&flen);
                if(n>0 && from.sin_addr.s_addr==peer.sin_addr.s_addr && from.sin_port==htons(WCAM_RTCP_PORT)) {
#if WCAM_ROLE_TX
                    if(wcam_rtcp_feedback(packet,(size_t)n,tx.ssrc)>0) {idr_requested=1;++pli_count;}
#else
                    wcam_rtcp_receive_sr(rx,packet,(size_t)n,now_ms());
#endif
                }
            }
#if WCAM_ROLE_RX
            if(FD_ISSET(rtp_fd,&reads)) {
                flen=sizeof from;int n=recvfrom(rtp_fd,packet,sizeof packet,0,(struct sockaddr*)&from,&flen);
                if(n>0 && from.sin_addr.s_addr==peer.sin_addr.s_addr && from.sin_port==htons(WCAM_RTP_PORT))
                    wcam_rtp_receive(rx,packet,(size_t)n,now_ms());
            }
#endif
        }
        now=now_ms();
#if WCAM_ROLE_RX
        wcam_rtp_poll(rx,now);
        int pli=active_generation && rx->have_ssrc &&
            (rx->feedback_epoch!=sent_feedback_epoch ||
             (rx->need_idr && now-last_pli>=WCAM_PLI_MS));
#else
        int pli=0;
#endif
        if(pli || now-last_report>=1000) {
            size_t n;struct sockaddr_in dst=peer;dst.sin_port=htons(WCAM_RTCP_PORT);
#if WCAM_ROLE_TX
            struct wcam_rtp_tx snapshot;
            taskENTER_CRITICAL();snapshot.ssrc=tx.ssrc;snapshot.timestamp=tx.timestamp;
            snapshot.packets=tx.packets;snapshot.octets=tx.octets;taskEXIT_CRITICAL();
            n=wcam_rtcp_sender(control,sizeof control,&snapshot,now);
#else
            n=wcam_rtcp_report(control,sizeof control,self,rx,pli,now);
#endif
            if(n && sendto(rtcp_fd,control,n,0,(struct sockaddr*)&dst,sizeof dst)!=(int)n)++send_errors;
            if(pli){last_pli=now;++pli_count;
#if WCAM_ROLE_RX
                sent_feedback_epoch=rx->feedback_epoch;
#endif
            }last_report=now;
        }
        if(now-last_log>=5000) {
#if WCAM_ROLE_TX
            printf("[wcam][net] role=TX link=%d packets=%lu bytes=%lu pli=%lu send_err=%lu\r\n",wcam_net_ready(),
                (unsigned long)tx.packets,(unsigned long)tx.octets,(unsigned long)pli_count,(unsigned long)send_errors);
#else
            printf("[wcam][net] role=RX active=%lu packets=%lu lost=%lu late=%lu ooo=%lu bad=%lu frames=%lu drop=%lu pool_drop=%lu wait_idr=%d pli=%lu\r\n",
                (unsigned long)active_generation,(unsigned long)rx->received,(unsigned long)rx->lost,(unsigned long)rx->late,(unsigned long)rx->out_of_order,
                (unsigned long)rx->invalid,(unsigned long)rx->frames,(unsigned long)rx->dropped,(unsigned long)pool_drops,
                rx->need_idr,(unsigned long)pli_count);
#endif
            last_log=now;
        }
    }
failed:
    printf("[wcam][net] initialization failed, reboot required\r\n");vTaskDelete(NULL);
}
int wcam_net_start(void) {
#if WCAM_ROLE_RX
    ready_queue=xQueueCreate(2,sizeof(struct wcam_net_frame));free_queue=xQueueCreate(2,sizeof(uint8_t*));
    if(!ready_queue || !free_queue)return -1;
#endif
    return xTaskCreate(network_task,"wcam_net",2048,NULL,tskIDLE_PRIORITY+2,NULL)==pdPASS?0:-1;
}
#if WCAM_ROLE_TX
uint32_t wcam_net_tx_generation(void) { return tx_generation; }
int wcam_net_ready(void) { return tx_generation != 0; }
int wcam_net_send(const uint8_t *p, size_t n, uint32_t ts, uint32_t generation) {
    if (!generation || generation != tx_generation) return -1;
    /* Only the TX encoder task drives the synchronous packetizer. */
    frame_generation = generation;
    return wcam_rtp_send(&tx, p, n, ts);
}
#else
int wcam_net_ready(void) { return link_ready; }
#endif
int wcam_net_take_idr(void) { int n;taskENTER_CRITICAL();n=idr_requested;idr_requested=0;taskEXIT_CRITICAL();return n; }
void wcam_net_rx_active(uint32_t generation) { wanted_generation=generation; }
void wcam_net_rx_request_idr(void) { ++requested_idr_epoch; }
int wcam_net_frame_get(struct wcam_net_frame *f,unsigned ms) {return xQueueReceive(ready_queue,f,pdMS_TO_TICKS(ms))==pdTRUE;}
void wcam_net_frame_release(struct wcam_net_frame *f) { xQueueSend(free_queue,&f->data,0); }
