#!/usr/bin/env python3
"""Exercise production bridge with fake clock APIs, including wrap and teardown."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
stub=r'''
#ifndef CLOCK_TEST_STUB
#define CLOCK_TEST_STUB
#include <stdint.h>
#include <stddef.h>
typedef struct {uint32_t xOverflowCount,xTimeOnEntering;} TimeOut_t;
#define configTICK_RATE_HZ 1000
#define CONFIG_TIMER_SCLK_FREQ 2000000
#define configUSE_TICKLESS_IDLE 1
extern uint32_t SystemCoreClock;
void enter(void);void leave(void);
#define taskENTER_CRITICAL() enter()
#define taskEXIT_CRITICAL() leave()
#define MEDIA_DRIFT_SYSTICK_RELOAD() 399999U
uint32_t hal_read_curtime_us(void);
void vTaskSetTimeOutState(TimeOut_t *);
int rt_printf(const char *,...);
#endif
'''
test=r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "media_clock_evidence.c"
uint32_t SystemCoreClock=400000000;
static uint64_t now;
static int slow_rtos;
static int depth,calls,finalizes,getters,prints;
static uint8_t step;
void enter(void){++depth;}
void leave(void){assert(depth>0);--depth;}
uint32_t hal_read_curtime_us(void){return (uint32_t)now;}
void vTaskSetTimeOutState(TimeOut_t *t){uint64_t tick=(now-(slow_rtos?now/1000000*2918:0))/1000;t->xOverflowCount=tick>>32;t->xTimeOnEntering=tick;}
uint64_t UpTicks(void){assert(!depth);return now/1000;}
uint8_t __real_AirTunesClock_Adjust(void *p,int64_t a,uint8_t r){assert(!depth && p==(void *)123 && a==0 && !r);++calls;return step;}
int __real_AirTunesClock_Finalize(void *p){assert(!depth && p==(void *)123);++finalizes;return 17;}
void AirTunesClock_GetSynchronizedTimeNearUpTicks(void *p,struct source_time *t,uint64_t ticks){
 assert(!depth && p==(void *)123 && ticks==now/1000);++getters;
 uint64_t us=900000000ULL+ticks*1000+ticks/20;
 t->seconds=us/1000000;t->fraction=((us%1000000)<<32)/1000000<<32;
}
int rt_printf(const char *fmt,...){assert(!depth);++prints;(void)fmt;return 0;}
int main(void){
 now=UINT32_MAX-4000000ULL;carbox_media_clock_evidence_report(1);
 assert(source_fit.state==MEDIA_LOCAL && !source_fit.applied_ppb);
 for(unsigned i=0;i<100;i++){
  now+=2000000;step=i==0;
  assert(__wrap_AirTunesClock_Adjust((void *)123,0,0)==step);
  if(i%5==0)carbox_media_clock_evidence_report(i+2);
 }
 carbox_media_clock_evidence_report(102);
 assert(calls==100 && getters==100 && !source.lost && !depth);
 assert(hw_extended>UINT32_MAX && source_fit.state==MEDIA_TRACK);
 assert(source_fit.estimated_ppb>49900 && source_fit.estimated_ppb<50100 && source_fit.applied_ppb==0);
 now+=11000000;carbox_media_clock_evidence_report(103);assert(source_fit.state==MEDIA_HOLDOVER);
 assert(__wrap_AirTunesClock_Finalize((void *)123)==17 && finalizes==1);
 unsigned generation=source.generation;
 now+=2000000;step=1;__wrap_AirTunesClock_Adjust((void *)123,0,0);
 assert(source.generation!=generation);
 carbox_media_clock_evidence_report(104);assert(source_fit.state!=MEDIA_TRACK);
 /* 正常 RTT 筛选可能每20秒才调用 Adjust；本机采样不得依赖它。 */
 slow_rtos=1;
 for(unsigned i=0;i<300;i++){
  now+=2000000;step=0;
  carbox_media_clock_evidence_tick();
  if(i%10==0)__wrap_AirTunesClock_Adjust((void *)123,0,0);
  if(i%5==0)carbox_media_clock_evidence_report(i+105);
 }
 assert(rtos_fit.estimated_ppb< -2900000 && rtos_fit.estimated_ppb> -2935000);
 assert(rtos_fit.out_of_range>0 && rtos_fit.applied_ppb==0);
 assert(__wrap_AirTunesClock_Finalize((void *)123)==17);
 for(unsigned i=0;i<30;i++){now+=2000000;carbox_media_clock_evidence_tick();carbox_media_clock_evidence_report(i+405);}
 assert(rtos_fit.estimated_ppb< -2900000 && rtos_fit.out_of_range>1);
 assert(prints>0 && !depth);puts("production bridge: one original call, no locked I/O, HAL wrap, late sync, slew candidate, stale/end/reuse PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
 d=Path(td);(d/'stubs.h').write_text(stub)
 for h in ['FreeRTOS.h','task.h','hal_timer.h','diag.h']:(d/h).write_text('#include "stubs.h"\n')
 (d/'test.c').write_text(test)
 args=['cc','-std=c99','-O1','-Wall','-Wextra','-Werror','-fsanitize=undefined','-DCONFIG_MEDIA_CLOCK_DRIFT_PROFILE=1','-I'+str(d),'-I'+str(root)]
 subprocess.run(args+[str(d/'test.c'),str(root/'media_clock/AirPlayMediaClockSource.c'),str(root/'media_clock/AirPlayMediaClockDiscipline.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-DCONFIG_MEDIA_CLOCK_DRIFT_PROFILE=0','-I'+str(root),'-c',str(root/'media_clock_evidence.c'),'-o',str(d/'off.o')],check=True)
 symbols=subprocess.check_output(['nm',str(d/'off.o')]).decode()
 assert not symbols.strip(),symbols
