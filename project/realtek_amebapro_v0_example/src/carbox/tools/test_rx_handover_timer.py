#!/usr/bin/env python3
"""Exercise actual timer diagnostic hooks and xTimerGenericCommand on a host.

The bounded queue stub observes state before a blocked send returns, simulates
an ISR and another task, and verifies only the returning task is cleared.
This validates diagnostic bookkeeping, not hardware scheduling or a deadlock.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[5]
s=(root/'component/os/freertos/freertos_v10.0.0/timers.c').read_text()
a=s.index('#if CONFIG_RX_HANDOVER_DIAG\n#include "rx_handover_diag.h"'); b=s.index('/*lint -restore */',a)
helpers=s[a:b]
a=s.index('BaseType_t xTimerGenericCommand( '); b=s.index('\n/*-----------------------------------------------------------*/',a)
command=s[a:b]
stubs=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_RX_HANDOVER_DIAG 1
#include "rx_handover_diag.h"
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef void *TimerHandle_t;
typedef struct { void (*pxCallbackFunction)(TimerHandle_t); } Timer_t;
typedef struct { int xMessageID; union { struct { TickType_t xMessageValue; Timer_t *pxTimer; } xTimerParameters; } u; } DaemonTaskMessage_t;
#define configTIMER_QUEUE_LENGTH 44
#define pdPASS 1
#define pdFAIL 0
#define tmrFIRST_FROM_ISR_COMMAND 6
#define taskSCHEDULER_RUNNING 1
#define tmrNO_DELAY 0
#define configASSERT(x) assert(x)
#define traceTIMER_COMMAND_SEND(a,b,c,d) ((void)0)
#define mtCOVERAGE_TEST_MARKER() ((void)0)
static int critical;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() do { assert(critical > 0); --critical; } while(0)
static char task_rx[]="rtw_recv_tasklet",task_timer[]="Tmr Svc",task_other[]="other";
static void *current=task_rx;
static void *xTimerTaskHandle=task_timer;
static int queue_storage, other_queue;
static void *xTimerQueue=&queue_storage;
static uint32_t now=100, depth;
static unsigned require_wait, callback_calls;
static void *xTaskGetCurrentTaskHandle(void) { return current; }
static uint32_t xTaskGetTickCount(void) { return now; }
static char *pcTaskGetName(void *task) { return task; }
static unsigned uxQueueMessagesWaiting(void *q) { assert(q==xTimerQueue); return depth; }
static int xTaskGetSchedulerState(void) { return taskSCHEDULER_RUNNING; }
static int xQueueSendToBack(void *,void *,uint32_t);
static int xQueueSendToBackFromISR(void *,void *,int *);
'''
body=r'''
static unsigned waiting_for(struct rxhd_timer_state *s, void *task) {
 for (unsigned i=0;i<RXHD_TIMER_WAIT_SLOTS;i++) if(s->waits[i].task==task) return 1;
 return 0;
}
static int xQueueSendToBack(void *q,void *item,uint32_t ticks) {
 if(depth<44) return pdPASS;
 if(ticks) {
  struct rxhd_timer_state before;
  carbox_rxhd_timer_block(q,item,ticks);
  carbox_rxhd_timer_snapshot(&before);
  assert(waiting_for(&before,current));
  assert(before.depth==44);
  assert(before.self_waits==(current==xTimerTaskHandle ? 1U : 0U));
  assert(require_wait);
  ++now;
 }
 return pdFAIL;
}
static int xQueueSendToBackFromISR(void *q,void *item,int *w) {
 (void)q;(void)item;(void)w;
 return pdFAIL;
}
static void callback(TimerHandle_t timer) {
 struct rxhd_timer_state s;
 carbox_rxhd_timer_snapshot(&s);
 assert(s.callback_active && s.callback_timer==timer);
 now+=120;
 callback_calls++;
}
int main(void) {
 struct rxhd_timer_state s;
 Timer_t timer={callback};
 DaemonTaskMessage_t m={.xMessageID=4,.u.xTimerParameters={0,&timer}};
 carbox_rxhd_timer_block(&other_queue,&m,100);
 assert(rxhd_timer_stats.full_waits==0);
 assert(xTimerGenericCommand(&timer,4,30,NULL,UINT32_MAX)==pdPASS);
 assert(rxhd_timer_stats.full_waits==0);
 depth=44; require_wait=1;
 assert(xTimerGenericCommand(&timer,4,30,NULL,UINT32_MAX)==pdFAIL);
 assert(rxhd_timer_stats.full_waits==1 && rxhd_timer_stats.send_fail==1);
 assert(rxhd_timer_waiters==0);
 /* Another task's blocked command must survive an ISR send and a different
  * task returning from its send. The reporter still sees the original wait. */
 current=task_other;
 carbox_rxhd_timer_block(xTimerQueue,&m,UINT32_MAX);
 current=task_rx;
 assert(xTimerGenericCommand(&timer,7,30,NULL,0)==pdFAIL);
 carbox_rxhd_timer_snapshot(&s);
 assert(waiting_for(&s,task_other));
 current=task_timer;
 assert(xTimerGenericCommand(&timer,4,30,NULL,UINT32_MAX)==pdFAIL);
 carbox_rxhd_timer_snapshot(&s);
 assert(s.self_waits==1 && waiting_for(&s,task_other) && !waiting_for(&s,task_timer));
 current=task_other; prvRxhdTimerSendReturned();
 assert(rxhd_timer_waiters==0);
 /* A deleted blocked sender never returns through the send cleanup. Its
  * slot must be reusable, including when a new TCB has the same address. */
 char deleted[RXHD_TIMER_WAIT_SLOTS][16];
 for (unsigned i=0;i<RXHD_TIMER_WAIT_SLOTS;i++) {
  snprintf(deleted[i],sizeof(deleted[i]),"deleted%u",i);
  current=deleted[i];
  carbox_rxhd_timer_block(xTimerQueue,&m,UINT32_MAX);
 }
 assert(rxhd_timer_waiters==RXHD_TIMER_WAIT_SLOTS);
 for (unsigned i=0;i<RXHD_TIMER_WAIT_SLOTS;i++) {
  taskENTER_CRITICAL();
  carbox_rxhd_timer_task_deleted(deleted[i]);
  carbox_rxhd_timer_task_deleted(deleted[i]); /* no double decrement */
  taskEXIT_CRITICAL();
 }
 assert(rxhd_timer_waiters==0);
 current=deleted[0]; strcpy(deleted[0],"reused"); now+=1000;
 uint32_t previous_full=rxhd_timer_stats.full_waits;
 carbox_rxhd_timer_block(xTimerQueue,&m,10);
 carbox_rxhd_timer_snapshot(&s);
 assert(s.full_waits==previous_full+1 && !s.wait_overflow);
 for (unsigned i=0;i<RXHD_TIMER_WAIT_SLOTS;i++) if(s.waits[i].task==current) {
  assert(s.waits[i].since_tick==now && s.waits[i].wait_ticks==10);
  assert(!strcmp(s.waits[i].task_name,"reused"));
 }
 assert(waiting_for(&s,current));
 prvRxhdTimerSendReturned();
 assert(rxhd_timer_waiters==0);
 RXHD_TIMER_CALLBACK(&timer);
 carbox_rxhd_timer_snapshot(&s);
 assert(callback_calls==1 && s.callback_started==1 && s.callback_done==1);
 assert(!s.callback_active && s.callback_max_ticks==120);
 assert(critical==0);
 puts("PASS timer full wait, self wait, ISR isolation, return/deletion cleanup, TCB reuse, callback forwarding");
}
'''
with tempfile.TemporaryDirectory(prefix='rxhd-timer-') as d:
 p=Path(d);(p/'test.c').write_text(stubs+helpers+command+body)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I',str(root/'project/realtek_amebapro_v0_example/src/carbox'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
