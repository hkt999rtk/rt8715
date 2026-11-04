#!/usr/bin/env python3
"""Exercise actual screen write wrapper and caller-style partial/retry progress."""
from pathlib import Path
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[1] / 'screen_tx_poll_wait.c'
STUB = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#define LWIP_TCP 1
#define LWIP_TCP_RECOVERY 1
#define CONFIG_SCREEN_TX_WRITE_CHUNK_BYTES 4096
#define CONFIG_AUDIO_OUTPUT_TASK_PRIORITY 5
static unsigned critical, tick, calls, polls, policy, logged, in_write;
static size_t last_len, used;
static int next_result = -99;
static int lwip_err;
static unsigned char output[20000];
static char logs[4096];
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() (--critical)
#define pdMS_TO_TICKS(n) (n)
#define configTICK_RATE_HZ 1000
typedef uint32_t TickType_t;
static TickType_t xTaskGetTickCount(void) { return tick; }
static uint32_t hal_read_curtime_us(void) {
 /* Another socket task changes the shared target error during instrumentation. */
 lwip_err=EIO; return tick * 1000;
}
static int lwip_usb_screen_rto_enable(int fd) { assert(fd==3 && !critical); policy++; return 0; }
static int lwip_errno(void) { return lwip_err; }
static ssize_t lwip_write(int fd, const void *p, size_t n) {
 assert(fd==3 && !critical); calls++; last_len=n;
 int r=next_result == -99 ? (int)n : next_result; next_result=-99;
 if(r<0) { lwip_err = r == -1 ? EAGAIN : EPIPE; return -1; }
 assert((size_t)r<=n && used+(size_t)r<=sizeof(output));
 memcpy(output+used,p,(size_t)r); used+=(size_t)r; return r;
}
void carbox_rxhd_log(const char *fmt, ...) {
 assert(!critical && !in_write); logged++;
 va_list ap; va_start(ap,fmt);
 size_t n=strlen(logs); vsnprintf(logs+n,sizeof(logs)-n,fmt,ap); va_end(ap);
}
'''
TEST = r'''
int main(void) {
 unsigned char data[10000];
 for(unsigned i=0;i<sizeof(data);i++) data[i]=(unsigned char)(i*13);
 size_t off=0;
 next_result=1000;
 while(off<sizeof(data)) {
  in_write=1;
  ssize_t r=carbox_screen_tcp_write(3,data+off,sizeof(data)-off);
  in_write=0;
  assert(last_len<=4096 && !critical && !logged);
  if(r<0) { assert(lwip_errno()==EAGAIN); polls++; continue; }
  off+=(size_t)r;
  if(calls==1) next_result=-1;
 }
 assert(calls==5 && polls==1 && policy==calls);
 assert(used==sizeof(data) && !memcmp(data,output,sizeof(data)));
 next_result=-2;
 assert(carbox_screen_tcp_write(3,data,8000)==-1 && lwip_errno()==EPIPE);
 next_result=0;
 assert(carbox_screen_tcp_write(3,data,8000)==0);
 assert(carbox_screen_tcp_write(3,data,0)==0);
#if CONFIG_SCREEN_TX_BATCH_DIAG
 tick=6000; carbox_screen_write_report();
 assert(logged && strstr(logs,"writes=8") && strstr(logs,"limited=6"));
 assert(strstr(logs,"partial=1") && strstr(logs,"eagain=1") && strstr(logs,"errors=1"));
 assert(strstr(logs,"zero=1") && strstr(logs,"accepted=10000"));
 unsigned old=logged; tick+=6000; carbox_screen_write_report(); assert(logged==old);
#endif
 puts("PASS screen batch: stream preserved through partial/EAGAIN, cap, fatal/zero, deferred stats");
}
'''

def check_build_selection():
    build = SOURCE.parents[2] / 'GCC-RELEASE/application.is.mk'
    text = build.read_text()
    fragment = text[text.index('SCREEN_TX_POLL_WAIT ?='):text.index('SRAM_C += ../src/carbox/screen_tx_direct_crypto.c')]
    for poll in (0, 1):
        result = subprocess.check_output(['make', '-s', '-f', '-', f'SCREEN_TX_POLL_WAIT={poll}'],
            input=fragment+'\n.PHONY: show\nshow:\n\t@echo $(SRAM_C)\n', text=True)
        assert '../src/carbox/screen_tx_poll_wait.c' in result, 'batch reporter must link even with poll disabled'


def main():
    for diag in (0, 1):
        with tempfile.TemporaryDirectory(prefix='screen-batch-') as d:
            p = Path(d)
            (p/'lwip').mkdir()
            for name in ('lwip/sockets.h', 'lwip/err.h', 'FreeRTOS.h', 'task.h', 'hal_timer.h'):
                (p/name).write_text('')
            (p/'test.c').write_text(STUB + f'\n#define CONFIG_SCREEN_TX_BATCH_DIAG {diag}\n'
                                  + '#include "'+str(SOURCE)+'"\n' + TEST)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-fsanitize=undefined', '-I', str(p),
                            str(p/'test.c'), '-o', str(p/'test')], check=True)
            subprocess.run([str(p/'test')], check=True)

if __name__ == '__main__':
    import sys
    if '--build-selection' in sys.argv:
        check_build_selection()
    else:
        main()
        check_build_selection()
