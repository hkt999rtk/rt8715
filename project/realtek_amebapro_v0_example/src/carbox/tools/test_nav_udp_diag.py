#!/usr/bin/env python3
"""Exercise real UDP callback branches and diagnostic state with mock ownership."""
from pathlib import Path
import subprocess
import tempfile
carbox = Path(__file__).resolve().parents[1]
root = carbox.parents[3]
api = root / 'component/common/network/lwip/lwip_v2.1.2/src/api/api_msg.c'
s = api.read_text()
callback = s[s.index('static void\nrecv_udp('):s.index('#endif /* LWIP_UDP */', s.index('static void\nrecv_udp('))]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
static unsigned critical,prints,frees,posts,events; static uint32_t clock_us=100;
static int no_mem,mbox_full;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() (--critical)
uint32_t hal_read_curtime_us(void) {return clock_us;}
#define CARBOX_RX_HANDOVER_DIAG_H
#define CONFIG_AUDIO_DIAG_LOG 1
static void test_log(const char *f,...) {
 char line[768];va_list ap;assert(!critical);va_start(ap,f);
 int n=vsnprintf(line,sizeof(line),f,ap);va_end(ap);
 assert(n>0 && n+1<=160); /* logger substitutes LF with CRLF */
 const char *p=strchr(line,' ');assert(p && strchr(p+1,' '));
 prints++;
}
#define carbox_rxhd_log test_log
#include "DIAG_SOURCE"
typedef uint16_t u16_t;
typedef unsigned ip_addr_t;
struct udp_pcb {uint16_t local_port;};
struct netbuf {struct pbuf *p,*ptr;ip_addr_t addr;uint16_t port;};
struct netconn {union {struct udp_pcb *udp;} pcb;int recvmbox,recv_avail,recv_bufsize;};
#define LWIP_UNUSED_ARG(x) (void)(x)
#define LWIP_ASSERT(s,x) ((void)0)
#define SYS_ARCH_GET(x,y) ((y)=(x))
#define SYS_ARCH_INC(x,y) ((x)+=(y))
#define NETCONN_MBOX_VALID(c,m) (*(m))
#define MEMP_NETBUF 0
#define ERR_OK 0
#define NETCONN_EVT_RCVPLUS 0
#define API_EVENT(c,e,l) (++events)
static struct netbuf bufmem;
static void pbuf_free(struct pbuf*p) {assert(p);frees++;}
static void *memp_malloc(int x) {(void)x;return no_mem?NULL:&bufmem;}
static void ip_addr_set(ip_addr_t *a,const ip_addr_t*b) {*a=*b;}
static int sys_mbox_trypost(int*m,void*b) {(void)m;(void)b;if(mbox_full)return -1;posts++;return 0;}
static void netbuf_delete(struct netbuf*b) {pbuf_free(b->p);}
CALLBACK
static uint8_t bytes[12]={0x80,0x60,0xff,0xfe};
static struct pbuf tail={NULL,bytes+5,7,7}, head={&tail,bytes,12,5};
static void seq(uint16_t n) {bytes[2]=n>>8;bytes[3]=n;clock_us+=1000;}
int main(void) {
 struct udp_pcb pcb={60000};struct netconn conn={{&pcb},1,0,100000};ip_addr_t addr=0;
#if CONFIG_AUDIO_NAV_DIAG
 carbox_nav_udp_register(60000,7);
#endif
 recv_udp(&conn,&pcb,&head,&addr,1234);assert(posts==1 && !frees && events==1);
#if CONFIG_AUDIO_NAV_DIAG
 assert(udp_slots[0].seen==1 && udp_slots[0].rtp==1 && udp_slots[0].first_seq==65534);
#endif
 seq(0);recv_udp(&conn,&pcb,&head,&addr,1234);
 seq(65535);recv_udp(&conn,&pcb,&head,&addr,1234);
 seq(1);recv_udp(&conn,&pcb,&head,&addr,1234);
#if CONFIG_AUDIO_NAV_DIAG
 assert(udp_slots[0].missing==1 && udp_slots[0].reorder==1 && udp_slots[0].last_seq==1);
#endif
 recv_udp(NULL,&pcb,&head,&addr,1234);assert(frees==1);
 conn.recvmbox=0;recv_udp(&conn,&pcb,&head,&addr,1234);assert(frees==2);conn.recvmbox=1;
#if LWIP_SO_RCVBUF
 conn.recv_bufsize=1;recv_udp(&conn,&pcb,&head,&addr,1234);conn.recv_bufsize=100000;
#endif
 no_mem=1;recv_udp(&conn,&pcb,&head,&addr,1234);no_mem=0;
 mbox_full=1;recv_udp(&conn,&pcb,&head,&addr,1234);mbox_full=0;
 assert(posts==4 && events==4 && frees==4+LWIP_SO_RCVBUF);
#if CONFIG_AUDIO_NAV_DIAG
 for(unsigned i=NAV_UDP_NO_CONN;i<NAV_UDP_RESULT_COUNT;i++)
   assert(udp_slots[0].result[i]==(i==NAV_UDP_RCVBUF_FULL?LWIP_SO_RCVBUF:1));
 assert(udp_slots[0].result[NAV_UDP_ACCEPTED]==4 && !prints);
 /* Other ports excluded; short/version-invalid headers counted without parsing. */
 pcb.local_port=60001;recv_udp(&conn,&pcb,&head,&addr,1234);pcb.local_port=60000;
 assert(udp_slots[0].seen==8+LWIP_SO_RCVBUF);
 head.tot_len=4;recv_udp(&conn,&pcb,&head,&addr,1234);head.tot_len=12;
 bytes[0]=0;recv_udp(&conn,&pcb,&head,&addr,1234);bytes[0]=0x80;
 assert(udp_slots[0].invalid_rtp==2);
 unsigned token=udp_slots[0].token;
 carbox_nav_udp_register(60000,7);assert(udp_slots[0].token==token);
 carbox_nav_udp_report();assert(!prints);clock_us+=5000000;
 carbox_nav_udp_report();assert(prints==4);
 carbox_nav_udp_unregister(60000);carbox_nav_udp_report();assert(prints==8);
 carbox_nav_udp_register(60000,8);assert(udp_slots[0].session==8 && !udp_slots[0].seen);
 carbox_nav_udp_finish(token,NAV_UDP_ACCEPTED);assert(udp_lost==1 && !udp_slots[0].result[NAV_UDP_ACCEPTED]);
 /* UINT32_MAX counters must fit complete atomic UART lines. */
 memset(&udp_slots[0],0xff,sizeof(udp_slots[0]));udp_slots[0].active=0;
 carbox_nav_udp_report();
 assert(!critical);
#endif
 puts("PASS UDP callback: accept/drop ownership, both RCVBUF modes, diagnostic enabled/disabled");
}
'''.replace('DIAG_SOURCE', str(carbox/'nav_udp_diag.c')).replace('CALLBACK',callback)
pbuf = r'''
#ifndef TEST_PBUF_H
#define TEST_PBUF_H
#include <stdint.h>
#include <string.h>
struct pbuf {struct pbuf *next;void *payload;uint16_t tot_len,len;};
static uint16_t pbuf_copy_partial(const struct pbuf*p,void*out,uint16_t n,uint16_t off) {
 uint16_t copied=0;
 for(;p && copied<n;p=p->next) {
  if(off>=p->len){off-=p->len;continue;}
  uint16_t k=p->len-off;if(k>n-copied)k=n-copied;
  memcpy((uint8_t*)out+copied,(uint8_t*)p->payload+off,k);copied+=k;off=0;
 }
 return copied;
}
#endif
'''
with tempfile.TemporaryDirectory() as tmp:
 d=Path(tmp);(d/'lwip').mkdir()
 for n in ['FreeRTOS.h','task.h','hal_timer.h']:(d/n).write_text('')
 (d/'lwip/pbuf.h').write_text(pbuf)
 # Disabled diagnostic does not itself include pbuf; the callback still needs it.
 (d/'test.c').write_text('#include "lwip/pbuf.h"\n'+harness)
 for enabled in (0,1):
  for rcvbuf in (0,1):
   subprocess.run(['gcc','-std=c99','-O2','-I'+str(d),'-I'+str(carbox),f'-DCONFIG_AUDIO_NAV_DIAG={enabled}',f'-DLWIP_SO_RCVBUF={rcvbuf}','-DLWIP_NETBUF_RECVINFO=0',str(d/'test.c'),'-o',str(d/'test')],check=True)
   subprocess.run([str(d/'test')],check=True)
