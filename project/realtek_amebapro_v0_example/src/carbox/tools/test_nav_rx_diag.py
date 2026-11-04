#!/usr/bin/env python3
"""Exercise real app RX collector with pinned ARM call-frame layout and stubs."""
from pathlib import Path
import subprocess
import tempfile
import re
source=Path(__file__).resolve().parents[1]/'audio_nav_rx_diag.c'
for fmt in re.findall(r'carbox_audio_diag_log\("([^"\n]+)"',source.read_text()):
    maximum=fmt.replace('%lu','4294967295').replace('%ld','-2147483648').replace('\\n','\n')
    assert len(maximum)<160, maximum
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
static unsigned critical,prints,registered,unregistered;static uint32_t clock_us=10,next_sid=1;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() (--critical)
#define LWIP_IPV6 1
#define CARBOX_RX_HANDOVER_DIAG_H
#define CONFIG_AUDIO_DIAG_LOG 1
#define carbox_rxhd_log log_printf
#define lwip_ntohs ntohs
uint32_t hal_read_curtime_us(void){return clock_us;}
uint32_t carbox_diag_log_skipped(void){return 0;}
uint32_t carbox_nav_session_id(void *ctx){assert(ctx);return next_sid;}
void carbox_nav_udp_register(uint16_t p,uint32_t s){assert(!critical && p==62599 && s);registered++;}
void carbox_nav_udp_unregister(uint16_t p){assert(!critical && p==62599);unregistered++;}
int lwip_getsockname(int fd,struct sockaddr *address,socklen_t *len){
 assert(!critical && fd==36 && *len>=sizeof(struct sockaddr_in));
 struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons(62599);memcpy(address,&a,sizeof(a));return 0;
}
static int log_printf(const char *f,...){char b[768];va_list a;va_start(a,f);int n=vsnprintf(b,sizeof(b),f,a);va_end(a);assert(n<160);assert(!critical);prints++;return 0;}
#include "SOURCE"
static uint32_t apps[6][400],node[400],regs[40],length,auth_error;
static uint32_t ptr(void *p){assert((uintptr_t)p<=UINT32_MAX);return (uint32_t)(uintptr_t)p;}
static void start(unsigned i){apps[i][7]=36;carbox_nav_app_before(apps[i],1,regs);}
static void rx(unsigned i,unsigned seq,unsigned len,int rc){
 uint8_t *p=(uint8_t *)node+8;memset(p,0,100);p[0]=0x80;p[2]=seq>>8;p[3]=seq;p[7]=0xaa;
 regs[1]=ptr(p);regs[2]=1472;regs[3]=ptr(&length);length=len;
 carbox_nav_app_before(apps[i],2,regs);clock_us+=10;regs[0]=(uint32_t)rc;
 carbox_nav_app_after(apps[i],2,regs);
}
static void free_node(unsigned i){carbox_nav_app_before(apps[i],4,regs);}
static void queued(unsigned i,int error){regs[0]=(uint32_t)error;carbox_nav_app_after(apps[i],5,regs);}
static void verify(unsigned i,unsigned decrypted,unsigned returned,int error,unsigned payload){
 auth_error=(uint32_t)error;node[0x5c8/4]=payload;regs[3]=ptr(&auth_error);
 regs[26]=ptr(node);regs[27]=decrypted;
 uint32_t copy[40];memcpy(copy,regs,sizeof(copy));
 carbox_nav_app_before(apps[i],3,regs);assert(!memcmp(copy,regs,sizeof(copy)));
 regs[0]=returned;copy[0]=returned;carbox_nav_app_after(apps[i],3,regs);
 assert(!memcmp(copy,regs,sizeof(copy)));
}
int main(void){
 start(0);struct app_rx_slot *s=&app_slots[0];assert(s->sid==1 && registered==1);
 rx(0,65534,80,0);queued(0,0);rx(0,1,80,0);queued(0,0);
 rx(0,0,80,0);queued(0,0);rx(0,1,80,0);queued(0,0);rx(0,2,80,0);queued(0,0);
 assert(s->missing==2 && s->reorder==1 && s->duplicate==1 && s->high_seq==2);
 assert(s->queued==5 && s->received==5 && prints==0);
 // Nonzero receive status must not read stale output length/buffer.
 regs[1]=0;regs[3]=0;carbox_nav_app_before(apps[0],2,regs);regs[0]=-11;
 carbox_nav_app_after(apps[0],2,regs);free_node(0);free_node(0);
 assert(s->recv_errors==1 && s->discarded[APP_RECV_ERROR]==1);
 rx(0,3,11,0);free_node(0);assert(s->discarded[APP_SHORT]==1);
 ((uint8_t *)apps[0])[1432]=1;
 rx(0,3,35,0);free_node(0);assert(s->discarded[APP_CRYPTO_SHORT]==1);
 rx(0,4,80,0);verify(0,40,4,-7,68);free_node(0);
 assert(s->auth_errors==1 && s->discarded[APP_AUTH]==1 && s->last_error==-7);
 rx(0,5,80,0);verify(0,40,3,0,68);free_node(0);
 assert(s->length_errors==1 && s->discarded[APP_LENGTH]==1);
 rx(0,6,80,0);verify(0,40,4,0,68);queued(0,-42);free_node(0);
 assert(s->queue_errors==1 && s->discarded[APP_QUEUE]==1 && s->last_discard_seq==6);
 rx(0,7,80,0);verify(0,40,4,0,68);queued(0,0);free_node(0);
 assert(s->queued==6 && s->discarded[APP_OTHER]==0);
 // Successful oversized output must not read out of bounds (null data pointer).
 regs[1]=0;regs[2]=1472;regs[3]=ptr(&length);length=1600;
 carbox_nav_app_before(apps[0],2,regs);regs[0]=0;carbox_nav_app_after(apps[0],2,regs);free_node(0);
 assert(s->oversize==1 && s->discarded[APP_OTHER]==1);
 // Header with unsupported RTP version cannot corrupt sequence evidence.
 regs[1]=ptr((uint8_t *)node+8);length=80;((uint8_t *)node)[8]=0;
 carbox_nav_app_before(apps[0],2,regs);carbox_nav_app_after(apps[0],2,regs);queued(0,0);
 assert(s->invalid==1 && s->high_seq==7);
 clock_us=0xfffffff0;carbox_nav_app_before(apps[0],7,regs);clock_us=20;regs[0]=-1;
 carbox_nav_app_after(apps[0],7,regs);assert(s->select_max==36 && s->select_errors==1);
 assert(!critical && !prints);clock_us+=5000000;carbox_nav_rx_report();assert(prints==7);
 // Closed session remains until report even when same app address is reused.
 carbox_nav_app_before(apps[0],6,regs);assert(unregistered==1);next_sid++;start(0);
 assert(!app_slots[0].active && app_slots[0].dirty && app_slots[1].active);
 carbox_nav_rx_report();assert(!app_slots[0].dirty && app_slots[1].sid==2);
 // No live slot eviction; capture overflow evidence without corrupting state.
 for(unsigned i=1;i<4;i++){next_sid++;start(i);}
 next_sid++;unsigned reg_before=registered;start(4);assert(app_lost_slots==1 && registered==reg_before);
 unsigned printed=prints;carbox_nav_rx_report();assert(prints==printed+1);
 // Ignore unregistered/non-navigation formats.
 next_sid=0;start(5);assert(app_lost_slots==1);
 assert(!critical);puts("PASS: raw seq wrap/reorder, validation/free reasons, bounded reads, caller stack ABI, deferred reporting and lifecycle");
}
'''.replace('SOURCE',str(source))
with tempfile.TemporaryDirectory() as temp:
    d=Path(temp)
    for name in ['FreeRTOS.h','task.h','hal_timer.h']:(d/name).write_text('')
    (d/'lwip').mkdir();(d/'lwip/sockets.h').write_text('')
    (d/'test.c').write_text(harness)
    subprocess.run(['gcc','-std=c99','-O2','-Wall','-Wextra','-Werror','-no-pie','-I'+str(d),str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
