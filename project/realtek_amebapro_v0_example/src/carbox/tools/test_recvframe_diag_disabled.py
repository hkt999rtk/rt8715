#!/usr/bin/env python3
"""Verify diagnostics-off startup preserves the UART mutex and then exits."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / 'wlan_recvframe_diag.c'
stub = r'''
#include <assert.h>
#include <stddef.h>
#define CONFIG_RX_HANDOVER_DIAG 0
#define pdPASS 1
#define tskIDLE_PRIORITY 0
static unsigned initialized, deleted;
static void (*created_task)(void *);
void printf_lock_init(void) { ++initialized; }
static void vTaskDelete(void *task) { assert(task == NULL); ++deleted; }
static int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, void *handle) {
    (void)name;
    assert(stack == 1024 && !arg && priority == 1 && !handle);
    created_task = fn;
    return pdPASS;
}
'''
test = r'''
int main(void) {
    carbox_wlan_recvframe_diag_start();
    assert(created_task && initialized == 0);
    created_task(NULL);
    assert(initialized == 1 && deleted == 1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='recvframe-diag-off-') as directory:
    path = Path(directory)
    for header in ('FreeRTOS.h', 'task.h'):
        (path / header).write_text('')
    (path / 'test.c').write_text(stub + '\n#include "' + str(source) + '"\n' + test)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-I', str(path), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
    symbols = subprocess.check_output(['nm', str(path / 'test')], text=True)
    assert '__wrap_rtw_queue_empty' not in symbols
    assert 'recvframe_stats' not in symbols
print('PASS diagnostics off: no RX observer/state; UART mutex initialized; task exits')
