#!/usr/bin/env python3
"""Exercise production vendor adapters with partial NTBs, queue failure and reuse."""
from pathlib import Path
import subprocess,tempfile
src=Path(__file__).resolve().parents[1]
h=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_AUDIO_NAV_TX_TRACE 1
#define CONFIG_NCM_TX_VENDOR_ASYNC 1
#define NAVTX_HOST_TEST 1
#define pdPASS 1
typedef void *QueueHandle_t;typedef int BaseType_t;typedef uint32_t TickType_t;typedef uint8_t u8_t;
struct pbuf {void *payload;};
static uint32_t now;static uintptr_t task=1;static unsigned critical;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() (--critical)
uintptr_t xTaskGetCurrentTaskHandle(void){return task;}
uint32_t hal_read_curtime_us(void){return now;}
const char *pcTaskGetName(void *p){return "test";}
void carbox_rxhd_log(const char *f,...){assert(!critical);}
BaseType_t xQueueGenericSend(QueueHandle_t,const void*,TickType_t,BaseType_t);
BaseType_t xQueueReceive(QueueHandle_t,void*,TickType_t);
uint32_t uxQueueMessagesWaiting(QueueHandle_t q){return 0;}
u8_t pbuf_free(struct pbuf *p);
#include "nav_tx_trace.c"
QueueHandle_t navtx_vendor_input_queue=(void*)1,navtx_vendor_ready_queue=(void*)2;
static struct nt_vendor_item fifo[8];static unsigned head,tail;
static int queue_fail,build_fail,usb_rc,freed,usb_calls;
static uint16_t build_limit=2;
BaseType_t xQueueGenericSend(QueueHandle_t q,const void *p,TickType_t t,BaseType_t pos){
 assert(!critical);assert(pos==0);if(queue_fail)return 0;
 if(q==navtx_vendor_input_queue)fifo[head++%8]=*(const struct nt_vendor_item*)p;
 return 1;
}
BaseType_t xQueueReceive(QueueHandle_t q,void *p,TickType_t t){
 assert(!critical);if(q==navtx_vendor_input_queue && head!=tail){*(struct nt_vendor_item*)p=fifo[tail++%8];return 1;}
 return 0;
}
u8_t pbuf_free(struct pbuf *p){assert(!critical);freed++;return 1;}
int carbox_ncm_tx_build_batch(const void *batch,void *buf,size_t cap,size_t *len,uint16_t *sent){
 const struct nt_vendor_batch*b=batch;assert(!critical);
 *sent=build_fail?0:(b->frames>build_limit?build_limit:b->frames);*len=100;now+=100;return build_fail?-1:0;
}
int carbox_ncm_tx_send_prebuilt(void *buf,size_t len){assert(!critical);usb_calls++;now+=10000;return usb_rc;}
static unsigned char rtp[1452]={0x80,0x60,0,1};
static struct pbuf p[3];static char payload[3][1600],ntb[2][16384];
static uint32_t queue(unsigned i){
 task=1;p[i].payload=payload[i];rtp[3]=i;
 uint32_t id=navtx_begin(2,rtp,sizeof(rtp),0,0);assert(id);
 navtx_prepared(&p[i]);struct nt_vendor_item item={.p=&p[i]};
 int rc=navtx_vendor_send(navtx_vendor_input_queue,&item,0,0);
 navtx_end(id,rc==1?1452:-1,rc==1?0:12);return id;
}
int main(void){
 uint32_t ids[3];for(unsigned i=0;i<3;i++)ids[i]=queue(i);
 task=2;struct nt_vendor_item item;
 for(unsigned i=0;i<3;i++){assert(navtx_vendor_receive(navtx_vendor_input_queue,&item,0)==1);assert(record(ids[i])->mask&(1U<<NT_DEQUEUE));}
 struct nt_vendor_batch b={.magic=0x324d434e,.frames=3,.segments=3};
 for(unsigned i=0;i<3;i++){b.frame[i].first_segment=i;b.frame[i].segment_count=1;b.segment[i].data=payload[i];}
 size_t len;uint16_t sent;now=100;
 assert(navtx_vendor_build(&b,ntb[0],sizeof(ntb[0]),&len,&sent)==0 && sent==2);
 assert(record(ids[0])->ntb==ntb[0] && record(ids[1])->ntb==ntb[0] && !record(ids[2])->ntb);
 assert(navtx_vendor_free(&p[0])==1 && navtx_vendor_free(&p[1])==1);
 assert(!record(ids[0])->pbuf && !record(ids[0])->payload);
 void *buffer=ntb[0];navtx_vendor_send(navtx_vendor_ready_queue,&buffer,0,0);
 b.frames=b.segments=1;b.frame[0].first_segment=0;b.segment[0].data=payload[2];now=500;
 navtx_vendor_build(&b,ntb[1],sizeof(ntb[1]),&len,&sent);navtx_vendor_free(&p[2]);
 assert(record(ids[2])->at[NT_BUILD]==500);
 buffer=ntb[1];navtx_vendor_send(navtx_vendor_ready_queue,&buffer,0,0);
 task=3;assert(!navtx_vendor_usb(ntb[0],100));assert(!record(ids[0]) && !record(ids[1]) && record(ids[2]));
 usb_rc=-4;assert(navtx_vendor_usb(ntb[1],100)==-4 && !record(ids[2]));assert(st.complete==3 && st.failed==1 && st.pending==0);
 // Reused source pbuf/NTB must not inherit previous packet handles.
 usb_rc=0;queue_fail=1;uint32_t id=queue(0);assert(!record(id) && st.failed==2);queue_fail=0;
 id=queue(0);task=2;navtx_vendor_receive(navtx_vendor_input_queue,&item,0);
 b.segment[0].data=payload[0];build_fail=1;
 navtx_vendor_build(&b,ntb[0],sizeof(ntb[0]),&len,&sent);navtx_vendor_free(&p[0]);
 assert(!record(id) && st.failed==3 && st.pending==0);
 // Timer queue items are not pbufs, even if their other fields contain garbage.
 memset(&item,0xff,sizeof(item));item.timer=1;
 navtx_vendor_send(navtx_vendor_input_queue,&item,0,0);navtx_vendor_receive(navtx_vendor_input_queue,&item,0);
 assert(!critical && usb_calls==2 && freed==4);
 puts("PASS vendor adapter: partial prefix, source release, per-NTB ids, queue/build/HAL failure, reuse, timer items");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=undefined','-I',str(src),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
