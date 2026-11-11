#!/usr/bin/env python3
"""Exercise the real clock code and the isolated customer object substitution."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from test_patch_airplay_music_output import elf_object

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT.parents[1] / 'GCC-RELEASE'
P = '/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
SCRIPT = ROOT / 'tools/patch_airplay_timer_clock.py'
SOURCE = BUILD / 'carplay_app/lib_Accessory2.a'
STUBS = r'''
#ifndef TEST_STUBS_H
#define TEST_STUBS_H
#include <stdint.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>
typedef struct { int32_t xOverflowCount; uint32_t xTimeOnEntering; } TimeOut_t;
#define configTICK_RATE_HZ 1000U
void enter_critical(void);
void exit_critical(void);
#define taskENTER_CRITICAL() enter_critical()
#define taskEXIT_CRITICAL() exit_critical()
void vTaskSetTimeOutState(TimeOut_t *);
uint32_t xTaskGetTickCount(void);
uint32_t hal_read_curtime_us(void);
int rt_printf(const char *, ...);
ssize_t lwip_sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
#endif
'''

class ClockTest(unittest.TestCase):
    def test_clock_survives_wall_step_rollovers_and_idle(self):
        self.assertTrue((ROOT/'airplay_timer_clock.c').exists(), 'isolated timing adapter missing')
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            (d/'lwip').mkdir()
            (d/'stubs.h').write_text(STUBS)
            for name in ['FreeRTOS.h', 'task.h', 'hal_timer.h', 'diag.h', 'lwip/sockets.h']:
                (d/name).write_text('#include "stubs.h"\n')
            for profile, origin, video in [(p,o,v) for p in (0,1) for o in (0,1) for v in (0,1)]:
                cmd = ['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                       '-DCONFIG_SCREEN_HW_TIMESTAMP=1', '-DCONFIG_IAP2_DEVICE_TIME_SYNC=1',
                       '-DCONFIG_AIRPLAY_TIMER_MONOTONIC=1',
                       '-DCONFIG_SCREEN_TIMER_ORIGIN='+str(video),
                       '-DCONFIG_AIRPLAY_TIMER_SHARED_ORIGIN='+str(origin),
                       '-DCONFIG_AIRPLAY_TIMER_CLOCK_PROFILE='+str(profile),
                       '-I'+str(d), '-I'+str(ROOT),
                       str(ROOT/'screen_hw_timestamp.c'), str(ROOT/'airplay_timer_clock.c'),
                       str(ROOT/'iap2_device_time_sync.c'), str(ROOT/'tools/airplay_timer_clock_test.c'),
                       '-o', str(d/'test')]
                subprocess.run(cmd, check=True)
                subprocess.run([str(d/'test')], check=True, env={**os.environ, 'TZ':'UTC'})

    def test_archive_changes_only_timer_imports(self):
        self.assertTrue(SCRIPT.exists(), 'isolated archive substitution missing')
        original = SOURCE.read_bytes()
        for observe in [0, 1]:
            with tempfile.TemporaryDirectory() as td:
                output = Path(td)/'out.a'
                subprocess.run([sys.executable, str(SCRIPT), P+'ar', P+'objcopy', str(SOURCE), str(output), str(observe)], check=True)
                members = subprocess.check_output([P+'ar', 't', str(SOURCE)]).splitlines()
                self.assertEqual(members, subprocess.check_output([P+'ar', 't', str(output)]).splitlines())
                replacements = {'gettimeofday': 'carbox_airplay_timer_gettimeofday'}
                if observe:
                    replacements['lwip_sendto'] = 'carbox_airplay_timer_sendto'
                for member in members:
                    a = subprocess.check_output([P+'ar', 'p', str(SOURCE), member.decode()])
                    b = subprocess.check_output([P+'ar', 'p', str(output), member.decode()])
                    if member not in ([b'AirPlayTimer.o', b'AirPlayScreen.o'] if observe else [b'AirPlayTimer.o']):
                        self.assertEqual(a, b, member)
                        continue
                    sections, rels, _ = elf_object(a)
                    patched, new_rels, syms = elf_object(b)
                    for name, value in sections.items():
                        if name.startswith(('.text', '.data', '.rodata')):
                            self.assertEqual(value, patched[name], name)
                    expected = {n:[(off,kind,replacements.get(sym,sym) if member==b'AirPlayTimer.o' else sym) for off,kind,sym in rows] for n,rows in rels.items()}
                    if member==b'AirPlayScreen.o':
                        key='.rel.text.AirPlayScreen_SendScreenConfigure'
                        expected[key]=[(off,kind,'carbox_airplay_clock_config_send' if sym=='AirPlayScreen_SendScreenData' else sym) for off,kind,sym in expected[key]]
                        self.assertEqual(expected,new_rels)
                        continue
                    key='.rel.text.AirPlayTimer_Start'
                    expected[key]=[(off,kind,'carbox_airplay_timer_start_time' if sym=='carbox_airplay_timer_gettimeofday' else sym) for off,kind,sym in expected[key]]
                    self.assertEqual(expected,new_rels)
                    self.assertEqual(sum(s=='gettimeofday' for rows in rels.values() for _,_,s in rows), 3)
                    self.assertEqual(syms['carbox_airplay_timer_gettimeofday'][5], 0)
                # An unsupported/already patched object must not replace an existing artifact.
                keep = Path(td)/'keep.a'; keep.write_bytes(b'keep')
                result = subprocess.run([sys.executable, str(SCRIPT), P+'ar', P+'objcopy', str(output), str(keep), str(observe)], capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(keep.read_bytes(), b'keep')
                result = subprocess.run([sys.executable, str(SCRIPT), P+'ar', P+'objcopy', str(SOURCE), str(SOURCE), str(observe)], capture_output=True)
                self.assertNotEqual(result.returncode, 0)
        self.assertEqual(SOURCE.read_bytes(), original)

if __name__ == '__main__':
    unittest.main()
