#!/usr/bin/env python3
"""Exercise production progress collector: lifetime, wrap, failures and accounting."""
from pathlib import Path
import subprocess,tempfile,re
source=Path(__file__).resolve().parents[1]/'audio_music_progress.c'
for fmt in re.findall(r'carbox_audio_diag_log\("([^"\n]+)"',source.read_text()):
    maximum=fmt.replace('%lu','4294967295').replace('%ld','-2147483648').replace('%08lx','ffffffff').replace('%p','->70abcdef').replace('\\n','\n')
    assert len(maximum)<160,maximum
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
static uint32_t clock_us=100;static unsigned critical,printed;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() (--critical)
#define CARBOX_RX_HANDOVER_DIAG_H
#define CONFIG_AUDIO_DIAG_LOG 1
#define carbox_rxhd_log log_printf
uint32_t hal_read_curtime_us(void){return clock_us;}
static int log_printf(const char *f,...){
 char b[768];va_list a;va_start(a,f);int n=vsnprintf(b,sizeof(b),f,a);va_end(a);
 assert(!critical && n<160);printed++;return 0;
}
#include "SOURCE"
static uint32_t sessions[5][2048],r[48],length,outlen;
static uint8_t packet[100];
static uint32_t ptr(void *p){assert((uintptr_t)p<=UINT32_MAX);return (uint32_t)(uintptr_t)p;}
static void start(unsigned i){sessions[i][1084/4]=102;sessions[i][1108/4]=36;carbox_music_progress_before(sessions[i],1,r);}
static void rx(unsigned seq,unsigned ssrc,int error){
 memset(packet,0,sizeof(packet));packet[0]=0x80;packet[2]=seq>>8;packet[3]=seq;packet[7]=0xa1;packet[11]=ssrc;
 r[1]=ptr(packet);r[2]=sizeof(packet);r[3]=ptr(&length);length=80;
 carbox_music_progress_before(sessions[0],2,r);clock_us+=10;r[0]=error;
 carbox_music_progress_after(sessions[0],2,r);
}
int main(void){
 start(0);struct music_progress *s=&music_slots[0];assert(s->sid==1 && s->stream==102);
 rx(65534,1,0);rx(1,1,0);rx(0,1,0);rx(1,1,0);rx(2,1,0);
 assert(s->received==5 && s->missing==2 && s->backward==1 && s->duplicates==1 && s->high_seq==2);
 rx(6000,1,0);assert(s->large_jump==1);rx(42,2,0);assert(s->ssrc_changes==1 && s->high_seq==42);
 uint32_t missing=s->missing;rx(43,2,0);assert(s->missing==missing);
 // Receive error must not dereference unsuccessful output pointers.
 r[1]=0;r[3]=0;carbox_music_progress_before(sessions[0],2,r);r[0]=-11;
 carbox_music_progress_after(sessions[0],2,r);assert(s->recv_errors==1 && s->last_recv_error==-11);
 // Invalid/short/oversize successful packets cannot change sequence tracking.
 r[1]=0;r[2]=100;r[3]=ptr(&length);length=999;
 carbox_music_progress_before(sessions[0],2,r);r[0]=0;carbox_music_progress_after(sessions[0],2,r);
 assert(s->invalid==1 && s->high_seq==43);
 // Native caller stack byte-count and zero/copied accounting.
 r[45]=4096;carbox_music_progress_before(sessions[0],4,r);clock_us+=500;
 r[2]=200;carbox_music_progress_before(sessions[0],10,r);
 r[2]=300;carbox_music_progress_before(sessions[0],11,r);
 r[2]=3596;carbox_music_progress_before(sessions[0],12,r);
 sessions[0][7148/4]=7;sessions[0][7196/4]=1234;
 ((uint8_t *)sessions[0])[7180]=1;((uint8_t *)sessions[0])[912]=0;sessions[0][608/4]=37;
 carbox_music_progress_before(sessions[0],5,r);
 assert(s->reads==1 && s->requested==4096 && s->zero_empty==200 && s->zero_future==300 && s->pcm_copied==3596);
 assert(s->read_max==500 && s->busy==7 && s->native_rx_ts==1234);
 assert(s->native_flush==1 && s->native_rx_count==37);
 // Decode result is bytes via original second stack argument, only on success.
 r[15]=ptr(&outlen);outlen=4096;carbox_music_progress_before(sessions[0],8,r);clock_us+=600;r[0]=0;
 carbox_music_progress_after(sessions[0],8,r);assert(s->decoded_bytes==4096 && s->decode_max==600);
 r[15]=0;carbox_music_progress_before(sessions[0],9,r);r[0]=-5;carbox_music_progress_after(sessions[0],9,r);
 assert(s->decode_errors==1 && s->last_decode_error==-5);
 carbox_music_progress_before(sessions[0],6,r);r[0]=-42;carbox_music_progress_after(sessions[0],6,r);
 assert(s->decrypt_errors==1 && s->last_decrypt_error==-42);
 // Time wrap and select-error accounting.
 clock_us=0xfffffff0;carbox_music_progress_before(sessions[0],3,r);clock_us=20;r[0]=-1;
 carbox_music_progress_after(sessions[0],3,r);assert(s->select_max==36 && s->select_errors==1);
 // Probes preserve registers/native state, and never print.
 uint32_t saved[48],native[2048];memcpy(saved,r,sizeof(r));memcpy(native,sessions[0],sizeof(native));
 r[6]=0;saved[6]=0;carbox_music_progress_before(sessions[0],14,r);
 assert(!memcmp(saved,r,sizeof(r)) && !memcmp(native,sessions[0],sizeof(native)) && !printed && !critical);
 clock_us+=5000100;carbox_music_progress_report();assert(printed==10);
 // Close retains data until reporting; same pointer gets a new lifetime.
 carbox_music_progress_before(sessions[0],13,r);start(0);
 assert(!music_slots[0].active && music_slots[0].dirty && music_slots[1].sid==2);
 carbox_music_progress_report();assert(!music_slots[0].dirty && printed==20);
 // Never evict an active session; lost evidence counted explicitly.
 start(1);start(2);start(3);start(4);assert(music_lost==1);
 carbox_music_progress_report();assert(printed==21 && !critical);
 puts("PASS: music receive/header/error, PCM accounting, decode outputs, native immutability, time wrap and bounded lifetimes");
}
'''.replace('SOURCE',str(source))
with tempfile.TemporaryDirectory() as temp:
 d=Path(temp)
 for n in ['FreeRTOS.h','task.h','hal_timer.h']:(d/n).write_text('')
 (d/'test.c').write_text(harness)
 subprocess.run(['gcc','-std=c99','-O2','-Wall','-Wextra','-Werror','-no-pie','-I'+str(d),str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
