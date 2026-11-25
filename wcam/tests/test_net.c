#include <arpa/inet.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "wcam_net.h"
#include "wcam_config.h"
#include "wcam_rtp.h"

typedef unsigned TickType_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef struct { int xOverflowCount; unsigned xTimeOnEntering; } TimeOut_t;
typedef struct { char ssid[33]; unsigned channel; } rtw_wifi_setting_t;
typedef struct { uint32_t addr; } ip4_addr_t;
struct netif { int up, link; uint32_t address; };
struct netif xnetif[1];
#define RTW_SUCCESS 0
#define RTW_MODE_STA 0
#define RTW_MODE_AP 1
#define RTW_SECURITY_WPA2_AES_PSK 4
#define WLAN0_NAME "wlan0"
#define WIFI_EVENT_DISCONNECT 0
#define pdTRUE 1
#define pdPASS 1
#define ERR_OK 0
#define tskIDLE_PRIORITY 0
#define configTICK_RATE_HZ 1000
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define xQueueReceive(a,b,c) ((void)(a), (void)(b), (void)(c), 0)
static int xQueueSend(void *q, const void *item, unsigned ticks) { return 0; }
#define xTaskCreate(fn,a,b,c,d,e) ((void)(fn), pdPASS)
#define vTaskDelete(t) abort()
#define LwIP_Init() ((void)0)
#define wifi_on(mode) 0
#define wifi_disable_powersave() ((void)0)
#define wifi_set_autoreconnect(a) ((void)0)
#define setsockopt(fd,l,opt,p,n) ((void)(fd),(void)(p),(void)0)
#define bind(fd,a,n) 0
#define socket(a,b,c) (++fd_count)
#define select mock_select
#define rtw_get_random_bytes(p,n) memset(p,1,n)
static int fd_count, connected, joins, waits, mutex_held, sent;
static unsigned test_channel = WCAM_WIFI_CHANNEL;
static const char *test_ssid = WCAM_WIFI_SSID;
static int late_join = 1, immediate_join, disconnect_during_setup, address_error;
static void (*disconnect_event)(char *,int,int,void *);
static void wifi_reg_event_handler(int event, void (*fn)(char *,int,int,void *), void *arg) {
    assert(event == WIFI_EVENT_DISCONNECT);
    disconnect_event = fn;
}
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex_held; }
static void xSemaphoreTake(SemaphoreHandle_t mutex, unsigned timeout) {
    assert(mutex == &mutex_held && !mutex_held);
    mutex_held = 1;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) {
    assert(mutex == &mutex_held && mutex_held);
    mutex_held = 0;
}
static int netifapi_netif_set_down(struct netif *iface) { iface->up = 0; return 0; }
static int netifapi_netif_set_link_down(struct netif *iface) { iface->link = 0; return 0; }
static int netifapi_netif_set_up(struct netif *iface) { iface->up = 1; return 0; }
static int netifapi_netif_set_link_up(struct netif *iface) {
    iface->link = 1;
    if (disconnect_during_setup) disconnect_event(NULL,0,0,NULL);
    return 0;
}
static int netifapi_netif_set_addr(struct netif *iface,const ip4_addr_t *addr,
                                   const ip4_addr_t *mask,const ip4_addr_t *gateway) {
    assert(mask->addr == inet_addr("255.255.255.0"));
    assert(gateway->addr == inet_addr("192.168.4.1"));
    if (address_error) return -1;
    iface->address = addr->addr;
    return 0;
}
#define sendto mock_sendto
static int mock_sendto(int fd,const void *p,size_t n,int flags,const struct sockaddr *addr,socklen_t len) {
    assert(mutex_held && connected);
    ++sent;
    return n;
}
static TickType_t ticks;
static jmp_buf done;

static int wifi_is_connected_to_ap(void) { return connected ? RTW_SUCCESS : -1; }
static int wifi_connect(const char *ssid, int security, const char *password,
                        int ssid_len, int password_len, int key, void *semaphore) {
    ++joins;
    if (immediate_join) {
        connected = 1;
        return RTW_SUCCESS;
    }
    return -1; /* Radio can still finish after the SDK's blocking timeout. */
}
static int wifi_disconnect(void) { connected=0; return 0; }
static int wifi_get_setting(const char *ifname, rtw_wifi_setting_t *setting) {
    strcpy(setting->ssid, test_ssid);
    setting->channel = test_channel;
    return 0;
}
static TickType_t xTaskGetTickCount(void) { return ticks; }
static void vTaskSetTimeOutState(TimeOut_t *t) { t->xOverflowCount=0; t->xTimeOnEntering=ticks; }
static void vTaskDelay(TickType_t n) {
    ++waits;
    assert(waits < 2000);
    ticks += n;
    if (late_join) connected = 1;
}
static int mock_select(int nfds, fd_set *reads, fd_set *w, fd_set *e, struct timeval *tv) {
    assert(joins == 1 && connected);
    assert(wcam_net_ready());
    assert(xnetif[0].up && xnetif[0].link && ticks < 1000);
    assert(xnetif[0].address == inet_addr("192.168.4.2"));
    longjmp(done, 1);
}
int wcam_rtp_tx_init(struct wcam_rtp_tx *tx,uint32_t ssrc,uint16_t seq,wcam_packet_fn send,void *arg) {
    tx->send=send; tx->arg=arg; return 0;
}
int wcam_rtp_send(struct wcam_rtp_tx *tx,const uint8_t *p,size_t n,uint32_t ts) {
    return tx->send(tx->arg,p,n);
}
void wcam_rtp_request_idr(struct wcam_rtp_rx *rx) {}
int wcam_rtcp_feedback(const uint8_t *p,size_t n,uint32_t ssrc) { return 0; }
size_t wcam_rtcp_sender(uint8_t *p,size_t n,const struct wcam_rtp_tx *t,uint32_t now) { return 0; }

#include "wcam_net.c"
int main(void) {
    if (!setjmp(done)) network_task(NULL);
    const uint8_t packet[] = {1,2,3};
    uint32_t tries = 1;
    uint32_t first = wcam_net_tx_generation();
    assert(first && wcam_net_take_idr());
    assert(wcam_net_send(packet,sizeof packet,0,first) == 0 && sent == 1);

    disconnect_event(NULL,0,0,NULL);
    assert(wcam_net_tx_generation() == 0);
    assert(wcam_net_send(packet,sizeof packet,0,first) < 0 && sent == 1);

    late_join = 0;
    test_ssid = "WCAM-RX-2";
    assert(!service_tx_link(&tries) && !connected && !wcam_net_ready());
    connected = 1;
    test_ssid = WCAM_WIFI_SSID;
    test_channel = 44;
    assert(!service_tx_link(&tries) && !connected && !wcam_net_ready());

    TickType_t start = ticks;
    assert(!service_tx_link(&tries) && ticks - start == 1000 && joins == 2);

    connected = 1;
    test_channel = WCAM_WIFI_CHANNEL;
    disconnect_during_setup = 1;
    assert(!service_tx_link(&tries) && !wcam_net_ready());
    disconnect_during_setup = 0;
    assert(service_tx_link(&tries) && wcam_net_take_idr());
    uint32_t second = wcam_net_tx_generation();
    assert(second && second != first);
    /* Callback for a remaining fragment of the previous AU. */
    assert(tx.send(tx.arg,packet,sizeof packet) < 0 && sent == 1);
    assert(wcam_net_send(packet,sizeof packet,0,first) < 0 && sent == 1);
    assert(wcam_net_send(packet,sizeof packet,0,second) == 0 && sent == 2);
    connected = 0; /* Polling also catches loss if the indication is missed. */
    immediate_join = 1;
    address_error = 1;
    assert(!service_tx_link(&tries) && !wcam_net_ready() && joins == 3);
    address_error = 0;
    assert(service_tx_link(&tries) && wcam_net_tx_generation() != second);
    assert(joins == 3);
    puts("TX late association, retry and packet generation tests passed");
    return 0;
}
