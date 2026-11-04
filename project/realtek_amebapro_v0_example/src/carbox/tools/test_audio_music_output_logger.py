#!/usr/bin/env python3
"""Run maximum-width output records through the actual shared UART formatter."""
from pathlib import Path
import re
import subprocess
import tempfile
from test_audio_music_output import SOURCE, STUB

native_stubs = STUB.split('void carbox_rxhd_log(',1)[0]
native_stubs = re.sub(r'uint32_t carbox_diag_log_skipped\(void\) \{[^}]*\}', '', native_stubs)
logger_stubs = r'''
#define CONFIG_RX_HANDOVER_DIAG 0
#define CONFIG_AUDIO_NAV_DIAG 0
#define pdPASS 1
typedef void *SemaphoreHandle_t;
static unsigned held, busy, line_bytes, max_line, max_format;
static int mutex;
void *printf_lock_handle(void) { return &mutex; }
static int xSemaphoreTake(void *m, unsigned wait) {
 assert(m==&mutex && wait==0 && !critical && !held);
 if(busy) return 0;
 held=1; return pdPASS;
}
static int xSemaphoreGive(void *m) {
 assert(m==&mutex && held && !critical); held=0; return pdPASS;
}
static void vTaskDelay(unsigned n) { assert(n==1 && !held && !critical); tick+=n; }
static int putc_mock(void *arg, char c) {
 assert(!arg && held && !critical && used+1<sizeof(output));
 output[used++]=c; output[used]=0; line_bytes++;
 if(c=='\n') {
  assert(line_bytes<=160 && output[used-2]=='\r');
  if(line_bytes>max_line) max_line=line_bytes;
  line_bytes=0;
 }
 return 1;
}
static unsigned core_mock(int (*putc_fn)(void *,char), void *arg,
                          const char *fmt, va_list ap) {
 char buffer[2048];
 assert(!held && !critical);
 int n=vsnprintf(buffer,sizeof(buffer),fmt,ap);
 assert(n>0 && n<768);
 if((unsigned)n>max_format) max_format=n;
 for(int i=0;i<n;i++) putc_fn(arg,buffer[i]);
 return n;
}
static const struct {
 unsigned (*printf_core)(int (*)(void *,char),void *,const char *,va_list);
 int (*stdio_port_sputc)(void *,char);
} stdio_printf_stubs={core_mock,putc_mock};
'''
test = r'''
int main(void) {
 memset(&outputs[0],0xff,sizeof(outputs[0]));
 outputs[0].inflight=0;
 busy=1; carbox_audio_music_output_report();
 assert(!used && outputs[0].dirty && !outputs[0].reported);
 busy=0;
 uint32_t skipped=carbox_diag_log_skipped();
 carbox_audio_music_output_report();
 assert(used && carbox_diag_log_skipped()==skipped && outputs[0].reported);
 assert(strstr(output,"wraps=4294967295") && strstr(output,"udp_drop=4294967295"));
 unsigned chunks=0;
 const char *prefix="[MUSIC_OUT] epoch=4294967295 ";
 for(char *p=output;*p;) {
  assert(!strncmp(p,prefix,strlen(prefix)));
  p=strchr(p,'\n'); assert(p); p++; chunks++;
 }
 assert(chunks>1 && !critical && !held && !line_bytes);
 printf("PASS music output real logger: max_format=%u<768, max_line=%u<=160, chunks=%u, final retained on contention\n",max_format,max_line,chunks);
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='music-output-logger-') as d:
        p=Path(d)
        (p/'lwip').mkdir()
        for name in ('FreeRTOS.h','task.h','semphr.h','rt_printf.h','hal_timer.h','lwip/sockets.h'):
            (p/name).write_text('')
        logger=SOURCE.with_name('rx_handover_diag.c')
        (p/'test.c').write_text(native_stubs+logger_stubs+'\n#include "'+str(logger)+'"\n'
                              +'#include "'+str(SOURCE)+'"\n'+test)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                        '-Wno-unused-function','-Wno-unused-variable',
                        '-fsanitize=undefined','-fno-pie','-no-pie','-I',str(p),
                        str(p/'test.c'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)


if __name__=='__main__':
    main()
