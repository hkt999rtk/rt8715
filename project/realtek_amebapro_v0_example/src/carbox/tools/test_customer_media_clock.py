#!/usr/bin/env python3
"""Run the shipped v19 Thumb code, with only HAL/RTOS/libc calls simulated."""
import io
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
P = '/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
ARCHIVE = Path(__file__).resolve().parents[3] / 'GCC-RELEASE/carplay_app/lib_Accessory2.a'
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]

class Machine:
    def __init__(self, elf_path):
        self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.u.reg_write(UC_ARM_REG_C1_C0_2, 0xf << 20)
        self.u.reg_write(UC_ARM_REG_FPEXC, 1 << 30)
        self.u.mem_map(0x10000, 0x90000)
        elf = ELFFile(io.BytesIO(elf_path.read_bytes()))
        for segment in elf.iter_segments():
            if segment['p_type'] == 'PT_LOAD':
                self.u.mem_write(segment['p_vaddr'], segment.data())
        self.syms = {s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name}
        self.externs = {self.syms[n]&~1:n for n in STUBS}
        self.depth = 0
        self.now = 0
        self.step = 0
        self.captured = None
        self.mode = None
        self.hal_calls = 0
        self.u.hook_add(UC_HOOK_CODE, self.hook)

    def hook(self, u, pc, size, user):
        if self.mode == 'config' and pc == (self.syms['AirPlayScreen_SendScreenData']&~1):
            self.captured = bytes(u.mem_read(u.reg_read(REGS[0]), 128))
            u.emu_stop(); return
        if pc not in self.externs: return
        name = self.externs[pc]
        r = [u.reg_read(reg) for reg in REGS]
        def set64(n, first=0):
            u.reg_write(REGS[first], n&0xffffffff); u.reg_write(REGS[first+1], (n>>32)&0xffffffff)
        if name == 'vTaskSetTimeOutState':
            ticks = self.now//1000
            u.mem_write(r[0], struct.pack('<II', ticks>>32, ticks&0xffffffff))
        elif name == 'hal_read_curtime_us':
            u.reg_write(REGS[0], self.now&0xffffffff)
            self.now += self.step; self.hal_calls += 1
        elif name == 'vPortEnterCritical': self.depth += 1
        elif name == 'vPortExitCritical':
            self.depth -= 1; assert self.depth >= 0
        elif name == '__aeabi_uldivmod':
            q, rem = divmod(r[0]|r[1]<<32, r[2]|r[3]<<32)
            set64(q); set64(rem,2)
        elif name in ('__aeabi_i2d','__aeabi_dmul','__aeabi_d2uiz'):
            def dbl(a,b): return struct.unpack('<d',struct.pack('<II',a,b))[0]
            if name == '__aeabi_d2uiz': u.reg_write(REGS[0],int(dbl(r[0],r[1])))
            else:
                v = float(r[0] if r[0]<0x80000000 else r[0]-0x100000000) if name=='__aeabi_i2d' else dbl(r[0],r[1])*dbl(r[2],r[3])
                set64(struct.unpack('<Q',struct.pack('<d',v))[0])
        elif name == 'lwip_htonl': u.reg_write(REGS[0],int.from_bytes(r[0].to_bytes(4,'little'),'big'))
        elif name == 'memset': u.mem_write(r[0],bytes([r[1]&255])*r[2])
        elif name == 'memcpy':
            if self.mode == 'normal' and r[2] == 128:
                self.captured = bytes(u.mem_read(r[1],128)); u.emu_stop(); return
            u.mem_write(r[0],bytes(u.mem_read(r[1],r[2])))
        elif name == 'malloc': u.reg_write(REGS[0],0x70000)
        elif name.startswith('pthread_'): u.reg_write(REGS[0],0)
        else: raise AssertionError('Unexpected external call '+name)
        u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))

    def call(self, name, *args):
        u=self.u
        u.reg_write(UC_ARM_REG_SP,0x90000)
        u.reg_write(UC_ARM_REG_LR,0x9f001)
        for i,reg in enumerate(REGS): u.reg_write(reg,args[i] if i<len(args) else 0)
        u.emu_start(self.syms[name]|1,0x9f000,count=100000)
        assert self.depth==0
        return u.reg_read(REGS[0])|u.reg_read(REGS[1])<<32

    def start(self, us):
        self.now=us; self.call('AirPlayTimer_Start')
        baseline=struct.unpack('<I',self.u.mem_read(self.syms['start_sec'],4))[0]
        assert baseline==us//1000000

    def video(self, config=False):
        self.mode='config' if config else 'normal'; self.captured=None
        self.u.mem_write(self.syms['bScreenSend'],b'\1')
        self.u.mem_write(self.syms['sockScreen'],struct.pack('<I',5))
        self.u.mem_write(self.syms['bFirstVideoFrame'],b'\0')
        self.u.mem_write(0x60000,b'\0\0\0\1\x67\x42\0\x1f'+b'\0'*32)
        if config: self.call('AirPlayScreen_SendScreenConfigure',0x60000,8,0x60000,8)
        else: self.call('AirPlayScreen_SendScreenNormalFrame',0x60000,8)
        self.mode=None
        assert self.captured is not None
        return struct.unpack_from('<Q',self.captured,8)[0]

    def timing(self):
        self.u.mem_write(0x90000,b'\0'*20)
        self.u.mem_write(0x90000+12,struct.pack('>II',123,456))
        self.call('AirPlayTimer_SetTimeRTCPData',0x60000)
        data=bytes(self.u.mem_read(0x60000,32))
        assert data[:4]==b'\x80\xd3\0\7'
        assert data[8:16]==struct.pack('>II',123,456)
        return [((s-0x83aa7e80)&0xffffffff)*1000000+f*1000000/(1<<32) for s,f in [struct.unpack_from('>II',data,off) for off in (16,24)]]

def us(ntp): return (ntp>>32)*1000000+(ntp&0xffffffff)*1000000/(1<<32)

class NativeClockTest(unittest.TestCase):
    def test_shipped_timer_and_screen_clock(self):
        global STUBS
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); objects=[]; defs=set(); refs=set()
            for member in ('AirPlayTimer.o','AirPlayScreen.o','AirPlayClock.o'):
                b=subprocess.check_output([P+'ar','p',str(ARCHIVE),member]); obj=d/member;obj.write_bytes(b);objects.append(str(obj))
                elf=ELFFile(io.BytesIO(b))
                for s in elf.get_section_by_name('.symtab').iter_symbols():
                    if s.name and s['st_info']['bind']!='STB_LOCAL':
                        (refs if s['st_shndx']=='SHN_UNDEF' else defs).add(s.name)
            STUBS=refs-defs
            (d/'stub.S').write_text('.syntax unified\n.thumb\n'+''.join('.section .text.stub_'+n+',"ax"\n.global '+n+'\n.thumb_func\n'+n+':\n bx lr\n' for n in sorted(STUBS)))
            subprocess.run([P+'gcc','-mthumb','-march=armv8-m.main+dsp','-c',str(d/'stub.S'),'-o',str(d/'stub.o')],check=True)
            subprocess.run([P+'ld','-Ttext=0x10000','-e','AirPlayTimer_Start',*objects,str(d/'stub.o'),'-o',str(d/'test.elf')],check=True)
            m=Machine(d/'test.elf')
            for start,samples in [(2641000,[2650000,5000123]),(25750000,[25750000,26000123,(1<<32)-1,(1<<32)+123,4*(1<<32)+876543,((1<<32)+123)*1000])]:
                m.start(start)
                for sample in samples:
                    m.now=sample; expected=sample-start//1000000*1000000
                    for config in (False,True): self.assertLess(abs(us(m.video(config))-expected),1)
                    rx,tx=m.timing();self.assertLess(abs(rx-expected),1);self.assertLess(abs(tx-expected),1)
            m.start(m.now)
            m.step=10
            rx,tx=m.timing(); self.assertGreater(tx,rx)
            print('Native v19: current headers, RTCP epoch/fraction, integer origin, reconnect, HW/RTOS wraps PASS')

if __name__=='__main__':unittest.main()
