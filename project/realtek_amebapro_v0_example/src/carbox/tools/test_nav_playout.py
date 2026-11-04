#!/usr/bin/env python3
"""Exercise production wake latch against non-retained POSIX signals."""
from pathlib import Path
import subprocess,tempfile
src=Path(__file__).resolve().parents[1]/'audio_nav_playout.c'
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define CONFIG_AUDIO_NAV_PLAYOUT 1
#define CONFIG_AUDIO_NAV_DIAG 0
#define NAV_PLAYOUT_HOST_TEST 1
static int critical, waits, signals, fail_init, signal_in_wait;
static unsigned seen_latency;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() (--critical)
int pthread_mutex_destroy(void*c){return 0;}
int pthread_cond_signal(void *c){signals++;return 0;}
int carbox_nav_condition_signal(void *);
int pthread_cond_wait(void*c,void*m){waits++;if(signal_in_wait)carbox_nav_condition_signal(c);return 0;}
int __real_RTPJitterBufferInit(void*c,const void*i,const void*o,uint32_t l){seen_latency=l;return fail_init;}
void __real_RTPJitterBufferFree(void*c){}
#include "SOURCE"
static unsigned char ctx[5][512];
static uint32_t fmt[9];
int main(void){
 fmt[0]=48000;fmt[4]=480;
 assert(!__wrap_RTPJitterBufferInit(ctx[0],fmt,0,300));assert(seen_latency==330);
 void *cond=ctx[0]+16;
 // Signal before decoder registers as waiter: old POSIX signal is discarded.
 carbox_nav_condition_signal(cond);assert(signals==1);
 assert(!carbox_nav_condition_wait(cond,(void*)1));assert(waits==0);
 // Latch is consumed exactly once; no busy loop at timestamp gate.
 carbox_nav_condition_wait(cond,(void*)1);assert(waits==1);
 carbox_nav_condition_signal(cond);carbox_nav_condition_signal(cond);
 carbox_nav_condition_wait(cond,(void*)1);assert(waits==1);
 carbox_nav_condition_wait(cond,(void*)1);assert(waits==2);
 // Signal while actually waiting must not cause a second artificial wake.
 signal_in_wait=1;carbox_nav_condition_wait(cond,(void*)1);assert(waits==3);
 signal_in_wait=0;carbox_nav_condition_wait(cond,(void*)1);assert(waits==4);
 __wrap_RTPJitterBufferFree(ctx[0]);
 carbox_nav_condition_signal(cond);
 assert(!__wrap_RTPJitterBufferInit(ctx[0],fmt,0,300));
 carbox_nav_condition_wait(cond,(void*)1);assert(waits==5);
 __wrap_RTPJitterBufferFree(ctx[0]);
 // Music and unsupported formats pass through unchanged.
 fmt[4]=1024;__wrap_RTPJitterBufferInit(ctx[0],fmt,0,300);assert(seen_latency==300);
 carbox_nav_condition_wait(cond,(void*)1);assert(waits==6);
 fmt[4]=480;fmt[0]=44100;
 __wrap_RTPJitterBufferInit(ctx[0],fmt,0,390);assert(seen_latency==400);
 __wrap_RTPJitterBufferFree(ctx[0]);
 __wrap_RTPJitterBufferInit(ctx[0],fmt,0,450);assert(seen_latency==450);
 __wrap_RTPJitterBufferFree(ctx[0]);
 // Init failure releases reserved slot. Exhaustion is safe passthrough.
 fail_init=1;__wrap_RTPJitterBufferInit(ctx[0],fmt,0,300);fail_init=0;
 for(int i=0;i<4;i++){__wrap_RTPJitterBufferInit(ctx[i],fmt,0,300);assert(seen_latency==330);}
 __wrap_RTPJitterBufferInit(ctx[4],fmt,0,300);assert(seen_latency==300);
 for(int i=0;i<5;i++)__wrap_RTPJitterBufferFree(ctx[i]);
 assert(!critical);puts("PASS: early/during-wait signals, coalescing, lifecycle, bounds and passthrough");
}
'''.replace('SOURCE',str(src))
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(harness)
 subprocess.run(['gcc','-std=c99','-Wall','-Werror','-Wno-unused-function','-I'+str(src.parent),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
