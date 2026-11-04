#!/usr/bin/env python3
"""Exercise production trace lifecycle, ordering, wrap, overload and FDK context."""
from pathlib import Path
import subprocess, tempfile
src=Path(__file__).resolve().parents[1]
h=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#define CONFIG_AUDIO_NAV_TX_TRACE 1
#define NAVTX_HOST_TEST 1
#define CONFIG_NCM_TX_VENDOR_ASYNC 0
static uint32_t now;static uintptr_t task=1;static unsigned critical;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() (--critical)
uintptr_t xTaskGetCurrentTaskHandle(void){return task;}
uint32_t hal_read_curtime_us(void){return now;}
const char *pcTaskGetName(void *unused){return "mock-core-owner";}
void carbox_rxhd_log(const char *f,...){assert(!critical);}
#include "nav_tx_trace.c"
static uint8_t data[1452]={0x80,0x60,0x12,0x34,0,0,1,0};
static int pbuf;
static uint32_t begin(void){return navtx_begin(7,data,sizeof(data),32500,10000);}
static void enqueue(uint32_t id){
 navtx_prepared(&pbuf);navtx_core_wait();now+=40000;navtx_core_acquired(0x123);
 assert(!navtx_ncm(data,0));assert(navtx_ncm(&pbuf,5)==id);navtx_core_release();
}
int main(void){
 now=UINT32_MAX-100;task=2;navtx_core_acquired(0x987);now+=50;task=1;
 uint32_t id=begin();enqueue(id);assert(record(id)->owner==2 && record(id)->owner_age==50);
 now+=20;navtx_end(id,1452,0);assert(record(id));
 task=3;navtx_mark(id,NT_DEQUEUE);now+=10;navtx_mark(id,NT_BUILD);now+=10;
 navtx_mark(id,NT_READY);now+=10;navtx_mark(id,NT_USB);now+=500;
 navtx_transport_end(id,0);assert(!record(id) && st.complete==1);
 assert(metrics[2].max==40000 && events[0].seq==0x1234 && events[0].ts==256);
 // HAL can finish before sendto returns; no premature recycle.
 task=1;id=begin();enqueue(id);task=3;navtx_transport_end(id,0);assert(record(id));
 task=1;navtx_end(id,1452,0);assert(!record(id) && st.complete==2);
 // Input queue full is an explicit failure, still await sender return.
 id=begin();enqueue(id);navtx_transport_end(id,-2);navtx_end(id,-1,12);assert(st.failed==1);
 // No NCM match is unobserved, not a fabricated USB success.
 id=begin();navtx_end(id,1452,0);assert(st.no_ncm==1);
 // Post-expiry hooks cannot mutate a newer generation in the same slot.
 id=begin();enqueue(id);now+=6000000;navtx_report();assert(!record(id) && st.expired==1);
 navtx_end(id,1452,0);assert(st.stale==1);
 uint32_t newer=begin();assert(newer);navtx_transport_end(id,0);assert(record(newer));
 navtx_end(newer,1452,0);
 // Bounded context pool and record pool; no blocking or allocation on pressure.
 for(unsigned i=1;i<=8;i++){task=i;assert(begin());}
 task=9;assert(!begin() && st.alloc_lost==1);
 for(unsigned i=1;i<=8;i++){task=i;struct nt_context *c=context(task,0);navtx_end(c->id,1452,0);}
 task=1;
 for(unsigned i=0;i<NT_SLOTS;i++){id=begin();assert(id);enqueue(id);navtx_end(id,1452,0);}
 assert(!begin() && st.alloc_lost==2);
 for(unsigned i=0;i<NT_SLOTS;i++){id=records[i].id;navtx_transport_end(id,0);}
 assert(st.detail_lost>0);
 // Actual concealment transitions correlate to decoder session/RTP timestamp.
 navtx_decode_context(11,480,1);navtx_fdk_state(0,1,0,480);
 navtx_decode_context(11,960,0);navtx_fdk_state(1,2,1,480);
 navtx_fdk_state(2,0,1,480);navtx_decode_context(0,0,0);
 navtx_fdk_state(0,4,0,480);assert(st.fdk_calls==3 && st.fdk_transitions==3);
 assert(fdk_events[0].sid==11 && fdk_events[0].ts==480 && fdk_events[0].plc==1);
 assert(fdk_events[1].plc==0 && fdk_events[1].after==2);
 now+=6000000;navtx_report();assert(!critical);
 puts("PASS nav TX: ordering, exact pbuf, lock owner, clock wrap, expiry, overload, FDK correlation");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=undefined','-I',str(src),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
