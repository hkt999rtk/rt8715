#!/usr/bin/env python3
"""Check production printf lock modes without running ARM MMIO on the host."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[5]
source = root / 'component/soc/realtek/8195b/misc/utilities/source/ram/libc_wrap.c'
code = source.read_text()
code = code[code.index('static SemaphoreHandle_t printf_mutex = NULL;'):code.index('static int _sputc_cr(')]
# Only hardware register reads are replaced. Lock decisions / unlock are production code.
code = code.replace('(*(volatile uint32_t *)0xE000ED04)', 'mock_ipsr')
code = re.sub(r'static inline uint32_t printf_irq_mask\(void\)\n\{.*?\n\}',
              'static inline uint32_t printf_irq_mask(void) { return mock_mask; }', code, flags=re.S)
stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef void *SemaphoreHandle_t;
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define taskSCHEDULER_RUNNING 2
#define configASSERT(x) assert(x)
static uint32_t mock_ipsr, mock_mask;
static unsigned creates, takes, gives, critical, held;
static int scheduler=2, mutex;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() do { assert(critical); --critical; } while(0)
static void *xSemaphoreCreateMutex(void) { creates++; return &mutex; }
static int xTaskGetSchedulerState(void) { return scheduler; }
static int xSemaphoreTake(void *m, uint32_t wait) {
 assert(m==&mutex && wait==portMAX_DELAY);
 assert(!mock_ipsr && !mock_mask && !critical && scheduler==2 && !held);
 held=1; takes++; return pdPASS;
}
static int xSemaphoreGive(void *m) {
 assert(m==&mutex && held); held=0; gives++; return pdPASS;
}
'''
test = r'''
int main(void) {
 printf_lock_guard_t g=printf_lock();
 assert(g==PRINTF_GUARD_CRITICAL && critical==1 && takes==0);
 printf_unlock(g); assert(critical==0);
 mock_ipsr=29; g=printf_lock(); printf_unlock(g);
 assert(takes==0 && gives==0 && critical==0);
 mock_ipsr=0; printf_lock_init(); printf_lock_init(); assert(creates==1);
 g=printf_lock(); assert(g==PRINTF_GUARD_MUTEX && held);
 printf_unlock(g); assert(!held && takes==1 && gives==1);
 /* ISR, masks and suspended/not-started scheduler never wait for a logger. */
 held=1;
 for(unsigned mode=0;mode<6;mode++) {
  mock_ipsr=mode==0 ? 29:0;
  mock_mask=mode>=1 && mode<=3 ? (1U<<mode):0;
  scheduler=mode==4 ? 0 : mode==5 ? 1:2;
  g=printf_lock(); assert(g==PRINTF_GUARD_NONE); printf_unlock(g);
  assert(held && takes==1 && gives==1 && critical==0);
 }
 held=0; mock_ipsr=mock_mask=0; scheduler=2;
 /* Unlock follows the acquired mode even if the surrounding context changes. */
 g=printf_lock(); mock_mask=1; printf_unlock(g);
 assert(!held && takes==2 && gives==2);
 puts("PASS printf lock: early boot, initialized, ISR, IRQ masks, scheduler state, matching unlock");
}
'''
with tempfile.TemporaryDirectory(prefix='printf-lock-') as d:
 p=Path(d)
 (p/'test.c').write_text(stub+code+test)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                 str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
