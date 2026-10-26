/* Execute production workflow with radio/flash calls recorded, not emulated. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ACTION_HOST_TEST 1
#define portTICK_PERIOD_MS 1
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(x) (x)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define RTW_SUCCESS 0
#define RTW_SECURITY_WPA2_AES_PSK 4
#define RTW_SECURITY_OPEN 0
#define DHCP_START 0
#define DHCP_ADDRESS_ASSIGNED 2
#define WIFI_EVENT_RX_MGNT 7
typedef void *QueueHandle_t;
struct netif { unsigned char hwaddr[6]; } xnetif[2]={{{0xdc,0x84,3,5,0x8d,0x7c}}};
static int calls, active_at, joins, join_fail, dhcps, dhcp_fail, reports, report_fail;
static int inits, init_fail, starts, ota_fail, deinits, resets, ready, disconnects;
static int allocations, queue_full, queued, enabled;
static unsigned last_delay;
static void *queued_item;
static char report[128];
static void *pvPortMalloc(size_t n) { void *p=malloc(n); if(p) allocations++; return p; }
static void vPortFree(void *p) { if(p) {allocations--;free(p);} }
static unsigned xTaskGetTickCount(void) {return 10000;}
static unsigned uxQueueMessagesWaiting(void *q) {(void)q;return 0;}
static int wifi_get_channel(int *p) {*p=36;return 0;}
static int wifi_get_rssi(int *p) {*p=-40;return 0;}
static int wifi_get_last_error(void) {return 0;}
static unsigned char *LwIP_GetIP(struct netif *n) {static unsigned char p[]={192,168,3,10};(void)n;return p;}
static unsigned char *LwIP_GetGW(struct netif *n) {return LwIP_GetIP(n);}
static void vTaskDelay(unsigned n) {last_delay=n;}
static int xQueueReceive(void *q,void *p,unsigned t) {(void)q;(void)p;(void)t;return 0;}
static int xQueueSend(void *q,void *p,unsigned t) {(void)q;(void)t;if(queue_full)return 0;queued_item=*(void **)p;queued++;return 1;}
static void *xQueueCreate(unsigned n,size_t size) {assert(n==16 && size==sizeof(void *));return (void *)1;}
static void vQueueDelete(void *q) {(void)q;}
static int xTaskCreate(void (*f)(void *),const char *n,unsigned s,void *a,unsigned p,void *h)
{(void)f;(void)n;(void)a;(void)h;assert(s==2048 && p==1);return 1;}
static void wifi_reg_event_handler(int e,void (*f)(char *,int,int,void *),void *p)
{assert(e==7 && f && !p);}
static void wifi_set_indicate_mgnt(int x) {enabled=x;}
static int rtw_get_random_bytes(void *p,unsigned n) {(void)p;assert(n==4);return 0;}
static int wifi_set_autoreconnect(int x) {assert(!x);return 0;}
static int wifi_connect(char *s,int sec,char *p,int sn,int pn,int key,void *sem)
{assert(!strcmp(s,"sukn") && sec==4 && !strcmp(p,"suknlovken") && sn==4 && pn==10 && key==0 && !sem);joins++;return join_fail?-1:0;}
static int wifi_disconnect(void) {disconnects++;return 0;}
static int LwIP_DHCP(int i,int state) {assert(!i && state==0);dhcps++;return dhcps<=dhcp_fail?3:2;}
static void netif_set_default(struct netif *n) {assert(n==xnetif);}
char *get_build_version(void) {return "LOCAL";}
int get_aac_cp_aa_state(void) {calls++;return active_at && calls>=active_at;}
void usb_ota_update_status(int s,int p) {(void)p;if(s==2)ready++;}
void sys_reset(void) {resets++;}
int carbox_ota_tcp_connect(const char *h,int p) {assert(!strcmp(h,"192.168.3.10") && p==58616);reports++;return report_fail?-1:42;}
int carbox_ota_send_all(int fd,const void *d,size_t n) {assert(fd==42 && n<128);memcpy(report,d,n);report[n]=0;return 0;}
static int fake_close(int fd) {assert(fd==42);return 0;}
#define close fake_close
#include "wifi_action_ota.c"
int ota_update_init(carplay_ota_context_t *c,char *h,int p,char *r,uint8_t t)
{(void)c;assert(!strcmp(h,"192.168.3.10") && p==8080 && !strcmp(r,"/ota_all.bin") && t==1);inits++;return init_fail?-1:0;}
int ota_update_start(carplay_ota_context_t *c) {assert(!c->progress_cb);starts++;return ota_fail?-1:0;}
int ota_update_deinit(carplay_ota_context_t *c) {(void)c;deinits++;return 0;}
static void reset(void) {
 calls=active_at=joins=join_fail=dhcps=dhcp_fail=reports=report_fail=0;
 inits=init_fail=starts=ota_fail=deinits=resets=ready=disconnects=0;
 idle_report_count=0;reported=0;last_delay=0;
}
int main(void) {
 struct action_request *r=calloc(1,sizeof(*r)+sizeof(struct action_ap));
 assert(r);
 unsigned char raw[39]={0xd0};
 r->ap_count=1;strcpy(r->ap[0].ssid,"sukn");
 strcpy(r->ap[0].password,"suknlovken");r->ap[0].secured=1;
 strcpy(r->url,"http://192.168.3.10:8080/ota_all.bin");strcpy(r->version,"REMOTE");
 reset();active_at=1;action_process(r);assert(!joins && !inits);
 reset();action_process(r);assert(joins==1 && reports==1 && starts==1 && deinits==1 && resets==1 && last_delay==20);
 assert(!strcmp(report,"DC8403 58D7C,LOCAL"));
 reset();active_at=2;action_process(r);assert(joins==1 && !reports && !starts);
 reset();join_fail=1;action_process(r);assert(joins==5 && resets==1 && !dhcps && last_delay==1000);
 reset();dhcp_fail=2;action_process(r);assert(joins==3 && dhcps==3 && disconnects==2 && starts==1);
 reset();ota_fail=1;action_process(r);assert(resets==1 && deinits==1 && last_delay==1000);
 reset();init_fail=1;action_process(r);assert(!starts && !resets);
 reset();strcpy(r->version,"LOCAL");action_process(r);assert(reports==1 && ready==1 && !starts);
 for(int i=0;i<8;i++) action_process(r);
 assert(reports==1 && joins==1 && ready==9);
 action_process(r);assert(reports==2 && joins==2 && ready==10);
 reset();report_fail=1;action_process(r);action_process(r);assert(reports==2 && joins==2 && !starts);
 reset();strcpy(r->version,"REMOTE");report_fail=1;action_process(r);assert(starts==1);
 carbox_wifi_action_ota_init();assert(enabled && action_queue);
 memcpy(raw+24,"\xc8OTA\xdd\x09litiantai",15);
 action_busy=1;action_rx((char *)raw,sizeof(raw),0,NULL);assert(!queued && !allocations && rx_stats.busy==1 && rx_stats.matched==1);
 action_busy=0;queue_full=1;action_rx((char *)raw,sizeof(raw),0,NULL);assert(!queued && !allocations && rx_stats.full==1);
 queue_full=0;action_rx((char *)raw,sizeof(raw),0,NULL);assert(queued==1 && allocations==1 && rx_stats.queued==1);
 vPortFree(queued_item);assert(!allocations);
 action_listen_log();
 free(r);
 puts("action workflow: PASS");
}
