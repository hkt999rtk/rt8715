#!/usr/bin/env python3
"""Exercise the production reporter with a contended UART mutex on the host.

Checks bounded complete lines, no blocking take, no UART under snapshot locks,
no format truncation at maximum counter values, and release before yielding.
The ROM formatter/UART/scheduler are stubbed; this is not a latency benchmark.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[5]
source = root / 'project/realtek_amebapro_v0_example/src/carbox/rx_handover_diag.c'
task_source = source.with_name('wlan_recvframe_diag.c')
libc = root / 'component/soc/realtek/8195b/misc/utilities/source/ram/libc_wrap.c'
lock_init = libc.read_text().split('static SemaphoreHandle_t printf_mutex = NULL;', 1)[1]
lock_init = 'static SemaphoreHandle_t printf_mutex = NULL;' + lock_init.split('typedef enum {', 1)[0]
stub = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_RX_HANDOVER_DIAG 1
#define configTICK_RATE_HZ 1000
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define tskIDLE_PRIORITY 0
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() do { assert(critical); --critical; } while(0)
#define configASSERT(x) assert(x)
typedef void *SemaphoreHandle_t;
static unsigned held, busy, critical, delays, take_calls, format_calls, max_line;
static uint32_t tick=123456789;
static char uart[32768];
static size_t used, line_len;
static int mutex;
static unsigned init_calls, report_waits;
static jmp_buf stop_task;
static void (*created_task)(void *);
static uintptr_t fake_pc;
#define __builtin_return_address(n) ((void *)fake_pc)
static uint32_t native_queue[32], native_epoch=1;
uint32_t carbox_recvframe_trace_epoch(const void *queue) {
 assert(critical); return queue==native_queue ? native_epoch : 0;
}
void carbox_recvframe_trace_report(void) { assert(init_calls==1 && !critical && !held); }
static void *xSemaphoreCreateMutex(void) {
 assert(!held && !critical); init_calls++; return &mutex;
}
static int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, void *handle) {
 assert(!strcmp(name,"wlan_rx_diag") && stack==1024 && !arg && priority==1 && !handle);
 created_task=fn; return pdPASS;
}
int __real_rtw_queue_empty(void *queue) { (void)queue; return 1; }
void *_rtw_alloc_recvframe(void *queue) { (void)queue; return NULL; }
static int xSemaphoreTake(void *m, unsigned wait) {
 assert(m==&mutex && wait==0 && !critical && !held); take_calls++;
 if(busy) return 0;
 held=1; return pdPASS;
}
static int xSemaphoreGive(void *m) {
 assert(m==&mutex && held && !critical); held=0; return pdPASS;
}
static void vTaskDelay(unsigned wait) {
 assert(!held && !critical);
 if(wait==2000) {
  if(++report_waits==2) longjmp(stop_task,1);
 } else { assert(wait==1); delays++; }
 tick+=wait;
}
static uint32_t xTaskGetTickCount(void) { return tick; }
static int putc_mock(void *arg, char c) {
 assert(!arg && held && !critical && used+1<sizeof(uart));
 uart[used++]=c; uart[used]=0; line_len++;
 if(c=='\n') {
  assert(line_len<=160 && used>1 && uart[used-2]=='\r');
  if(line_len>max_line) max_line=line_len;
  line_len=0;
 }
 return 1;
}
static unsigned core_mock(int (*putc_fn)(void *,char),void *arg,const char *fmt,va_list ap) {
 char buffer[2048];
 assert(!held && !critical); format_calls++;
 int n=vsnprintf(buffer,sizeof(buffer),fmt,ap);
 assert(n>=0 && (size_t)n<sizeof(buffer));
 for(int i=0;i<n;i++) putc_fn(arg,buffer[i]);
 return (unsigned)n;
}
static const struct {
 unsigned (*printf_core)(int (*)(void *,char),void *,const char *,va_list);
 int (*stdio_port_sputc)(void *,char);
} stdio_printf_stubs={core_mock,putc_mock};
'''
test = r'''
void carbox_rxhd_stack_snapshot(struct rxhd_stack_state *out) { memset(out,0xff,sizeof(*out)); }
uint32_t carbox_rxhd_zc_outstanding(void) { return UINT32_MAX; }
void carbox_rxhd_timer_snapshot(struct rxhd_timer_state *out) {
 memset(out,0,sizeof(*out));
 out->depth=44; out->capacity=44; out->callback_active=1;
 out->callback_since=tick-1000; out->callback_max_ticks=UINT32_MAX;
 out->daemon=(void *)0x1234;
 for(unsigned i=0;i<RXHD_TIMER_WAIT_SLOTS;i++) {
  out->waits[i].task=(void *)(uintptr_t)(0x1000+i);
  out->waits[i].timer=(void *)(uintptr_t)(0x2000+i);
  out->waits[i].command=-2147483647;
  out->waits[i].since_tick=1;
  out->waits[i].wait_ticks=UINT32_MAX;
  strcpy(out->waits[i].task_name,"rtw_recv_tasklet");
 }
}
int main(void) {
 for(unsigned i=0;i<RXHD_COUNT;i++) rxhd_counters[i]=UINT32_MAX;
 /* Run the actual production diagnostic task from an uninitialized mutex.
  * The second sampling sleep exits this infinite task after its first report. */
 assert(printf_lock_handle()==NULL);
 carbox_wlan_recvframe_diag_start();
 assert(created_task);
 if(!setjmp(stop_task)) created_task(NULL);
 assert(init_calls==1 && printf_lock_handle()!=NULL);
 assert(used && !held && !line_len && delays>=4 && format_calls>=4);
 assert(strstr(uart,"stack_reject=4294967295"));
 assert(strstr(uart,"zc_held=4294967295"));
 assert(strstr(uart,"wait_ticks=4294967295"));
 assert(rxhd_log_skipped==0);
 unsigned saved=used, calls=take_calls, formats=format_calls;
 busy=1;
 carbox_rxhd_report();
 assert(used==saved && !held && rxhd_log_skipped && take_calls>calls);
 assert(format_calls==formats); /* skip formatting when already contended */
 busy=0; printf_mutex=NULL;
 carbox_rxhd_report();
 assert(used==saved && !held);
 printf_mutex=&mutex;
 carbox_rxhd_report();
 assert(used>saved && !held && !line_len);
 /* Retained flow events repeat id (not tick) across continuation lines. */
 char fields[650]; fields[0]=0;
 for(unsigned i=0;i<32;i++) strcat(fields,"field=4294967295 ");
 saved=used; unsigned flow_skipped=rxhd_log_skipped;
 carbox_rxhd_log("[WLAN_FRAME_FLOW] id=%lu tick=4294967295 %squeue_count_at_hook=4294967295\n",
                (unsigned long)UINT32_MAX,fields);
 assert(used>saved && rxhd_log_skipped==flow_skipped);
 assert(strstr(uart+saved,"queue_count_at_hook=4294967295"));
 unsigned parts=0;
 for(char *p=uart+saved;*p;) {
  assert(!strncmp(p,"[WLAN_FRAME_FLOW] id=4294967295 ",30));
  p=strchr(p,'\n'); assert(p); p++; parts++;
 }
 assert(parts>1 && !held && !line_len);
 char oversize[900]; memset(oversize,'x',sizeof(oversize)-1); oversize[899]=0;
 saved=used; unsigned skipped=rxhd_log_skipped;
 carbox_rxhd_log("[RX_TEST] tick=123 %s\n",oversize);
 assert(used==saved && rxhd_log_skipped==skipped+1 && !held);
 /* Same native caller, but only the initialized free pool may be sampled.
  * Deliberately unreadable unknown pointer catches accidental field reads. */
 fake_pc=((uintptr_t)_rtw_alloc_recvframe & ~(uintptr_t)1)+8;
 assert(__wrap_rtw_queue_empty((void *)1)==1);
 assert(recvframe_stats[0].checks==0 && !critical);
 native_queue[0x2c/4]=31;
 assert(__wrap_rtw_queue_empty(native_queue)==1);
 assert(recvframe_stats[0].checks==1 && recvframe_stats[0].free_last==31);
 native_epoch++;
 assert(__wrap_rtw_queue_empty(native_queue)==1);
 assert(recvframe_stats[0].checks==1 && recvframe_stats[0].epoch==native_epoch && !critical);
 printf("PASS reporter startup + output: max_line=%u, nonblocking mutex, no locked formatting, skip/recovery, overflow\n",max_line);
}
'''
with tempfile.TemporaryDirectory(prefix='rxhd-report-') as d:
    p=Path(d)
    for name in ('FreeRTOS.h', 'task.h', 'semphr.h', 'rt_printf.h'):
        (p/name).write_text('')
    (p/'test.c').write_text(stub+'\n'+lock_init+'\n#include "'+str(source)+'"\n#include "'+str(task_source)+'"\n'+test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                    '-I',str(p),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
