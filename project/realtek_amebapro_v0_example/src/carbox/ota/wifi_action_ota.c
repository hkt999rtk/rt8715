/* Port of carbox_smart.orig/component/wifi/api/wifi_init.c Action OTA flow.
 * RX only copies the frame. Connecting, reporting and flash I/O run on a worker.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wifi_action_protocol.h"
#include "carplay_ota_compat.h"
#include "carplay_ota_http.h"
#ifndef ACTION_HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "wifi/wifi_conf.h"
#include "wifi/wifi_ind.h"
#include "lwip_netconf.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#endif

extern struct netif xnetif[];
extern char *get_build_version(void);
extern int get_aac_cp_aa_state(void);
extern void usb_ota_update_status(int status, int percent);
extern void sys_reset(void);
/* Smart has an on-chip BT reconnect task; Pro1 uses the external MCU instead.
 * If a platform supplies that task, use its exact stop entry point. */
extern void stop_bt_task(void) __attribute__((weak));

struct action_message { size_t len; int flags; uint8_t frame[]; };
static QueueHandle_t action_queue;
static volatile unsigned action_busy;
static unsigned idle_report_count;
static int reported;
static struct {
    unsigned long mgmt, action, ota, matched, busy, no_queue, no_memory, full, queued, invalid;
    unsigned last_category;
} rx_stats;

static uint32_t action_now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void action_listen_log(void)
{
    static uint32_t last;
    uint32_t now=action_now_ms();
    int channel=-1, channel_ret;
    if((uint32_t)(now-last)<5000) return;
    last=now;
    channel_ret=wifi_get_channel(&channel);
    printf("[ACTION_OTA][RX] ms=%lu channel=%d channel_ret=%d mgmt=%lu action=%lu "
           "ota=%lu match=%lu queued=%lu busy_drop=%lu no_queue=%lu nomem=%lu full=%lu "
           "invalid=%lu pending=%u last_category=0x%02x\n",
           (unsigned long)now,channel,channel_ret,rx_stats.mgmt,rx_stats.action,rx_stats.ota,
           rx_stats.matched,rx_stats.queued,rx_stats.busy,rx_stats.no_queue,rx_stats.no_memory,
           rx_stats.full,rx_stats.invalid,(unsigned)uxQueueMessagesWaiting(action_queue),rx_stats.last_category);
}

static void action_address_log(void)
{
    const unsigned char *ip=LwIP_GetIP(&xnetif[0]), *gw=LwIP_GetGW(&xnetif[0]);
    int rssi=0, channel=-1;
    int rr=wifi_get_rssi(&rssi), cr=wifi_get_channel(&channel);
    printf("[ACTION_OTA] STA ip=%u.%u.%u.%u gw=%u.%u.%u.%u channel=%d(%d) rssi=%d(%d)\n",
           ip[0],ip[1],ip[2],ip[3],gw[0],gw[1],gw[2],gw[3],channel,cr,rssi,rr);
}

static int action_version_report(const struct action_url *url)
{
    char text[128];
    const unsigned char *mac=xnetif[0].hwaddr;
    int fd=carbox_ota_tcp_connect(url->host,58616), n, ret;
    if(fd<0) { printf("[ACTION_OTA] report connect failed\n"); return -1; }
    /* Preserve Smart's exact wire format (including its fourth MAC field). */
    n=snprintf(text,sizeof(text),"%02X%02X%02X%2X%02X%02X,%s",
               mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],get_build_version());
    ret=(n>0 && n<(int)sizeof(text) && !carbox_ota_send_all(fd,text,(size_t)n));
    vTaskDelay(pdMS_TO_TICKS(10));
    close(fd);
    printf("[ACTION_OTA] version report port=58616 result=%d\n",ret);
    return ret;
}

static void action_upgrade(struct action_url *url)
{
    carplay_ota_context_t ctx;
    int ret;
    memset(&ctx,0,sizeof(ctx));
    ret=ota_update_init(&ctx,url->host,url->port,url->path,1 /* OTA_HTTP */);
    if(ret) { printf("[ACTION_OTA] OTA init failed\n"); return; }
    usb_ota_update_status(1,0);
    ret=ota_update_start(&ctx);
    if(ota_update_deinit(&ctx)) ret=-1;
    printf("[ACTION_OTA] OTA result=%d; reboot\n",ret);
    vTaskDelay(pdMS_TO_TICKS(ret?1000:20));
    sys_reset();
}

static void action_process(const struct action_request *r)
{
    struct action_url url;
    int same=r->version[0] && !strcmp(r->version,get_build_version());
    unsigned retries=5;
    uint32_t started;
    int state;
    printf("[ACTION_OTA] parsed aps=%u local=%s remote=%s same=%d url_present=%d\n",
           r->ap_count,get_build_version(),r->version,same,r->url[0]!=0);
    if(same) {
        idle_report_count++;
        if(idle_report_count%10==0) reported=0;
        if(reported==1) {
            printf("[ACTION_OTA] already up-to-date; report cached\n");
            usb_ota_update_status(2,0);
            return;
        }
    }
    if(!r->ap_count) { printf("[ACTION_OTA] skip: no AP in frame\n"); return; }
    state=get_aac_cp_aa_state();
    printf("[ACTION_OTA] pre_connect cp_aa_state=%d\n",state);
    if(state) { printf("[ACTION_OTA] skip: CarPlay/AA in use\n"); return; }
    wifi_set_autoreconnect(0);
    for(;;) {
        unsigned random_value=(unsigned)rand();
        unsigned idx;
        rtw_get_random_bytes(&random_value,sizeof(random_value));
        idx=random_value%r->ap_count;
        const struct action_ap *ap=&r->ap[idx];
        int ret;
        printf("[ACTION_OTA] connect ssid=%s index=%u/%u secured=%d attempts_left=%u\n",
               ap->ssid,idx,r->ap_count,ap->secured,retries);
        started=action_now_ms();
        ret=wifi_connect((char *)ap->ssid,ap->secured?RTW_SECURITY_WPA2_AES_PSK:RTW_SECURITY_OPEN,
                         ap->secured?(char *)ap->password:NULL,strlen(ap->ssid),
                         ap->secured?strlen(ap->password):0,0,NULL);
        printf("[ACTION_OTA] connect result=%d wifi_error=%d elapsed_ms=%lu\n",
               ret,wifi_get_last_error(),(unsigned long)(action_now_ms()-started));
        if(ret!=RTW_SUCCESS) {
            printf("[ACTION_OTA] connect failed ret=%d\n",ret);
            vTaskDelay(pdMS_TO_TICKS(100));
            if(--retries==0) {
                printf("[ACTION_OTA] connect retry exhausted; reboot in 1000 ms\n");
                vTaskDelay(pdMS_TO_TICKS(1000)); sys_reset(); return;
            }
            continue;
        }
        printf("[ACTION_OTA] BT reconnect stop hook=%s\n",stop_bt_task?"present":"absent (external MCU)");
        if(stop_bt_task) stop_bt_task();
        started=action_now_ms();
        printf("[ACTION_OTA] DHCP start interface=0\n");
        ret=LwIP_DHCP(0,DHCP_START);
        printf("[ACTION_OTA] DHCP result=%d elapsed_ms=%lu\n",ret,(unsigned long)(action_now_ms()-started));
        if(ret!=DHCP_ADDRESS_ASSIGNED) {
            printf("[ACTION_OTA] DHCP failed; reconnect\n");
            wifi_disconnect(); vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        printf("[ACTION_OTA] DHCP assigned\n");
        /* Pro1 also has an AP and USB netif. OTA must route via STA. */
        netif_set_default(&xnetif[0]);
        action_address_log();
        break;
    }
    if(!r->url[0]) { printf("[ACTION_OTA] no URL; connect-only complete\n"); return; }
    if(action_parse_url(r->url,&url)) { printf("[ACTION_OTA] invalid HTTP URL\n"); return; }
    state=get_aac_cp_aa_state();
    printf("[ACTION_OTA] pre_report cp_aa_state=%d host=%s port=%d\n",state,url.host,url.port);
    if(state) { printf("[ACTION_OTA] skip: CarPlay/AA became active\n"); return; }
    reported=action_version_report(&url);
    if(same) {
        printf("[ACTION_OTA] already up-to-date\n"); usb_ota_update_status(2,0); return;
    }
    printf("[ACTION_OTA] upgrade local=%s remote=%s host=%s port=%d path=%s\n",
           get_build_version(),r->version,url.host,url.port,url.path);
    action_upgrade(&url);
}

static void action_worker(void *unused)
{
    struct action_message *m;
    (void)unused;
    for(;;) {
        if(xQueueReceive(action_queue,&m,pdMS_TO_TICKS(5000))==pdTRUE) {
            struct action_request *r;
            uint32_t started=action_now_ms();
            int ret;
            taskENTER_CRITICAL(); action_busy=1; taskEXIT_CRITICAL();
            printf("[ACTION_OTA] process begin ms=%lu len=%u flags=%d src=%02X:%02X:%02X:%02X:%02X:%02X\n",
                   (unsigned long)started,(unsigned)m->len,m->flags,
                   m->frame[10],m->frame[11],m->frame[12],m->frame[13],m->frame[14],m->frame[15]);
            r=pvPortMalloc(action_request_bytes(m->len));
            ret=r?action_parse_frame(m->frame,m->len,r):-1;
            vPortFree(m);
            if(ret==1) action_process(r);
            else {
                rx_stats.invalid++;
                if(r) printf("[ACTION_OTA] parse failed ret=%d offset=%u type=0x%02x len=%u\n",
                             ret,r->parse_offset,r->parse_type,r->parse_length);
                else printf("[ACTION_OTA] parse allocation failed\n");
            }
            vPortFree(r);
            printf("[ACTION_OTA] process end elapsed_ms=%lu\n",(unsigned long)(action_now_ms()-started));
            taskENTER_CRITICAL(); action_busy=0; taskEXIT_CRITICAL();
        }
        action_listen_log();
    }
}

static void action_rx(char *buf, int len, int flags, void *arg)
{
    static const unsigned char prefix[]={0xc8,'O','T','A',0xdd,9,
                                        'l','i','t','i','a','n','t','a','i'};
    struct action_message *m;
    (void)arg;
    rx_stats.mgmt++;
    if(!buf || len<25 || ((unsigned char)buf[0]&0xfc)!=0xd0) return;
    rx_stats.action++; rx_stats.last_category=(unsigned char)buf[24];
    if(len<28 || memcmp(buf+24,prefix,4)) return;
    rx_stats.ota++;
    if(len<39 || len>ACTION_FRAME_MAX || memcmp(buf+24,prefix,sizeof(prefix))) return;
    rx_stats.matched++;
    if(!action_queue) {rx_stats.no_queue++; return;}
    if(action_busy) {rx_stats.busy++; return;}
    m=pvPortMalloc(sizeof(*m)+(size_t)len);
    if(!m) {rx_stats.no_memory++; return;}
    m->len=(size_t)len; m->flags=flags; memcpy(m->frame,buf,(size_t)len);
    if(xQueueSend(action_queue,&m,0)!=pdTRUE) {
        vPortFree(m); rx_stats.full++; return;
    }
    rx_stats.queued++;
}

void carbox_wifi_action_ota_init(void)
{
    if(!action_queue) {
        action_queue=xQueueCreate(16,sizeof(struct action_message *));
        if(!action_queue) { printf("[ACTION_OTA] queue create failed\n"); return; }
        if(xTaskCreate(action_worker,"action_ota",2048,NULL,1,NULL)!=pdPASS) {
            vQueueDelete(action_queue); action_queue=NULL;
            printf("[ACTION_OTA] worker create failed\n"); return;
        }
    }
    wifi_reg_event_handler(WIFI_EVENT_RX_MGNT,action_rx,NULL);
    wifi_set_indicate_mgnt(1);
    printf("[ACTION_OTA] enabled build=%s marker=C8/OTA/litiantai queue=16 priority=1 stats_ms=5000\n",get_build_version());
}
