#!/usr/bin/env python3
"""Check exact-name task priority override using the production wrapper."""
from pathlib import Path
import re
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / 'gcd_sync_profiler.c'
code = source.read_text().split('#endif /* task-priority override enabled */')[0]
code += '\n#endif\n'
code = re.sub(r'^#include[^\n]*', '', code, flags=re.M)
stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define configSTACK_DEPTH_TYPE unsigned
#define configMAX_PRIORITIES 12
#define CONFIG_AUDIO_OUTPUT_TASK_PRIORITY 5
#define CONFIG_AUDIO_DECODER_TASK_PRIORITY 6
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
static unsigned got;
BaseType_t __real_xTaskCreate(TaskFunction_t f, const char *n,
 unsigned stack, void *p, unsigned prio, TaskHandle_t *out) {
 assert(!f && stack==1024 && !p && !out); (void)n; got=prio; return 1;
}
'''
test = r'''
int main(void) {
 const char *names[]={"AudioOutput", "ScreenThread", "AudioOutputOther", "AudioOut", "AirPlayAudioDecoder", NULL};
 unsigned expected[]={5,4,4,4,6,4};
 for(unsigned i=0;i<sizeof(expected)/sizeof(expected[0]);i++) {
  assert(__wrap_xTaskCreate(NULL,names[i],1024,NULL,4,NULL)==1);
  assert(got==expected[i]);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='audio-priority-') as d:
    p = Path(d)
    (p/'test.c').write_text(stub+code+test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS priority: only exact AudioOutput=5, decoder=6, screen/other preserved')
