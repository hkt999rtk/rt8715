#include "stubs.h"
#include "screen_session.h"
#include "screen_setup_evidence.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
volatile unsigned char carbox_screen_sending;
int carbox_screen_sock=-1;
void *carbox_screen_frames;
unsigned char carbox_screen_mutex[96];
int lwip_err;
static int writes,write_result=4,polls,joins,closed,starts,creates,exit_mode,fail_create,poll_result,poll_flags;
static void *fake_thread=(void*)0x1234;
static unsigned char info[1800];
static uint32_t now;
int lwip_errno(void){return lwip_err;}
uint32_t hal_read_curtime_us(void){return ++now;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (void*)1;}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*p){return p;}
int xSemaphoreTake(SemaphoreHandle_t p,unsigned t){return 1;}
int xSemaphoreGive(SemaphoreHandle_t p){return 1;}
void vTaskDelay(unsigned t){now+=t*1000; if(exit_mode){carbox_screen_frames=NULL; carbox_screen_thread_exit(NULL);}}
int lwip_write(int f,const void*p,size_t n){writes++; return write_result<0?write_result:(int)(n<(size_t)write_result?n:(size_t)write_result);}
int lwip_poll(struct pollfd*p,unsigned n,int t){polls++;p[0].revents=poll_flags;return poll_result;}
int lwip_shutdown(int fd,int how){return 0;}
int lwip_usb_screen_rto_enable(int f){return 0;}
int pthread_attr_setdetachstate(void*p,int d){assert(d==1);return 0;}
int pthread_create(void **p,const void*a,void*(*e)(void*),void*x){creates++;if(fail_create)return EAGAIN;*p=fake_thread;return 0;}
int pthread_join(void*t,void**r){assert(t==fake_thread);joins++;return 0;}
void *pthread_self(void){return NULL;}
void pthread_exit(void*p){}
int pthread_mutex_lock(void*p){return 0;}
int pthread_mutex_unlock(void*p){return 0;}
int CVector_size(void*p){return 0;}
void CVector_push_back(void*p,const void*f){}
void carbox_move_screen_free(void*p){free(p);}
void *GetAirPlayInfo(void){return info;}
void AirPlayClient_SendMessageTearDown(int t){assert(t==110);}
void AirPlayScreen_DestroyScreenSockConnect(void){assert(!carbox_screen_frames);closed++;carbox_screen_sock=-1;}
void carbox_vendor_screen_start(int w,int h){void*t;starts++;carbox_screen_sending=1;carbox_screen_sock=7;carbox_screen_frames=(void*)3;carbox_screen_create(&t,NULL,NULL,NULL);}
static int encrypt_calls;
void AirPlayScreen_EncryptData(void*h,void*p,int n){encrypt_calls++;}
const void *CFDictionaryGetValue(const void*d,const void*k){return d;}
unsigned long CFGetTypeID(const void*v){return (unsigned long)(uintptr_t)v;}
long CFArrayGetCount(const void*v){return 3;}
const void *CFDictionaryGetTypedValue(const void*d,const void*k,unsigned long t,int32_t*e){*e=!d?-6727:((uintptr_t)d==t?0:-6756);return *e?NULL:d;}
const void *CFBinaryPlistV0CreateWithData(const void*d,size_t n,int32_t*e){*e=0;return d;}
int HTTPClientSendMessage(void*c,void*m){return 0;}
void carbox_vendor_info_complete(void*m){}
uint32_t carbox_move_current_frame_id(void){return 42;}
extern const void *carbox_info_hid_lookup(const void *,const void *,unsigned long,int32_t *);
extern int carbox_info_send(void *,void *);
extern void carbox_info_complete(void *);
int main(void){
 unsigned char one[]={0,0,0,1,0x61,0x80,0,0,3,1};
 unsigned char two[]={0,0,0,1,0x61,0x80,0,0,0,1,0x65,0x80};
 struct screen_nal_summary n;
 screen_nal_scan(one,sizeof(one),&n);assert(n.count==1&&n.bad==0&&n.first_length==6);
 screen_nal_scan(two,sizeof(two),&n);assert(n.count==2&&n.first_length==2&&n.second_length==2);
 screen_nal_scan(two,3,&n);assert(n.bad);
 {unsigned char b[]={0,0,1,0x61};screen_nal_scan(b,sizeof(b),&n);assert(n.count==1&&n.prefix3==1);}
 {unsigned char b[]={0,0,0,1};screen_nal_scan(b,sizeof(b),&n);assert(n.bad);}
 {unsigned char saved[sizeof(two)];memcpy(saved,two,sizeof(two));carbox_screen_encrypt_observe(NULL,two,sizeof(two));assert(encrypt_calls==1&&!memcmp(saved,two,sizeof(two)));}
 {unsigned status,seq; const char*h="HTTP/1.1 200 OK\r\nCSeq: 123\r\n\r\n";screen_info_http_fields(h,strlen(h),&status,&seq);assert(status==200&&seq==123);screen_info_http_fields(h,5,&status,&seq);assert(status==0&&seq==UINT32_MAX);}
 {int32_t error=99;assert(!carbox_info_hid_lookup(NULL,NULL,7,&error)&&error==-6727);
  assert(!carbox_info_hid_lookup((void*)8,NULL,7,&error)&&error==-6756);
  assert(carbox_info_hid_lookup((void*)7,NULL,7,&error)==(void*)7&&error==0);}
 {unsigned char message[0x900]={0},saved[0x900];uint32_t size;
  const char *request="GET /info HTTP/1.1\r\nCSeq: 123\r\n\r\n";
  size=(uint32_t)strlen(request);memcpy(message+0x10,request,size);memcpy(message+0x410,&size,4);message[0x8fc]=5;
  memcpy(saved,message,sizeof(message));assert(carbox_info_send(NULL,message)==0);assert(!memcmp(saved,message,sizeof(message)));
  request="HTTP/1.1 200 OK\r\nCSeq: 123\r\n\r\n";size=(uint32_t)strlen(request);
  memcpy(message+0x10,request,size);memcpy(message+0x410,&size,4);memcpy(saved,message,sizeof(message));
  carbox_info_complete(message);assert(!memcmp(saved,message,sizeof(message)));}
 carbox_screen_setup_report(0);
 carbox_screen_sending=0;carbox_screen_sock=7;assert(carbox_screen_write(7,one,4)==-1&&writes==0);
 carbox_screen_sending=1;write_result=2;assert(carbox_screen_write(7,one,4)==2&&carbox_screen_sending);
 write_result=0;assert(carbox_screen_write(7,one,4)==-1&&!carbox_screen_sending);
 carbox_screen_sending=1;write_result=-1;lwip_err=EAGAIN;assert(carbox_screen_write(7,one,4)==-1&&carbox_screen_sending);
 lwip_err=EPIPE;assert(carbox_screen_write(7,one,4)==-1&&!carbox_screen_sending);
 {struct pollfd p={7,POLLOUT,0};carbox_screen_sending=1;poll_result=1;poll_flags=POLLHUP;
  assert(carbox_screen_poll(&p,1,250)==1&&!carbox_screen_sending);poll_result=0;poll_flags=0;}
 carbox_screen_sending=1;write_result=2;writes=0;AirPlayScreen_SendScreenData((char*)one,10);assert(writes==5);
 carbox_screen_sending=0;info[1732]=0;AirPlayScreen_StopVideo();assert(closed==1&&carbox_screen_sock==-1);
 AirPlayScreen_StartVideo(800,480);assert(starts==1&&creates==1);
 /* 未退出的旧线程禁止关闭/重用 fd；新 start 必须拒绝。 */
 carbox_screen_sending=0;exit_mode=0;AirPlayScreen_StartVideo(800,480);assert(starts==1&&closed==1);
 exit_mode=1;AirPlayScreen_StopVideo();assert(joins==1&&closed==2);
 exit_mode=0;AirPlayScreen_StartVideo(800,480);assert(starts==2);
 carbox_screen_frames=NULL;carbox_screen_thread_exit(NULL);AirPlayScreen_StopVideo();assert(joins==2&&closed==3);
 /* 清理幂等。 */
 AirPlayScreen_StopVideo();assert(closed==3);
 carbox_screen_setup_report(1);
 puts("screen session/NAL/info behavior PASS");return 0;
}
