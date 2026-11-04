#!/usr/bin/env python3
"""Execute the actual Thumb pacing adapter on both handled/native return paths."""
from pathlib import Path
import subprocess,tempfile
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import *
from elftools.elf.elffile import ELFFile
P='/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
asm=Path(__file__).resolve().parents[1]/'audio_music_output_hooks.S'
with tempfile.TemporaryDirectory(prefix='nav-pacing-abi-') as t:
 d=Path(t);obj=d/'hooks.o';elfpath=d/'hooks.axf'
 subprocess.run([P+'gcc','-mcpu=cortex-m33','-mthumb','-DCONFIG_AUDIO_MUSIC_DIAG=1','-DCONFIG_AUDIO_NAV_PACING=1','-c',str(asm),'-o',str(obj)],check=True)
 names=['carbox_music_output_'+x+'_c' for x in ['init','push','pop','exit','sendto']]+['carbox_nav_output_pace_c','RingBuffer_size']
 stubs={n:0x20000+i*0x100 for i,n in enumerate(names)}
 subprocess.run([P+'ld','-Ttext=0x10000','-e','carbox_nav_output_pace',str(obj),'-o',str(elfpath)]+['--defsym='+n+'='+hex(a|1) for n,a in stubs.items()],check=True)
 with elfpath.open('rb') as f:
  e=ELFFile(f);code=e.get_section_by_name('.text').data();syms={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()}
 for handled in [0,1]:
  u=Uc(UC_ARCH_ARM,UC_MODE_THUMB);u.mem_map(0x10000,0x30000);u.mem_write(0x10000,code)
  for a in stubs.values():u.mem_write(a,b'\x70\x47')
  regs=[0x100+i for i in range(13)];stack=0x3f000;base=0x30000
  for i,v in enumerate(regs):u.reg_write(UC_ARM_REG_R0+i,v)
  u.reg_write(UC_ARM_REG_SP,stack);u.reg_write(UC_ARM_REG_LR,(base+0x11c)|1)
  seen=[];done=[]
  def hook(uc,addr,size,user):
   if addr in [base+0x90,base+0x11c]:done.append(addr);uc.emu_stop();return
   if addr==stubs['carbox_nav_output_pace_c']:
    assert uc.reg_read(UC_ARM_REG_SP)%8==0
    assert [uc.reg_read(UC_ARM_REG_R0+i) for i in range(3)]==[regs[0],regs[4],regs[5]]
    for i in [0,1,2,3,12]:uc.reg_write(UC_ARM_REG_R0+i,0xdead0000+i)
    uc.reg_write(UC_ARM_REG_R0,handled);seen.append('pace')
   if addr==stubs['RingBuffer_size']:
    assert not handled and uc.reg_read(UC_ARM_REG_R0)==regs[0]
    assert uc.reg_read(UC_ARM_REG_SP)==stack
    uc.reg_write(UC_ARM_REG_R0,9600);seen.append('native')
  u.hook_add(UC_HOOK_CODE,hook);u.emu_start(syms['carbox_nav_output_pace'],0,count=100)
  assert done==[base+(0x90 if handled else 0x11c)]
  assert seen==(['pace'] if handled else ['pace','native'])
  assert u.reg_read(UC_ARM_REG_SP)==stack
  for i in range(4,12):assert u.reg_read(UC_ARM_REG_R0+i)==regs[i]
  if not handled:assert u.reg_read(UC_ARM_REG_R0)==9600
print('PASS navigation pacing Thumb execution: C arguments, maximal caller clobber, stack alignment, loop/stop return and native fallback')
