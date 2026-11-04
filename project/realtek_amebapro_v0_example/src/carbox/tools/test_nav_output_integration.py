#!/usr/bin/env python3
"""Exercise actual output wrappers with pacing, including music fallback and lifetime."""
import subprocess,tempfile
from pathlib import Path
from test_audio_music_output import STUB,SOURCE
ROOT=SOURCE.parent
extra=r'''
#include <stdlib.h>
#define CONFIG_AUDIO_NAV_PACING 1
static uint32_t ring_bytes=6240, sleeps;
static unsigned fail_alloc;
static void *test_calloc(size_t n,size_t s) { return fail_alloc?NULL:calloc(n,s); }
#define calloc test_calloc
static int RingBuffer_size(void *r) { assert(!critical && r==expected_ring); return ring_bytes; }
static void vTaskDelay(uint32_t n) { assert(!critical && n);tick+=n;micros+=n*1000;sleeps++; }
'''
test=r'''
static void put32(void *p,uint32_t n){memcpy(p,&n,4);}
static void start(unsigned type,unsigned rate){
 put32(stream+4,23);put32(stream+8,type);put32(stream+20,4);put32(stream+32,rate);
 expected_ring=ring;expected_stream=stream;assert(carbox_music_output_init_c(ring,100000,stream)==17);
}
int main(void){
 (void)second_stream; (void)second_ring;
 start(100,48000);assert(carbox_nav_output_pace_c(ring,stream,1440)==0 && sleeps==0);
 carbox_music_output_exit_c((void*)0x1234,stream);carbox_audio_music_output_report();
 start(101,44100);assert(carbox_nav_output_pace_c(ring,stream,1440)==0);
 carbox_music_output_exit_c((void*)0x1234,stream);carbox_audio_music_output_report();
 fail_alloc=1;start(101,48000);assert(carbox_nav_output_pace_c(ring,stream,1440)==0);
 carbox_music_output_exit_c((void*)0x1234,stream);carbox_audio_music_output_report();fail_alloc=0;
 start(101,48000);expected_data=data;expected_length=1440;native_result=1;
 assert(carbox_music_output_pop_c(ring,data,1440,stream)==1);
 uint32_t before=micros;
 assert(carbox_nav_output_pace_c(ring,stream,1440)==1);
 assert(micros-before>=7500 && micros-before<9000 && sleeps>0);
 assert(carbox_nav_output_pace_c(ring,stream,1416)==1);
 native_result=0;carbox_music_output_pop_c(ring,data,1440,stream);micros+=12000;
 native_result=1;carbox_music_output_pop_c(ring,data,1440,stream);
 native_result=200;data[2]=0;data[3]=7;data[7]=42;
 carbox_music_output_sendto_c(23,data,200,7,&address,13,stream);
 micros+=42311;data[3]=8;carbox_music_output_sendto_c(23,data,200,7,&address,13,stream);
 native_result=0;carbox_music_output_pop_c(ring,data,1440,stream);micros+=30000;
 uint32_t old_sleeps=sleeps;put32(stream+52,1);
 assert(carbox_nav_output_pace_c(ring,stream,1440)==1 && sleeps==old_sleeps);
 carbox_music_output_exit_c((void*)0x1234,stream);
 carbox_audio_music_output_report();
#if CONFIG_AUDIO_DIAG_LOG
 assert(strstr(output,"[NAV_PACE]") && strstr(output,"enabled=1"));
 assert(strstr(output,"empty_resume_n=1") && strstr(output,"gap_seq=8"));
 assert(strstr(output,"terminal_empty_us=30"));
#else
 assert(!used);
#endif
 /* More sessions than slots: quiet reporting must still recycle epochs. */
 for(unsigned i=0;i<40;i++) {
  put32(stream+52,0);start(101,48000);
  assert(outputs[0].pacer || outputs[1].pacer);
  assert(carbox_nav_output_pace_c(ring,stream,1440)==1);
  carbox_music_output_exit_c((void*)0x1234,stream);
  carbox_audio_music_output_report();
 }
 assert(output_overflow==0);
 /* State lives until the final snapshot is emitted and is freed on slot reuse. */
 for(unsigned i=0;i<MUSIC_OUTPUT_SLOTS;i++){free(outputs[i].pacer);outputs[i].pacer=NULL;}
 puts("PASS pacing output integration: unchanged native forwarding, music/format/nomem fallback, sleep outside lock, terminal stats");
}
'''
with tempfile.TemporaryDirectory(prefix='nav-output-integration-') as t:
 p=Path(t);(p/'lwip').mkdir()
 for name in ('FreeRTOS.h','task.h','hal_timer.h','lwip/sockets.h'):(p/name).write_text('')
 for logs in (0,1):
  (p/'test.c').write_text(STUB.replace('#define CONFIG_AUDIO_DIAG_LOG 1', '#define CONFIG_AUDIO_DIAG_LOG '+str(logs))+extra+'\n#include "'+str(SOURCE)+'"\n'+test)
  subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2','-fsanitize=undefined','-fno-pie','-no-pie','-I',str(p),str(p/'test.c'),str(ROOT/'audio_nav_pacing.c'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
