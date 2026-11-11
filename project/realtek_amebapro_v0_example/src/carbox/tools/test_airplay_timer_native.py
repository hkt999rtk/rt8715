#!/usr/bin/env python3
"""执行原厂 Start/SetTime 指令，验证改入口后的真实 RTCP 秒/小数计算。"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                              UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC)
from test_airplay_timer_clock import P, SOURCE, SCRIPT
from test_patch_airplay_music_output import elf_object


def execute(sections, rels, captured_us, shared, samples):
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    u.mem_map(0x10000, 0x10000)
    calls = {}
    for fn, base in [('AirPlayTimer_Start', 0x10000),
                     ('AirPlayTimer_SetTimeRTCPData', 0x11000)]:
        code = bytearray(sections['.text.'+fn])
        for off, kind, sym in rels['.rel.text.'+fn]:
            # ELF helper renders STT_SECTION symbols as empty names. Exact
            # baseline literals were verified against the hash-guarded object.
            baseline_pc = 0x54 if fn == 'AirPlayTimer_Start' else 0xf0
            if kind == 2 and off == baseline_pc:
                struct.pack_into('<I', code, off, 0x18000)
            elif kind == 10:
                calls[base+off] = sym
        u.mem_write(base, bytes(code))
    regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
    times = iter(samples)
    seen = []

    def read_double(lo, hi):
        return struct.unpack('<d', struct.pack('<II', lo, hi))[0]

    def hook(uc, addr, size, _):
        if addr not in calls:
            return
        name = calls[addr]
        values = [uc.reg_read(r) for r in regs]
        seen.append(name)
        if name == 'carbox_airplay_timer_start_time':
            sec, usec = divmod(captured_us, 1000000)
            uc.mem_write(values[0], struct.pack('<II', 0 if shared else sec, usec))
            uc.reg_write(regs[0], 0)
        elif name == 'carbox_airplay_timer_gettimeofday':
            uc.mem_write(values[0], struct.pack('<II', *divmod(next(times), 1000000)))
            uc.reg_write(regs[0], 0)
        elif name == 'memset':
            uc.mem_write(values[0], bytes([values[1] & 255])*values[2])
        elif name in ('__aeabi_i2d', '__aeabi_dmul'):
            value = float(values[0]) if name == '__aeabi_i2d' else (
                read_double(*values[:2]) * read_double(*values[2:]))
            lo, hi = struct.unpack('<II', struct.pack('<d', value))
            uc.reg_write(regs[0], lo); uc.reg_write(regs[1], hi)
        elif name == '__aeabi_d2uiz':
            uc.reg_write(regs[0], int(read_double(*values[:2])))
        elif name == 'lwip_htonl':
            uc.reg_write(regs[0], int.from_bytes(values[0].to_bytes(4, 'little'), 'big'))
        else:
            raise AssertionError('unexpected native call '+name)
        uc.reg_write(UC_ARM_REG_PC, (addr+4)|1)

    u.hook_add(UC_HOOK_CODE, hook)
    u.reg_write(UC_ARM_REG_SP, 0x1e000)
    # Stop after native baseline store, before pthread initialization.
    u.emu_start(0x10001, 0x10012, count=50)
    baseline = struct.unpack('<I', u.mem_read(0x18000, 4))[0]
    assert baseline == (0 if shared else captured_us//1000000)
    u.reg_write(UC_ARM_REG_SP, 0x1e000)
    u.reg_write(UC_ARM_REG_R0, 0x19000)
    u.reg_write(UC_ARM_REG_LR, 0x1f001)
    u.emu_start(0x11001, 0x1f000, count=500)
    assert seen.count('carbox_airplay_timer_start_time') == 1
    assert seen.count('carbox_airplay_timer_gettimeofday') == 2
    assert u.reg_read(UC_ARM_REG_SP) == 0x1e000
    return bytes(u.mem_read(0x19000, 32))


class NativeTimerTest(unittest.TestCase):
    def test_origin_restart_and_fraction_across_second_boundary(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)/'timer.a'
            subprocess.run([sys.executable, str(SCRIPT), P+'ar', P+'objcopy',
                            str(SOURCE), str(out), '0'], check=True)
            sections, rels, _ = elf_object(subprocess.check_output([P+'ar', 'p', str(out), 'AirPlayTimer.o']))
            for captured in [2123456, 37876543, 3600234567]:
                for shared in [False, True]:
                    # 接收/发送采样跨越整数秒；不能把两个时间戳复制成同一个。
                    samples = [(captured//1000000+5)*1000000+999950,
                               (captured//1000000+6)*1000000+10]
                    packet = execute(sections, rels, captured, shared, samples)
                    self.assertEqual(packet[:4], b'\x80\xd3\x00\x07')
                    for off, sample in zip([16,24], samples):
                        sec, frac = struct.unpack_from('>II', packet, off)
                        wire_us = ((sec-0x83aa7e80) & 0xffffffff)*1000000 + frac*1000000/(1<<32)
                        expected = sample - (0 if shared else captured//1000000*1000000)
                        self.assertLess(abs(wire_us-expected), 1)


if __name__ == '__main__':
    unittest.main()
