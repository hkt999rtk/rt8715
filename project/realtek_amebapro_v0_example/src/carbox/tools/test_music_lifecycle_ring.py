#!/usr/bin/env python3
"""Exercise the production event ring, overflow accounting and deferred output."""
from pathlib import Path
import subprocess,tempfile
source=Path(__file__).resolve().parents[1]/'audio_music_lifecycle.c'
h=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
static unsigned critical,prints,skipped;static uint32_t ticks=0xfffffff0U;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() (--critical)
#define portTICK_PERIOD_MS 1U
#define CARBOX_RX_HANDOVER_DIAG_H
#define CONFIG_AUDIO_DIAG_LOG 1
#define carbox_rxhd_log capture
static uint32_t carbox_diag_log_skipped(void){return skipped;}
static uint32_t xTaskGetTickCount(void){return ticks;}
static void *xTaskGetCurrentTaskHandle(void){return (void *)(uintptr_t)0x12340000U;}
static void capture(const char *fmt,...){char line[256];va_list a;va_start(a,fmt);int n=vsnprintf(line,sizeof(line),fmt,a);va_end(a);assert(!critical && n<160);prints++;}
#include "SOURCE"
int main(void){
 uint32_t r[14];for(unsigned i=0;i<14;i++)r[i]=0xffffffffU-i;
 carbox_music_life_record(6,0,r);assert(count==0);
 carbox_music_life_record(6,1,r);assert(count==1 && events[0].session==r[4] && events[0].a0==r[0]);
 carbox_music_life_record(2,0,r);assert(count==2 && events[1].session==r[0]);
 ticks=25;
 for(unsigned i=2;i<70;i++)carbox_music_life_record(4,0,r);
 assert(count==64 && lost==6 && !prints && !critical);
 carbox_music_lifecycle_report();assert(count==0 && prints==129 && reported_lost==6);
 carbox_music_lifecycle_report();assert(prints==129);
 carbox_music_life_record(22,0,r);assert(count==1);carbox_music_lifecycle_report();assert(prints==131);
 skipped=5;carbox_music_lifecycle_report();assert(prints==132 && reported_skip==5);
 puts("PASS: no producer output; bounded ring, loss reporting, wrap and worst-case line length");
}
'''.replace('SOURCE',str(source))
with tempfile.TemporaryDirectory() as d:
 d=Path(d)
 for name in ['FreeRTOS.h','task.h','hal_timer.h']:(d/name).write_text('')
 (d/'test.c').write_text(h)
 subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-I',str(d),str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
