#!/usr/bin/env python3
"""Execute actual continuity shims: saved native state and converter stack ABI."""
from pathlib import Path
import subprocess,tempfile
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import *
from elftools.elf.elffile import ELFFile
P='/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
asm=Path(__file__).resolve().parents[1]/'audio_nav_continuity_hooks.S'
with tempfile.TemporaryDirectory() as td:
 d=Path(td);subprocess.run([P+'gcc','-mcpu=cortex-m33','-mthumb','-c',str(asm),'-o',str(d/'hook.o')],check=True)
 syms={'carbox_nav_convert_saved':0x20000,'carbox_nav_gap_saved':0x20100}
 subprocess.run([P+'ld','-Ttext=0x10000','-e','carbox_nav_convert_hook',str(d/'hook.o'),'-o',str(d/'hook.elf')]+['--defsym='+n+'='+hex(a|1) for n,a in syms.items()],check=True)
 with (d/'hook.elf').open('rb') as f:
  elf=ELFFile(f);code=elf.get_section_by_name('.text').data();entries={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
 for name,reason in [('carbox_nav_convert_hook',0),('carbox_nav_future_hook',11),('carbox_nav_gap_hook',13)]:
  u=Uc(UC_ARCH_ARM,UC_MODE_THUMB);u.mem_map(0x10000,0x30000);u.mem_write(0x10000,code)
  for a in syms.values():u.mem_write(a,b'\x70\x47')
  stack=0x3f000;stop=0x30000;initial=[0x100+i for i in range(13)]
  for i,v in enumerate(initial):u.reg_write(UC_ARM_REG_R0+i,v)
  u.reg_write(UC_ARM_REG_SP,stack);u.reg_write(UC_ARM_REG_LR,stop|1)
  extra=b'\x01\x02\x03\x04\x05\x06\x07\x08';u.mem_write(stack,extra);seen=[]
  def hook(uc,addr,size,user):
   if addr==stop:uc.emu_stop();return
   if addr not in syms.values():return
   assert addr==syms['carbox_nav_gap_saved' if reason else 'carbox_nav_convert_saved'];seen.append(addr)
   assert uc.reg_read(UC_ARM_REG_R0)==initial[4]
   r=uc.reg_read(UC_ARM_REG_R1);assert r%8==0
   assert list(int.from_bytes(uc.mem_read(r+i*4,4),'little') for i in range(13))==initial
   assert bytes(uc.mem_read(r+56,8))==extra
   if reason:assert uc.reg_read(UC_ARM_REG_R2)==reason
   # Simulate maximal caller-saved clobber by a real C function.
   for i in [0,1,2,3,12]:uc.reg_write(UC_ARM_REG_R0+i,0xdeadc000+i)
   uc.reg_write(UC_ARM_REG_R0,0x1234)
  u.hook_add(UC_HOOK_CODE,hook);u.emu_start(entries[name],stop,count=100)
  assert len(seen)==1 and u.reg_read(UC_ARM_REG_SP)==stack and u.reg_read(UC_ARM_REG_R0)==0x1234
  for i in range(1,13):assert u.reg_read(UC_ARM_REG_R0+i)==initial[i]
print('PASS: continuity saved registers, frame cursor registers, stack arguments and return ABI')
