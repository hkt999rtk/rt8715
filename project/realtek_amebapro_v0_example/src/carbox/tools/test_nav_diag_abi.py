#!/usr/bin/env python3
"""Execute every production Thumb trampoline with Unicorn. Check ABI forwarding."""
from pathlib import Path
import subprocess,tempfile,importlib.util,sys
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import *
from elftools.elf.elffile import ELFFile
H=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('patch',H/'patch_carplay_nav_diag.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
p='/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
asm=H.parent/'audio_nav_hooks.S'
assert asm.read_text()==m.assembly()
policy=int(sys.argv[1]) if len(sys.argv)>1 else 0
continuity=int(sys.argv[2]) if len(sys.argv)>2 else 0
if continuity:
 for h in m.HOOKS:
  if h['id'] in (4,11,13,21):h['callee']={4:'carbox_nav_convert_hook',11:'carbox_nav_future_hook',13:'carbox_nav_gap_hook',21:'carbox_nav_startup_purge'}[h['id']]
if policy:
 for h in m.HOOKS:
  if h['callee'] in ('pthread_cond_wait','pthread_cond_signal'):
   h['callee']='carbox_nav_condition_'+h['callee'].rsplit('_',1)[1]
with tempfile.TemporaryDirectory() as d:
 d=Path(d);subprocess.run([p+'gcc','-mcpu=cortex-m33','-mthumb','-DCONFIG_AUDIO_NAV_PLAYOUT='+str(policy),'-DCONFIG_AUDIO_NAV_CONTINUITY='+str(continuity),'-c',str(asm),'-o',str(d/'hook.o')],check=True)
 names=['carbox_nav_before','carbox_nav_after']+sorted({x['callee'] for x in m.HOOKS})
 syms={n:0x20000+i*0x100 for i,n in enumerate(names)}
 subprocess.run([p+'ld','-Ttext=0x10000','-e',m.HOOKS[0]['name'],str(d/'hook.o'),'-o',str(d/'hook.elf')]+['--defsym='+n+'='+hex(a|1) for n,a in syms.items()],check=True)
 with (d/'hook.elf').open('rb') as f:
  elf=ELFFile(f); sec=elf.get_section_by_name('.text'); code=sec.data(); tab=elf.get_section_by_name('.symtab');entries={s.name:s['st_value'] for s in tab.iter_symbols()}
 for h in m.HOOKS:
  u=Uc(UC_ARCH_ARM,UC_MODE_THUMB);u.mem_map(0x10000,0x30000);u.mem_write(0x10000,code)
  for a in syms.values():u.mem_write(a,b'\x70\x47')
  stop=0x30000;u.mem_write(stop,b'\x00\xbe');stack=0x3f000
  initial=[0x100+i for i in range(13)]
  for i,v in enumerate(initial):u.reg_write(UC_ARM_REG_R0+i,v)
  u.reg_write(UC_ARM_REG_SP,stack);u.reg_write(UC_ARM_REG_LR,stop|1)
  u.mem_write(stack,(0xabcdef01).to_bytes(4,'little')+(0x12345678).to_bytes(4,'little'))
  seen=[]
  def hook(uc,addr,size,user):
   if addr==stop:uc.emu_stop();return
   if addr not in syms.values():return
   name=next(n for n,a in syms.items() if a==addr);seen.append(name)
   sp=uc.reg_read(UC_ARM_REG_SP);assert sp%8==0
   if name in ['carbox_nav_before','carbox_nav_after']:
    assert uc.reg_read(UC_ARM_REG_R0)==initial[4]
    assert uc.reg_read(UC_ARM_REG_R1)==h['id']
    r=uc.reg_read(UC_ARM_REG_R2)
    assert int.from_bytes(uc.mem_read(r+24,4),'little')==initial[6]
    expected=initial[0] if name.endswith('before') else 0x1234
    assert int.from_bytes(uc.mem_read(r,4),'little')==expected
    for i in range(4):uc.reg_write(UC_ARM_REG_R0+i,0xdead0000+i)
   else:
    assert name==h['callee']
    for i in range(4):assert uc.reg_read(UC_ARM_REG_R0+i)==initial[i]
    if h['stack']:assert bytes(uc.mem_read(sp,8))==bytes(uc.mem_read(stack,8))
    uc.reg_write(UC_ARM_REG_R0,0x1234)
  u.hook_add(UC_HOOK_CODE,hook);u.emu_start(entries[h['name']],stop,count=500)
  assert seen==['carbox_nav_before',h['callee'],'carbox_nav_after'],seen
  assert u.reg_read(UC_ARM_REG_R0)==0x1234 and u.reg_read(UC_ARM_REG_SP)==stack
  for i in range(4,12):assert u.reg_read(UC_ARM_REG_R0+i)==initial[i]
print('PASS: all 25 Thumb probes preserve native arguments, result, callee-saved registers, SP and converter stack arguments')
