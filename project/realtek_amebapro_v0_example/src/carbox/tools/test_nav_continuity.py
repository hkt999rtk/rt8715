#!/usr/bin/env python3
"""Production C test: native layout simulated in low-address storage."""
from pathlib import Path
import subprocess,tempfile
src=Path(__file__).resolve().parents[1]/'audio_nav_continuity.c'
h=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NAV_CONTINUITY_HOST_TEST 1
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
static int fail_alloc, plc_rc, plc_calls, real_calls, purges, lock_fail, real_rc;
static uint32_t clock_us;
static void *test_alloc(size_t n){return fail_alloc?NULL:calloc(1,n);}
#define NAV_ALLOC test_alloc
uint32_t hal_read_curtime_us(void){return ++clock_us;}
int dispatch_semaphore_wait(void*p,uint64_t t){return lock_fail;}
int dispatch_semaphore_signal(void*p){return 0;}
uint32_t carbox_nav_session_id(void*c){return 7;}
int carbox_audio_diag_log(const char*f,...){return 0;}
void carbox_nav_diag_concealed(void*c,unsigned id,uint32_t n){}
void _RTPJitterBufferDiscardExcess(void*c){purges++;}
int __wrap_aacDecoder_DecodeFrame(void*h,int16_t*out,int n,unsigned flags){
 assert(flags==1 && n==480);plc_calls++;
 for(int i=0;i<n;i++)out[i]=(int16_t)(plc_calls*100+i);
 return plc_rc;
}
int AudioConverterFillComplexBuffer(void*a,void*b,void*c,uint32_t*d,void*e,void*f){real_calls++;*d=480;return real_rc;}
#include "SOURCE"
static uint8_t ctx[512], converter[32], node[1504];
static int16_t tail_pcm[480];
static void setword(void*p,unsigned o,uint32_t v){memcpy((uint8_t*)p+o,&v,4);}
static void ptr(void*p,unsigned o,void*v){assert((uintptr_t)v<=UINT32_MAX);setword(p,o,(uintptr_t)v);}
static struct continuity *setup(void){
 memset(ctx,0,sizeof(ctx));memset(converter,0,sizeof(converter));
 setword(ctx,0x150,44100);setword(ctx,0x154,0x61616365);setword(ctx,0x160,480);
 setword(ctx,0x174,44100);setword(ctx,0x188,2);setword(ctx,0x18c,1);setword(ctx,0x190,16);
 setword(ctx,0x148,50);setword(ctx,0x14c,34);setword(converter,0,0x61616365);
 setword(converter,12,1);ptr(converter,16,ctx);ptr(ctx,0x198,converter);
 ptr(ctx,0x138,node);setword(node,12,0);setword(node,0x5c8,960);ptr(node,0x5d0,tail_pcm);
 carbox_nav_continuity_open(ctx);return find(ctx);
}
static int decode(uint32_t ts){uint32_t n=480;setword(node,12,ts);return carbox_nav_convert(ctx,converter,0,node,&n,0,0);}
int main(void){
 struct continuity*s=setup();assert(s);
 carbox_nav_startup_purge(ctx);assert(!purges && s->stats.start_keep==1);
 carbox_nav_startup_purge(ctx);assert(purges==1);
 decode(0);decode(960);assert(plc_calls==1 && real_calls==2);
 int16_t out[480];memset(out,0,sizeof(out));
 assert(carbox_nav_fill_gap(ctx,out,120,480,11)==120);assert(out[0]==100 && out[59]==159);
 assert(carbox_nav_fill_gap(ctx,out,840,540,13)==840);assert(out[0]==160 && out[419]==579);
 assert(s->stats.plc_bytes==960 && s->stats.fallback_bytes==0);
 decode(2880);assert(plc_calls==4); // two real frames at 0,960; then 3 missing
 assert(carbox_nav_fill_gap(ctx,out,960,1440,13)==960);
 assert(carbox_nav_fill_gap(ctx,out,960,1920,13)==960);
 assert(carbox_nav_fill_gap(ctx,out,960,2400,13)==960);
 decode(5280);assert(plc_calls==4 && s->stats.gap_skip==1); // four: bounded fallback
 decode(5761);assert(plc_calls==4); // unaligned
 decode(5760);assert(plc_calls==4); // backwards
 carbox_nav_continuity_close(ctx);assert(!find(ctx));
 s=setup();setword(ctx,0x14c,47);carbox_nav_startup_purge(ctx);assert(purges==2);
 decode(0);plc_rc=0x1002;decode(960);assert(s->stats.plc_fail==1);
 memset(out,0x7f,sizeof(out));assert(!carbox_nav_fill_gap(ctx,out,960,480,13));assert(!out[0] && !out[479]);
 plc_rc=0;decode(1920);assert(s->stats.plc_ok==1);
 carbox_nav_continuity_close(ctx);
 s=setup();decode(0xfffffc40U);decode(0);assert(s->stats.plc_ok==1);
 assert(carbox_nav_fill_gap(ctx,out,960,0xfffffe20U,13)==960);
 carbox_nav_continuity_close(ctx);
 // Two-frame loss and valid FDK concealment error outputs are playable.
 s=setup();decode(0);plc_rc=0x4004;decode(1440);plc_rc=0;
 assert(s->stats.plc_ok==2 && s->stats.plc_fail==0);
 assert(carbox_nav_fill_gap(ctx,out,960,480,13)==960);
 assert(carbox_nav_fill_gap(ctx,out,960,960,13)==960);
 carbox_nav_continuity_close(ctx);
 // Cache pressure must not overwrite still-playable compensation.
 s=setup();decode(0);for(int i=1;i<=5;i++)decode(i*960);
 assert(s->stats.cache_full==1 && s->stats.plc_ok==5);
 assert(carbox_nav_fill_gap(ctx,out,960,480,13)==960);
 assert(!carbox_nav_fill_gap(ctx,out,960,4320,13));
 // Stale entries are reclaimed relative to the native playback cursor.
 s->started=1;setword(ctx,0x1b8,4800);decode(5760);
 assert(s->stats.cache_expired==3);
 assert(carbox_nav_fill_gap(ctx,out,960,5280,13)==960);
 // A failed real decode invalidates expected TS, avoiding duplicate PLC.
 real_rc=-1;decode(6240);real_rc=0;int prev=plc_calls;decode(7200);assert(plc_calls==prev);
 carbox_nav_continuity_close(ctx);s=setup();
 // Stop snapshot reads actual remaining PCM, not backing-buffer capacity.
 for(int i=0;i<480;i++)tail_pcm[i]=i&1?-100:100;
 setword(node,0x5c8,240);setword(node,0,0);setword(ctx,0x140,0);
 carbox_nav_continuity_before_mutex_destroy(ctx+0xc4);assert(s->stats.tail_samples==120);
 assert(s->stats.tail_nonzero==120 && s->stats.tail_peak==100 && s->stats.tail_mean_abs==100);
 carbox_nav_continuity_close(ctx);
 // Reuse must not retain another session's PCM or nonzero energy.
 s=setup();memset(tail_pcm,0,sizeof(tail_pcm));carbox_nav_continuity_before_mutex_destroy(ctx+0xc4);
 assert(s->stats.tail_samples==480 && !s->stats.tail_nonzero && !s->stats.tail_peak);
 assert(!carbox_nav_fill_gap(ctx,out,960,5280,13));carbox_nav_continuity_close(ctx);
 fail_alloc=1;assert(!setup());carbox_nav_startup_purge(ctx);assert(purges==3);
 fail_alloc=0;setword(ctx,0x18c,2);carbox_nav_continuity_open(ctx);assert(!find(ctx));
 puts("PASS: startup, bounded FDK conceal, partial PCM, wrap, fallback, stop energy, lifecycle");
}
'''.replace('SOURCE',str(src))
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(h)
 subprocess.run(['gcc','-std=c99','-O1','-g','-no-pie','-Wall','-Werror','-Wno-unused-function','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
