#!/usr/bin/env python3
"""Execute native Thumb timestamp writes, including the long-idle regression."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R4, UC_ARM_REG_SP, UC_ARM_REG_PC
from test_patch_airplay_music_output import elf_object

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT.parents[1] / 'GCC-RELEASE'
P = '/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
SCRIPT = ROOT/'tools/patch_screen_current_timestamp.py'
FN = 'AirPlayScreen_SendScreenNormalFrame'

def execute(code, relocs, old, current):
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    u.mem_map(0x10000, 0x10000)
    u.mem_write(0x10000, code)
    u.reg_write(UC_ARM_REG_R4, 0x18000)
    u.reg_write(UC_ARM_REG_SP, 0x19000)
    u.mem_write(0x18000, struct.pack('<Q', old))
    u.mem_write(0x19000, b'\xa5'*128)
    calls = {pc: name for pc, kind, name in relocs if kind == 10 and name in ('UpTicks', 'UpTicksToNTP')}
    seen = []
    def hook(uc, addr, size, _):
        pc = addr-0x10000
        if pc not in calls: return
        seen.append(calls[pc])
        value = 123 if calls[pc] == 'UpTicks' else current
        uc.reg_write(UC_ARM_REG_R0, value & 0xffffffff)
        uc.reg_write(UC_ARM_REG_R1, value >> 32)
        uc.reg_write(UC_ARM_REG_PC, (addr+4)|1)
    u.hook_add(UC_HOOK_CODE, hook)
    u.emu_start(0x10041, 0x10054, count=30)
    assert seen == ['UpTicks', 'UpTicksToNTP']
    assert u.reg_read(UC_ARM_REG_R4) == 0x18000
    assert u.reg_read(UC_ARM_REG_SP) == 0x19000
    stack = bytes(u.mem_read(0x19000,128))
    assert stack[:16] == b'\xa5'*16 and stack[24:] == b'\xa5'*104
    return struct.unpack('<Q',stack[16:24])[0], struct.unpack('<Q',u.mem_read(0x18000,8))[0]

class TimestampTest(unittest.TestCase):
    def test_archive_and_executed_timestamp(self):
        # 从已提交的原厂库生成派生输入，避免依赖先前诊断构建的缓存。
        fixtures = tempfile.TemporaryDirectory()
        self.addCleanup(fixtures.cleanup)
        fixture_dir = Path(fixtures.name)
        vendor = BUILD/'carplay_app/lib_Accessory2.a'
        resource = fixture_dir/'resource450.a'
        evidence = fixture_dir/'evidence.a'
        clock = fixture_dir/'evidence_mono_p1.a'
        for script, source, output, extra in [
            ('patch_carplay_resource_450.py', vendor, resource, []),
            ('patch_screen_session.py', resource, evidence, ['--evidence-only']),
            ('patch_airplay_timer_clock.py', evidence, clock, ['1']),
        ]:
            subprocess.run([sys.executable, str(ROOT/'tools'/script),
                            P+'ar', P+'objcopy', str(source), str(output), *extra],
                           check=True)
        sources = [vendor, resource, clock]
        for source in sources:
            original = source.read_bytes()
            a = subprocess.check_output([P+'ar','p',str(source),'AirPlayScreen.o'])
            sections, rels, _ = elf_object(a)
            code = sections['.text.'+FN]; calls = rels['.rel.text.'+FN]
            # Reproduce the old bug, including a 5-second idle/reconfiguration.
            for old, now in [(1<<32, (1<<32)+100000), (1<<32, 6<<32), (20<<32,21<<32)]:
                self.assertEqual(execute(code,calls,old,now), (old,now))
            self.assertTrue(SCRIPT.exists(), 'current-frame timestamp patch missing')
            with tempfile.TemporaryDirectory() as td:
                out=Path(td)/'out.a'
                subprocess.run([sys.executable,str(SCRIPT),P+'ar',str(source),str(out)],check=True)
                b=subprocess.check_output([P+'ar','p',str(out),'AirPlayScreen.o'])
                new,newrels,_=elf_object(b)
                for old,now in [(1<<32,(1<<32)+100000),(1<<32,6<<32),(20<<32,21<<32),((1<<64)-5,7)]:
                    self.assertEqual(execute(new['.text.'+FN],newrels['.rel.text.'+FN],old,now),(now,now))
                for name,data in sections.items():
                    if name == '.text.'+FN:
                        self.assertEqual(data[:0x40],new[name][:0x40]); self.assertEqual(data[0x54:],new[name][0x54:])
                    elif name != '.rel.text.'+FN:
                        self.assertEqual(data,new[name],name)
                expected=[(pc-4 if pc in (0x48,0x4c) else pc,k,n) for pc,k,n in calls]
                self.assertEqual(newrels['.rel.text.'+FN],expected)
                members=subprocess.check_output([P+'ar','t',str(source)]).decode().splitlines()
                self.assertEqual(members,subprocess.check_output([P+'ar','t',str(out)]).decode().splitlines())
                for member in members:
                    if member != 'AirPlayScreen.o':
                        self.assertEqual(subprocess.check_output([P+'ar','p',str(source),member]),subprocess.check_output([P+'ar','p',str(out),member]))
                keep=Path(td)/'keep.a'; keep.write_bytes(b'keep')
                result=subprocess.run([sys.executable,str(SCRIPT),P+'ar',str(out),str(keep)],capture_output=True)
                self.assertNotEqual(result.returncode,0); self.assertEqual(keep.read_bytes(),b'keep')
                result=subprocess.run([sys.executable,str(SCRIPT),P+'ar',str(source),str(source)],capture_output=True)
                self.assertNotEqual(result.returncode,0)
            self.assertEqual(source.read_bytes(),original)

if __name__=='__main__': unittest.main()
