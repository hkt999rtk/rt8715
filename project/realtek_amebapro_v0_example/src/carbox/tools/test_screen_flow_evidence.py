#!/usr/bin/env python3
"""Observer must preserve timestamps, payload, return and errno, without I/O in locks."""
from pathlib import Path
import subprocess,tempfile
src=Path(__file__).resolve().parents[1]/'screen_flow_evidence.c'
with tempfile.TemporaryDirectory() as td:
 d=Path(td);(d/'lwip').mkdir()
 (d/'FreeRTOS.h').write_text('extern unsigned critical;\nvoid enter_critical(void);\n#define taskENTER_CRITICAL() enter_critical()\n#define taskEXIT_CRITICAL() (--critical)\n')
 (d/'task.h').write_text('typedef void *TaskHandle_t;\nTaskHandle_t xTaskGetCurrentTaskHandle(void);\nconst char *pcTaskGetName(TaskHandle_t);\n')
 (d/'hal_timer.h').write_text('#include <stdint.h>\nuint32_t hal_read_curtime_us(void);\n')
 (d/'diag.h').write_text('int rt_printf(const char *,...);\n')
 (d/'lwip/sockets.h').write_text('#include <sys/socket.h>\nint lwip_sendto(int,const void*,size_t,int,const struct sockaddr*,socklen_t);\n')
 (d/'lwip/errno.h').write_text('extern int lwip_err;\n')
 h=r"""
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
unsigned critical;int lwip_err;
#include "SOURCE"
static uint32_t now;static uint64_t stamp;static int calls,rc,inject;
static unsigned long reported_age;
static char output[32768];
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (void *)1;}
const char *pcTaskGetName(TaskHandle_t t){(void)t;return "Accessory";}
void enter_critical(void){if(inject){now+=100;flow.video.at=now;inject=0;}critical++;}
uint32_t hal_read_curtime_us(void){return now;}
uint64_t AirPlayClock_VideoTimestamp(void){assert(!critical);calls++;return stamp;}
void AirPlayClock_GetTime(struct timeval*t){assert(!critical);calls++;t->tv_sec=9;t->tv_usec=123;}
void AirPlayClock_StartTime(struct timeval*t){AirPlayClock_GetTime(t);}
int lwip_sendto(int fd,const void*b,size_t n,int f,const struct sockaddr*a,socklen_t z){(void)fd;(void)b;(void)n;(void)f;(void)a;(void)z;assert(!critical);calls++;lwip_err=77;return rc;}
int rt_printf(const char*f,...){char out[1024];va_list ap;assert(!critical);va_start(ap,f);vsnprintf(out,sizeof out,f,ap);va_end(ap);strncat(output,out,sizeof(output)-strlen(output)-1);char*p=strstr(out,"video_age_ms=");if(p)reported_age=strtoul(p+13,NULL,10);return 0;}
int main(void){
 unsigned char sps[]={0,0,0,1,0x67,0,0,0,1,0x68};
 unsigned char shortbuf[]={0,0,1};struct timeval tv;
 carbox_screen_flow_frame(0,sps,sizeof sps);carbox_screen_flow_frame(1,sps,sizeof sps);
 carbox_screen_flow_frame(0,sps,sizeof sps);carbox_screen_flow_frame(2,sps,sizeof sps);
 carbox_screen_flow_frame(0,shortbuf,sizeof shortbuf);carbox_screen_flow_frame(0,NULL,100);
 assert(flow.frames[0]==4 && flow.sps[0]==2 && flow.sps[1]==1 && flow.sps[2]==1);
 stamp=0x1234567812345678ULL;assert(carbox_screen_time_config()==stamp && calls==1);
 assert(carbox_screen_time_video()==stamp && calls==2);
 stamp++;assert(carbox_screen_time_video()==stamp && calls==3);
 stamp-=2;assert(carbox_screen_time_video()==stamp && calls==4);
 assert(flow.video.back==1 && flow.video.count==3 && flow.config.count==1);
 carbox_screen_time_start(&tv);assert(calls==5 && tv.tv_sec==9 && tv.tv_usec==123);
 carbox_screen_time_receive(&tv);carbox_screen_time_transmit(&tv);assert(calls==7);
 unsigned char pkt[32]={0x80,0xd3,0,7},copy[32];memcpy(copy,pkt,32);
 rc=32;assert(carbox_screen_timing_sendto(7,pkt,32,0,NULL,0)==32 && lwip_err==77);
 rc=-1;assert(carbox_screen_timing_sendto(7,pkt,32,0,NULL,0)==-1 && lwip_err==77);
 assert(!memcmp(pkt,copy,32) && flow.send_ok==1 && flow.send_error==1 && calls==9);
 now=0xfffffff0U;carbox_screen_time_video();now=0x10;
 carbox_screen_flow_report(1);assert(!critical);
 now=1000;flow.video.at=now;inject=1;carbox_screen_flow_report(2);assert(reported_age==0);
 /* 两组完整参数不同，手机参数被略过，IDR仍转送。观测不得改payload。 */
 unsigned char local[]={0,0,0,1,0x67,0x64,0,0x1f,0x80,0,0,1,0x68,0xc0,0,0,1,0x65,0xb8};
 unsigned char phone[]={0,0,0,1,0x67,0x42,0,0x1e,0x80,0,0,1,0x68,0x50};
 unsigned char idr[]={0,0,1,0x65,0xb4};unsigned char saved[sizeof phone];
 memcpy(saved,phone,sizeof phone);
 carbox_screen_flow_frame(0,local,sizeof local);carbox_screen_flow_frame(1,local,sizeof local);
 carbox_screen_flow_frame(0,phone,sizeof phone);carbox_screen_flow_frame(2,phone,sizeof phone);
 carbox_screen_flow_frame(0,idr,sizeof idr);carbox_screen_flow_frame(1,idr,sizeof idr);
 carbox_screen_flow_report(3);carbox_screen_flow_report(4);carbox_screen_flow_report(5);
 assert(!memcmp(saved,phone,sizeof phone));
 assert(strstr(output,"[SCREEN_NAL]"));
 assert(strstr(output,"stage=unforwarded"));
 assert(strstr(output,"pps_id=1"));
 assert(strstr(output,"forward_equal=0"));
 assert(strstr(output,"hex=6742001e80"));
 /* 空输入、短起始码、截断UE、超长参数、队列耗尽均不能越界或改变业务。 */
 unsigned char *large=malloc(5000);memset(large,0xff,5000);
 large[0]=large[1]=large[2]=0;large[3]=1;large[4]=0x67;
 for(unsigned k=0;k<20;k++)carbox_screen_flow_frame(0,large,k);
 carbox_screen_flow_frame(0,large,5000);
 output[0]=0;
 for(unsigned k=0;k<50;k++)carbox_screen_flow_frame(0,phone,sizeof phone);
 for(unsigned k=0;k<8;k++)carbox_screen_flow_report(6+k);
 free(large);
 assert(strstr(output,"[SCREEN_NAL_LIMIT]"));
 char *loss=strstr(output,"lost=");assert(loss && strtoul(loss+5,NULL,10)>0);
 char *limit=strstr(output,"scan_limited=");assert(limit && strtoul(limit+13,NULL,10)>0);
 assert(strstr(output,"previous_equal=1"));
 return 0;
}
""".replace('SOURCE',str(src))
 (d/'test.c').write_text(h)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+td,str(d/'test.c'),str(src.with_name('screen_nal_evidence.c')),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
 print('screen flow transparent observer PASS')
