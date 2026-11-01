#!/usr/bin/env python3
"""Exercise production frame lifecycle hooks; no driver/hardware simulation claims."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[5]
source = root / 'project/realtek_amebapro_v0_example/src/carbox/wlan_recvframe_trace.c'
stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#define CONFIG_RX_HANDOVER_DIAG 1
#define configTICK_RATE_HZ 1000
static unsigned critical, inserts, deletes, frees, indications;
static uint32_t tick;
static uintptr_t fake_pc;
static uintptr_t fake_free_pc;
static unsigned consume_entry;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() do { assert(critical); --critical; } while(0)
#define __builtin_return_address(n) ((void *)fake_pc)
static uint32_t xTaskGetTickCount(void) { return tick; }
static void *xTaskGetCurrentTaskHandle(void) { return (void *)0x1234; }
/* Reserve distinct PC ranges like the native functions. Tiny adjacent host
 * stubs can otherwise make unrelated function+offset sites collide. */
#define NATIVE_FN __attribute__((aligned(1024)))
NATIVE_FN void _rtw_init_recv_priv(void) {}
NATIVE_FN void _rtw_free_recv_priv(void) {}
NATIVE_FN void _rtw_alloc_recvframe(void) {}
NATIVE_FN void rtw_free_recvframe(void) {}
NATIVE_FN void enqueue_reorder_recvframe(void) {}
NATIVE_FN void recv_indicatepkts_in_order(void) {}
NATIVE_FN void recvframe_chk_defrag(void) {}
NATIVE_FN void _rtw_enqueue_recvframe(void) {}
NATIVE_FN void amsdu_to_msdu(void) {}
void __real_rtw_list_insert_tail(void *p, void *q) { assert(p && q && !critical); inserts++; }
void __real_rtw_list_delete(void *p) { assert(p && !critical); deletes++; }
void __real_rtw_vmfree(void *p, uint32_t n) { assert(p && n==6400 && !critical); frees++; }
static unsigned char frames[6400] __attribute__((aligned(256)));
static unsigned char extra_frames[4][6400] __attribute__((aligned(256)));
static unsigned char extra_adapters[4][14000] __attribute__((aligned(4)));
static unsigned char *extra_queues[4] = {
 extra_adapters[0]+0xc44, extra_adapters[1]+0xc44,
 extra_adapters[2]+0xc44, extra_adapters[3]+0xc44
};
static unsigned char adapter[14000] __attribute__((aligned(4))), control[64];
static unsigned char *queue = adapter+0xc44;
static char output[32768];
static unsigned used;
void carbox_rxhd_log(const char *fmt, ...) {
 assert(!critical); va_list ap; va_start(ap,fmt);
 int n=vsnprintf(output+used,sizeof(output)-used,fmt,ap); va_end(ap);
 assert(n<768); /* production bounded formatter must not discard a record */
 assert(n>0 && (unsigned)n<sizeof(output)-used); used+=(unsigned)n;
}
void __wrap_rtw_list_insert_tail(void *,void *);
void __wrap_rtw_list_delete(void *);
void carbox_recvframe_insert(void *,void *,uintptr_t,uintptr_t);
/* Host cannot execute the Thumb shim; final ELF guard validates it separately. */
void __wrap_rtw_list_insert_tail(void *f, void *q) {
 carbox_recvframe_insert(f,q,fake_pc,fake_free_pc);
}
int __real_rtw_recv_entry(void *f) {
 assert(f==frames && !critical);
 if(consume_entry) {
  fake_pc=((uintptr_t)rtw_free_recvframe&~(uintptr_t)1)+0x46;
  __wrap_rtw_list_insert_tail(f,queue);
  fake_pc=((uintptr_t)_rtw_alloc_recvframe&~(uintptr_t)1)+0x14;
  __wrap_rtw_list_delete(f);
 }
 return -7;
}
int __real_rtw_recv_indicatepkt(void *a, void *f) {
 assert(a==adapter && f==frames && !critical); indications++;
 fake_pc=((uintptr_t)rtw_free_recvframe&~(uintptr_t)1)+0x46;
 __wrap_rtw_list_insert_tail(f,queue);
 return 1;
}
'''
test = r'''
static void putptr(void *p, void *v) {
 assert((uintptr_t)v<=UINT32_MAX);
 uint32_t n=(uintptr_t)v; memcpy(p,&n,4);
}
static void insert(void *f, void *q, uintptr_t pc) { fake_pc=pc; __wrap_rtw_list_insert_tail(f,q); }
static void remove_frame(void *f, uintptr_t pc) { fake_pc=pc; __wrap_rtw_list_delete(f); }
#define PC(fn,off) (((uintptr_t)fn&~(uintptr_t)1)+(off))
static void init_pool(void) {
 putptr(queue+0x24,frames); putptr(queue+0x28,frames);
 for(unsigned i=0;i<32;i++) {
  putptr(frames+i*192+16,adapter);
  insert(frames+i*192,queue,PC(_rtw_init_recv_priv,0x66));
 }
}
int main(void) {
 init_pool(); assert(pools[0].active && pools[0].allocs==0);
 tick=100; remove_frame(frames,PC(_rtw_alloc_recvframe,0x14));
 assert(pools[0].frames[0].state==RF_ALLOC && pools[0].allocs==1);
 assert(pools[0].frames[0].borrow_queue==(uintptr_t)queue);
 assert(pools[0].frames[0].alloc_adapter==(uintptr_t)adapter);
 fake_pc=0x2211; assert(__wrap_rtw_recv_entry(frames)==-7);
 assert(pools[0].frames[0].entry_caller==0x2210);
 assert(pools[0].frames[0].entry_adapter==(uintptr_t)adapter);
 putptr(frames+188,control);
 uint16_t seq=12; memcpy(frames+40,&seq,2);
 tick=110; insert(frames,control+12,PC(enqueue_reorder_recvframe,0x36));
 assert(pools[0].frames[0].state==RF_REORDER && pools[0].frames[0].seq==12);
 tick=140; remove_frame(frames,PC(recv_indicatepkts_in_order,0x90));
 assert(pools[0].frames[0].state==RF_DRAIN);
 assert(__wrap_rtw_recv_indicatepkt(adapter,frames)==1);
 assert(pools[0].frames[0].state==RF_FREE && pools[0].returns==1 && indications==1);
 for(unsigned i=0;i<32;i++) {
  tick=200; remove_frame(frames+i*192,PC(_rtw_alloc_recvframe,0x14));
  if(i<31) insert(frames+i*192,control+12,PC(enqueue_reorder_recvframe,0x36));
 }
 tick=2700; carbox_recvframe_trace_report();
 assert(strstr(output,"held=32") && strstr(output,"reorder=31"));
 assert(strstr(output,"oldest_ms=2500") && strstr(output,"live_mask=ffffffff"));
 unsigned details=0; for(char *p=output;(p=strstr(p,"[WLAN_FRAME]"));p++) details++;
 assert(details==4);
 unsigned old_used=used; tick+=2000; carbox_recvframe_trace_report();
 assert(used>old_used && !strstr(output+old_used,"[WLAN_FRAME]"));
 remove_frame(frames,PC(recv_indicatepkts_in_order,0x90));
 tick+=1; insert(frames,queue,PC(rtw_free_recvframe,0x46));
 insert(frames,queue,PC(rtw_free_recvframe,0x46));
 assert(pools[0].double_return==1);
 remove_frame(frames,PC(_rtw_alloc_recvframe,0x14));
 remove_frame(frames,PC(_rtw_alloc_recvframe,0x14));
 assert(pools[0].double_alloc==1);
 unsigned epoch=pools[0].epoch;
 fake_pc=PC(_rtw_free_recv_priv,0x2a); __wrap_rtw_vmfree(frames,6400);
 assert(!pools[0].active && frees==1);
 init_pool(); assert(pools[0].epoch!=epoch && pools[0].allocs==0);
 assert(carbox_recvframe_trace_epoch(queue)==pools[0].epoch);
 assert(carbox_recvframe_trace_epoch(adapter+0xc5c)==0);
 tick=500; remove_frame(frames,PC(_rtw_alloc_recvframe,0x14));
 tick=510; insert(frames,adapter+0xc5c,PC(_rtw_enqueue_recvframe,0x14));
 assert(pools[0].frames[0].state==RF_DECRYPT);
 tick=700; remove_frame(frames,PC(_rtw_alloc_recvframe,0x14));
 assert(pools[0].frames[0].state==RF_DETACHED);
 assert(pools[0].frames[0].loan_tick==500 && pools[0].allocs==1 && pools[0].double_alloc==0);
 frames[42]=2; frames[43]=1;
 insert(frames,control,PC(recvframe_chk_defrag,0xe0));
 assert(pools[0].frames[0].state==RF_DEFRAG && pools[0].frames[0].frag==2);
 assert(pools[0].frames[0].morefrag==1);
 frames[43]=0; insert(frames,control,PC(recvframe_chk_defrag,0x60));
 assert(pools[0].frames[0].state==RF_DEFRAG && pools[0].frames[0].morefrag==0);
 assert(pools[0].frames[0].loan_tick==500);
 observe(frames,(uintptr_t)adapter,PC(amsdu_to_msdu,0x1fe),RF_INDICATE);
 assert(!pools[0].frames[0].attrs && !pools[0].frames[0].frag && !pools[0].frames[0].seq);
 assert(pools[0].amsdu_child==1 && pools[0].amsdu_adapter_mismatch==0);
 /* Fixed child is processed by buddy but returned to its original pool. */
 putptr(frames+16,extra_adapters[0]);
 observe(frames,(uintptr_t)extra_adapters[0],PC(amsdu_to_msdu,0x1fe),RF_INDICATE);
 assert(pools[0].amsdu_child==2 && pools[0].amsdu_adapter_mismatch==0);
 /* Regression signal for the old stale-adapter bug. */
 observe(frames,(uintptr_t)adapter,PC(amsdu_to_msdu,0x1fe),RF_INDICATE);
 assert(pools[0].amsdu_child==3 && pools[0].amsdu_adapter_mismatch==1);
 putptr(frames+16,adapter);
 insert(frames,queue,PC(rtw_free_recvframe,0x46));
 fake_pc=PC(_rtw_free_recv_priv,0x2a); __wrap_rtw_vmfree(frames,6400);
 assert(carbox_recvframe_trace_epoch(queue)==0);
 init_pool();
 tick=UINT32_MAX-10; remove_frame(frames,PC(_rtw_alloc_recvframe,0x14));
 tick=19; assert((uint32_t)(tick-pools[0].frames[0].loan_tick)==30);
 unsigned before=deletes; fake_pc=0;
 __wrap_rtw_list_delete(queue); assert(deletes==before+1 && pools[0].allocs==1);
 for(unsigned i=0;i<4;i++) {
  unsigned char *q=(unsigned char *)extra_queues[i];
  putptr(q+0x24,extra_frames[i]); putptr(q+0x28,extra_frames[i]);
  insert(extra_frames[i],q,PC(_rtw_init_recv_priv,0x66));
  assert((carbox_recvframe_trace_epoch(q)!=0)==(i<3));
 }
 assert(missed_pools==1);
 fake_free_pc=0x4455;
 putptr(frames+16,extra_adapters[0]);
 insert(frames,extra_queues[0],PC(rtw_free_recvframe,0x46));
 assert(pools[0].foreign_return==1 && pools[0].frames[0].state==RF_FREE);
 used=0; output[0]=0; tick+=11000;
 carbox_recvframe_trace_report();
 assert(strstr(output,"[WLAN_FRAME_FLOW]") && strstr(output,"event=foreign_return"));
 assert(strstr(output,"free_pc=0x4454"));
 assert(strstr(output,"[WLAN_POOL_BALANCE]") && strstr(output,"foreign_free=1"));
 assert(strstr(output,"home_free=31") && strstr(output,"foreign_mask=00000001"));
 uint32_t foreign_capacity=62;
 memcpy(extra_queues[0]+0x2c,&foreign_capacity,4);
 used=0; output[0]=0; tick+=11000; carbox_recvframe_trace_report();
 assert(strstr(output,"native_free=62 home_free=32"));
 /* Reborrow from the foreign queue, with the new loan retaining that origin.
  * Native returned metadata must be captured before the frame is consumed. */
 remove_frame(frames,PC(_rtw_alloc_recvframe,0x14));
 assert(pools[0].frames[0].borrow_queue==(uintptr_t)extra_queues[0]);
 assert(pools[0].frames[0].alloc_adapter==(uintptr_t)extra_adapters[0]);
 putptr(frames+16,adapter);
 fake_pc=0x6677; assert(__wrap_rtw_recv_entry(frames)==-7);
 fake_free_pc=0x8899;
 insert(frames,queue,PC(rtw_free_recvframe,0x46));
 used=0; output[0]=0; tick+=11000;
 carbox_recvframe_trace_report();
 assert(strstr(output,"event=foreign_borrow") && strstr(output,"event=home_return"));
 assert(strstr(output,"entry_pc=0x6676") && strstr(output,"free_pc=0x8898"));
 assert(strstr(output,"home_free=32 foreign_free=0"));
 /* The generic native enqueue may also name a registered foreign free pool. */
 remove_frame(frames,PC(_rtw_alloc_recvframe,0x14));
 insert(frames,extra_queues[0],PC(_rtw_enqueue_recvframe,0x14));
 assert(pools[0].frames[0].state==RF_FREE);
 remove_frame(frames,PC(_rtw_alloc_recvframe,0x14));
 assert(pools[0].frames[0].borrow_queue==(uintptr_t)extra_queues[0]);
 insert(frames,queue,PC(rtw_free_recvframe,0x46));
 carbox_recvframe_trace_report(); carbox_recvframe_trace_report();
 /* Unknown destination must stay opaque: never dereference 0xdead0000. */
 remove_frame(frames,PC(_rtw_alloc_recvframe,0x14));
 putptr(frames+16,(void *)0xdead1000);
 insert(frames,(void *)0xdead0000,PC(rtw_free_recvframe,0x46));
 used=0; output[0]=0; carbox_recvframe_trace_report();
 assert(strstr(output,"queue_epoch=0") && strstr(output,"queue_count_at_hook=4294967295"));
 assert(strstr(output,"adapter_type=4294967295 promisc=4294967295"));
 putptr(frames+16,adapter);
 /* Saturate the ring, preserving the first 16 events and a visible lost count.
  * A normal return to home following each foreign borrow is another event. */
 remove_frame(frames,PC(_rtw_alloc_recvframe,0x14));
 insert(frames,queue,PC(rtw_free_recvframe,0x46));
 carbox_recvframe_trace_report();
 for(unsigned i=0;i<20;i++) {
  remove_frame(frames,PC(_rtw_alloc_recvframe,0x14));
  insert(frames,extra_queues[0],PC(rtw_free_recvframe,0x46));
 }
 assert(event_count==16 && event_lost==23);
 used=0; output[0]=0; carbox_recvframe_trace_report();
 details=0; for(char *p=output;(p=strstr(p,"[WLAN_FRAME_FLOW]"));p++) details++;
 assert(details==2 && event_count==14);
 assert(strstr(output,"queued=14 lost=23"));
 /* Pool teardown/reinit must not rewrite the saved event epoch or pointers. */
 unsigned old_epoch=pools[0].epoch;
 fake_pc=PC(_rtw_free_recv_priv,0x2a); __wrap_rtw_vmfree(frames,6400);
 init_pool(); assert(pools[0].epoch!=old_epoch);
 used=0; output[0]=0; tick+=11000; carbox_recvframe_trace_report();
 char expected[64]; snprintf(expected,sizeof(expected),"epoch=%u slot=0",old_epoch);
 assert(strstr(output,expected));
 /* Maximum 32-bit target values must fit the production 768-byte formatter.
  * Set opaque addresses to 32-bit values even on this 64-bit host. */
 struct rf_event *e=&events[event_read];
 e->id=e->tick=e->epoch=e->queue_epoch=e->slot=UINT32_MAX;
 e->kind=RF_FOREIGN_RETURN;
 e->frame=e->home=e->queue=e->free_caller=e->previous_caller=UINT32_MAX;
 e->home_count_at_hook=e->queue_count_at_hook=UINT32_MAX;
 e->adapter=e->buddy=UINT32_MAX;
 e->adapter_type=e->promisc=UINT32_MAX;
 e->loan.borrow_queue=e->loan.alloc_adapter=e->loan.entry_adapter=e->loan.entry_caller=UINT32_MAX;
 e->loan.loan_tick=0; e->loan.attrs=e->loan.amsdu=e->loan.frag=e->loan.morefrag=255;
 e->loan.seq=65535; used=0; output[0]=0;
 carbox_recvframe_trace_report();
 assert(strstr(output,"id=4294967295") && strstr(output,"queue_count_at_hook=4294967295"));
 /* Native entry is allowed to consume/reborrow the frame before it returns.
  * The wrapper must never replace the NEW loan's empty entry metadata. */
 init_pool(); tick+=1; remove_frame(frames,PC(_rtw_alloc_recvframe,0x14));
 consume_entry=1; fake_pc=0x6677;
 assert(__wrap_rtw_recv_entry(frames)==-7);
 assert(pools[0].frames[0].state==RF_ALLOC && pools[0].allocs==2);
 assert(pools[0].frames[0].entry_adapter==0 && pools[0].frames[0].entry_caller==0);
 assert(!critical && inserts>=96);
 puts("PASS frame trace: lifecycle, handoff/return provenance, foreign residency/reborrow/home return, bounded overflow/drain, retained epochs, decrypt/defrag, tick wrap, forwarding");
}
'''
with tempfile.TemporaryDirectory(prefix='recvframe-trace-') as d:
    p=Path(d)
    for name in ('FreeRTOS.h', 'task.h'):
        (p/name).write_text('')
    (p/'test.c').write_text(stub+'\n#include "'+str(source)+'"\n'+test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                    '-fno-pie','-no-pie','-I',str(p),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
