#!/usr/bin/env python3
"""Run production observation code against synthetic native queue memory."""
from pathlib import Path
import subprocess,tempfile
src=Path(__file__).resolve().parents[1]/'audio_nav_diag.c'
assert src.exists(), 'navigation diagnostic state collector not implemented'
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define CONFIG_AUDIO_NAV_PURGE_FIX 1
void carbox_nav_rx_report(void) {}
void carbox_nav_udp_report(void) {}
void carbox_nav_playout_report(void) {}
static unsigned critical, prints; static uint32_t clock_us=100;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() (--critical)
uint32_t hal_read_curtime_us(void) {return clock_us;}
static int log_printf(const char *f, ...) {assert(!critical); prints++;return 0;}
int __real_RTPJitterBufferInit(void*a,const void*b,const void*c,uint32_t d){return 0;}
void __real_RTPJitterBufferFree(void*a) {}
int __real_RTPJitterBufferGetFreeNode(void*a,void**b){*b=0;return -1;}
#define CARBOX_RX_HANDOVER_DIAG_H
#define CONFIG_AUDIO_DIAG_LOG 1
#define carbox_rxhd_log log_printf
uint32_t carbox_diag_log_skipped(void){return 0;}
#include "SOURCE"
static uint32_t ctx[128], nodes[32][380], regs[14];
static void put(unsigned off,uint32_t v){ctx[off/4]=v;}
static uint32_t ptr(void*p){assert((uintptr_t)p<=UINT32_MAX);return (uint32_t)(uintptr_t)p;}
int main(void){
 put(0x150,44100);put(0x160,480);put(0x188,2);put(0x148,50);put(0x1b4,300);
 carbox_nav_diag_open(ctx);assert(slots[0].ctx==ctx);
 uint32_t sid=slots[0].sid;
 // 28 encoded frames (~305 ms), no PCM: retain distinction at underrun.
 for(unsigned i=0;i<28;i++){nodes[i][0]=i==27?0:ptr(nodes[i+1]);nodes[i][3]=1000+i*480;}
 put(0x140,ptr(nodes[0]));put(0x14c,28);regs[5]=900;regs[8]=2220;
 carbox_nav_before(ctx,14,regs);
 assert(events[(head-1)%NAV_EVENTS].q.enc_n==28);
 assert(events[(head-1)%NAV_EVENTS].q.pcm_n==0);
 assert(events[(head-1)%NAV_EVENTS].q.enc_span==13440);
 assert(events[(head-1)%NAV_EVENTS].missing==1320);
 assert(prints==0);
 // Timestamp hole must not inflate actual PCM sample count.
 nodes[28][0]=ptr(nodes[29]);nodes[28][3]=1000;nodes[28][0x5c8/4]=960;
 nodes[29][0]=0;nodes[29][3]=2440;nodes[29][0x5c8/4]=960;
 put(0x138,ptr(nodes[28]));clock_us+=100;carbox_nav_before(ctx,14,regs);
 struct nav_event *e=&events[(head-1)%NAV_EVENTS];
 assert(e->q.pcm_samples==960 && e->q.pcm_span==1920 && e->q.pcm_holes==1);
 // Decode duration across 32-bit timer wrap, and preserve error result.
 clock_us=0xfffffff0;carbox_nav_before(ctx,14,regs);carbox_nav_before(ctx,4,regs);clock_us=20;regs[0]=123;
 carbox_nav_after(ctx,4,regs);assert(slots[0].decode_last==36 && slots[0].decode_errors==1);
 // Zero fill classification and elapsed rebuffer -> recovered.
 regs[2]=144;carbox_nav_before(ctx,12,regs);assert(slots[0].zero_bytes[2]==144);
 clock_us+=300000;carbox_nav_before(ctx,25,regs);assert(slots[0].recovery_last==300036);
 // Concealment removes only substituted bytes from actual-zero counters.
 clock_us+=1000000;uint32_t gap_head=head;
 regs[2]=960;carbox_nav_before(ctx,13,regs);assert(head==gap_head);
 carbox_nav_diag_concealed(ctx,13,840);carbox_nav_after(ctx,13,regs);
 assert(head==gap_head+1 && events[(head-1)%NAV_EVENTS].kind==5);
 assert(events[(head-1)%NAV_EVENTS].s.zero_bytes[3]==120);
 assert(slots[0].zero_bytes[3]==120 && slots[0].zero_bytes[2]==144);
 carbox_nav_diag_concealed(ctx,12,100);assert(slots[0].zero_bytes[2]==144);
 // End records survive free and context address reuse.
 carbox_nav_diag_close(ctx);assert(!slots[0].ctx);
 carbox_nav_diag_open(ctx);assert(slots[0].sid!=sid);
 for(unsigned i=0;i<40;i++)carbox_nav_before(ctx,14,regs);
 assert(lost_events>0 && head-tail<=NAV_EVENTS);
 carbox_audio_nav_report();assert(prints && head==tail && !critical);
 // Cover 48 kHz mono navigation too.
 carbox_nav_diag_close(ctx);put(0x150,48000);carbox_nav_diag_open(ctx);assert(slots[0].ctx==ctx);
 // Ignore non-navigation contexts.
 carbox_nav_diag_close(ctx);put(0x150,24000);
 carbox_nav_diag_open(ctx);assert(!slots[0].ctx);
 puts("PASS: queue separation/holes, zero reasons, wrap-safe timing, lifetime, bounded ring and deferred output");
}
'''.replace('SOURCE',str(src))
with tempfile.TemporaryDirectory() as d:
 d=Path(d)
 for n in ['FreeRTOS.h','task.h','hal_timer.h']:(d/n).write_text('')
 (d/'test.c').write_text(harness)
 subprocess.run(['gcc','-std=c99','-O2','-no-pie','-I'+str(d),str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
