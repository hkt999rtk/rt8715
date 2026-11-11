#!/usr/bin/env python3
"""Native relocation preservation and executable forwarding/restart checks."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from test_patch_airplay_music_output import elf_object

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT.parents[1] / 'GCC-RELEASE'
PREFIX = '/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
SOURCE = BUILD / 'carplay_app/lib_Accessory2.a'
SCRIPT = ROOT / 'tools/patch_audio_tx_evidence.py'
STUB = r'''
#ifndef AUDIO_TEST_STUB
#define AUDIO_TEST_STUB
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
void enter_critical(void);
void exit_critical(void);
#define taskENTER_CRITICAL() enter_critical()
#define taskEXIT_CRITICAL() exit_critical()
uint32_t hal_read_curtime_us(void);
int rt_printf(const char *, ...);
ssize_t lwip_sendto(int,const void *,size_t,int,const struct sockaddr *,socklen_t);
#endif
'''

class AudioEvidenceTest(unittest.TestCase):
    def test_forwarding_rollover_failures_and_restarts(self):
        self.assertTrue((ROOT/'audio_tx_evidence.c').exists())
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            (d/'lwip').mkdir()
            (d/'stubs.h').write_text(STUB)
            for name in ['FreeRTOS.h', 'task.h', 'hal_timer.h', 'diag.h', 'lwip/sockets.h']:
                (d/name).write_text('#include "stubs.h"\n')
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                            '-DCONFIG_AUDIO_TX_EVIDENCE=1', '-I'+str(d), '-I'+str(ROOT),
                            str(ROOT/'tools/audio_tx_evidence_test.c'), '-o', str(d/'test')], check=True)
            subprocess.run([str(d/'test')], check=True)
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                            '-DCONFIG_AUDIO_TX_EVIDENCE=0', '-I'+str(d), '-I'+str(ROOT),
                            '-c', str(ROOT/'audio_tx_evidence.c'), '-o', str(d/'off.o')], check=True)

    def test_archive_only_changes_reviewed_audio_calls(self):
        self.assertTrue(SCRIPT.exists())
        from patch_audio_tx_evidence import HOOKS, SHA256
        raw = subprocess.check_output([PREFIX+'ar', 'p', str(SOURCE), 'AirPlayAudio.o'])
        self.assertEqual(hashlib.sha256(raw).hexdigest(), SHA256)
        original = SOURCE.read_bytes()
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)/'out.a'
            subprocess.run([sys.executable, str(SCRIPT), PREFIX+'ar', PREFIX+'objcopy', str(SOURCE), str(out)], check=True)
            members = subprocess.check_output([PREFIX+'ar', 't', str(SOURCE)]).splitlines()
            self.assertEqual(members, subprocess.check_output([PREFIX+'ar', 't', str(out)]).splitlines())
            for m in members:
                a = subprocess.check_output([PREFIX+'ar', 'p', str(SOURCE), m.decode()])
                b = subprocess.check_output([PREFIX+'ar', 'p', str(out), m.decode()])
                if m != b'AirPlayAudio.o':
                    self.assertEqual(a,b,m)
                    continue
                sections, rels, _ = elf_object(a)
                patched, changed, symbols = elf_object(b)
                for n,v in sections.items():
                    if n.startswith(('.text', '.data', '.rodata')):
                        self.assertEqual(v,patched[n],n)
                lookup = {('.rel.text.'+fn,pc,old):new for fn,pc,old,new in HOOKS}
                expected = {n:[(pc,kind,lookup.get((n,pc,sym),sym)) for pc,kind,sym in rows] for n,rows in rels.items()}
                self.assertEqual(expected,changed)
                self.assertEqual(sum(a!=b for n in rels for a,b in zip(rels[n],changed[n])),7)
                for _,_,_,name in HOOKS:
                    self.assertEqual(symbols[name][5],0)
            keep = Path(td)/'keep.a'; keep.write_bytes(b'keep')
            result = subprocess.run([sys.executable,str(SCRIPT),PREFIX+'ar',PREFIX+'objcopy',str(out),str(keep)],capture_output=True)
            self.assertNotEqual(result.returncode,0)
            self.assertEqual(keep.read_bytes(),b'keep')
            result = subprocess.run([sys.executable,str(SCRIPT),PREFIX+'ar',PREFIX+'objcopy',str(SOURCE),str(SOURCE)],capture_output=True)
            self.assertNotEqual(result.returncode,0)
        self.assertEqual(original,SOURCE.read_bytes())

if __name__ == '__main__':
    unittest.main()
