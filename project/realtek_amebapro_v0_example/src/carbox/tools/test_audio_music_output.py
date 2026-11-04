#!/usr/bin/env python3
"""Run production PCM evidence counters against deterministic native boundaries."""
from pathlib import Path
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[1] / 'audio_music_output.c'
STUB = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#define CONFIG_AUDIO_MUSIC_DIAG 1
#define CONFIG_AUDIO_DIAG_LOG 1
#define configTICK_RATE_HZ 1000
typedef unsigned socklen_t;
struct sockaddr { unsigned char bytes[16]; };
static unsigned critical, calls[5];
static uint32_t tick, micros;
static uintptr_t fake_task=1;
static int native_result, native_errno, consume_reinit;
static void *expected_ring, *expected_data, *expected_stream;
static uint32_t expected_length;
static unsigned char stream[720] __attribute__((aligned(4)));
static unsigned char second_stream[720] __attribute__((aligned(4)));
static unsigned char ring[64], second_ring[64], data[32];
static struct sockaddr address;
static char output[65536];
static unsigned used;
static uint32_t fake_skipped;
static unsigned skip_log;
uint32_t carbox_diag_log_skipped(void) { return fake_skipped; }
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() do { assert(critical); --critical; } while (0)
static uint32_t xTaskGetTickCount(void) { return tick; }
void *xTaskGetCurrentTaskHandle(void) { return (void *)fake_task; }
static uint32_t hal_read_curtime_us(void) { return micros++; }
int carbox_music_output_init_c(void *, uint32_t, void *);
static int RingBuffer_init(void *r, uint32_t n) {
 assert(!critical && r==expected_ring && n==100000); calls[0]++; return 17;
}
static int RingBuffer_push(void *r, const void *d, uint32_t n) {
 assert(!critical && r==expected_ring && d==expected_data && n==expected_length);
 calls[1]++;
 if(consume_reinit) {
  consume_reinit=0;
  assert(carbox_music_output_init_c(r,100000,expected_stream)==17);
 }
 return native_result;
}
static int RingBuffer_pop(void *r, void *d, uint32_t n) {
 assert(!critical && r==expected_ring && d==expected_data && n==expected_length);
 calls[2]++; return native_result;
}
static ssize_t lwip_sendto(int fd, const void *d, size_t n, int flags,
                         const struct sockaddr *to, socklen_t len) {
 assert(!critical && fd==23 && d==data && n==200 && flags==7);
 assert(to==&address && len==13); calls[3]++; micros+=39; return native_result;
}
static int lwip_errno(void) { return native_errno; }
static void pthread_exit(void *v) { assert(!critical && v==(void *)0x1234); calls[4]++; }
void carbox_rxhd_log(const char *fmt, ...) {
 if(skip_log) { fake_skipped++; return; }
 assert(!critical); va_list ap; va_start(ap,fmt);
 int n=vsnprintf(output+used,sizeof(output)-used,fmt,ap); va_end(ap);
 assert(n>0 && n<768 && (unsigned)n<sizeof(output)-used); used+=(unsigned)n;
}
'''
TEST = r'''
static void put32(void *p, uint32_t v) { memcpy(p,&v,4); }
static void start(void *s, void *r, unsigned type) {
 put32((char *)s+4,23); put32((char *)s+8,type);
 put32((char *)s+48,(uint32_t)(uintptr_t)r);
 expected_ring=r; expected_stream=s;
 assert(carbox_music_output_init_c(r,100000,s)==17);
}
static void reset_output(void) { used=0; output[0]=0; }
int main(void) {
 tick=100; start(stream,ring,100);
 expected_data=data; expected_length=100;
 native_result=1; assert(carbox_music_output_push_c(ring,data,100,stream)==1);
 native_result=0; assert(carbox_music_output_push_c(ring,data,100,stream)==0);
 native_result=1; assert(carbox_music_output_pop_c(ring,data,100,stream)==1);
 native_result=0; assert(carbox_music_output_pop_c(ring,data,100,stream)==0);
 native_result=200; assert(carbox_music_output_sendto_c(23,data,200,7,&address,13,stream)==200);
 native_result=-1; native_errno=11;
 assert(carbox_music_output_sendto_c(23,data,200,7,&address,13,stream)==-1);
 native_result=60; assert(carbox_music_output_sendto_c(23,data,200,7,&address,13,stream)==60);
 tick=5100; carbox_audio_music_output_report();
 assert(strstr(output,"type=100") && strstr(output,"push_ok=1 push_fail=1"));
 assert(strstr(output,"push_pcm=100 push_drop_pcm=100"));
 assert(strstr(output,"pop_ok=1 pop_empty=1 pop_pcm=100"));
 assert(strstr(output,"send_ok=1 send_fail=1 send_short=1"));
 assert(strstr(output,"udp_bytes=260 udp_drop=340") && strstr(output,"errno=11"));
 reset_output(); tick=7100; carbox_audio_music_output_report(); assert(used==0);
 tick=7200; carbox_music_output_exit_c((void *)0x1234,stream);
 skip_log=1; carbox_audio_music_output_report(); assert(used==0);
 skip_log=0;
 carbox_audio_music_output_report(); assert(strstr(output,"closed=1") && strstr(output,"epoch=1"));
 assert(calls[4]==1);
 reset_output(); tick=7300; start(stream,ring,100);
 expected_length=200; native_result=1;
 consume_reinit=1; assert(carbox_music_output_push_c(ring,data,200,stream)==1);
 /* A native call finishing after reinit belongs to the previous epoch. */
 carbox_audio_music_output_report();
 assert(strstr(output,"epoch=2") && strstr(output,"push_pcm=200"));
 assert(strstr(output,"reinit=1"));
 reset_output(); tick=12300; carbox_audio_music_output_report();
 assert(strstr(output,"epoch=3") && strstr(output,"push_pcm=0"));
 /* Final snapshots survive rapid reuse; overflow is visible, never reallocates. */
 for(unsigned i=0;i<12;i++) { tick++; start(stream,ring,100); }
 skip_log=1; carbox_audio_music_output_report(); skip_log=0;
 reset_output(); carbox_audio_music_output_report();
 assert(strstr(output,"[MUSIC_OUT_LOSS]") && strstr(output,"slot_overflow="));
 unsigned records=0; for(char *p=output;(p=strstr(p,"[MUSIC_OUT]"));p++) records++;
 assert(records<=4);
 /* A separate stream retains its type instead of inheriting a recycled task. */
 for(unsigned i=0;i<4;i++) { reset_output(); tick+=5000; carbox_audio_music_output_report(); }
 start(second_stream,second_ring,101);
 reset_output(); tick+=5000; carbox_audio_music_output_report();
 assert(strstr(output,"type=101"));
 assert(!critical && calls[0]>=15 && calls[1]==3 && calls[2]==2 && calls[3]==3);
 /* Previously reported active data still needs a final record at reinit. */
 expected_length=777; native_result=1;
 carbox_music_output_push_c(second_ring,data,777,second_stream);
 reset_output(); tick+=5000; carbox_audio_music_output_report();
 assert(strstr(output,"push_pcm=777"));
 start(second_stream,second_ring,101);
 reset_output(); carbox_audio_music_output_report();
 assert(strstr(output,"closed=1 reinit=1") && strstr(output,"push_pcm=777"));
 /* The native thread clears running before pthread_exit: a new output task
  * can start at the same stream address before the old exit hook executes. */
 for(unsigned i=0;i<3;i++) { tick+=5000; carbox_audio_music_output_report(); }
 fake_task=21; start(second_stream,second_ring,101);
 fake_task=22; start(second_stream,second_ring,101);
 fake_task=21; carbox_music_output_exit_c((void *)0x1234,second_stream);
 reset_output(); tick+=5000; carbox_audio_music_output_report();
 assert(strstr(output,"task=0x16") && strstr(output,"closed=0"));
 /* All field widths together must fit the real formatter's 768-byte budget. */
 memset(outputs,0,sizeof(outputs));
 memset(&outputs[0],0xff,sizeof(outputs[0]));
 reset_output(); carbox_audio_music_output_report();
 assert(strstr(output,"wraps=4294967295") && strstr(output,"udp_drop=4294967295"));
 puts("PASS music output: native args/results, PCM and UDP losses, 5s throttle, final epochs, concurrent reuse, bounded overflow");
}
'''


def main():
    assert SOURCE.exists(), 'missing PCM evidence implementation'
    with tempfile.TemporaryDirectory(prefix='music-output-test-') as d:
        p = Path(d)
        (p / 'lwip').mkdir()
        for name in ('FreeRTOS.h', 'task.h', 'hal_timer.h', 'lwip/sockets.h'):
            (p / name).write_text('')
        (p / 'test.c').write_text(STUB+'\n#include "'+str(SOURCE)+'"\n'+TEST)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                        '-fsanitize=undefined','-fno-pie','-no-pie','-I',str(p),
                        str(p/'test.c'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)


if __name__ == '__main__':
    main()
