#!/usr/bin/env python3
"""Execute every production Thumb trampoline with Unicorn. Check ABI forwarding."""
from pathlib import Path
import subprocess,tempfile,importlib.util
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import *
from elftools.elf.elffile import ELFFile
H=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('patch',H/'patch_carplay_music_progress.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
p='/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
asm=H.parent/'audio_music_progress_hooks.S'
assert asm.read_text()==m.assembly()
with tempfile.TemporaryDirectory() as d:
 d=Path(d);subprocess.run([p+'gcc','-mcpu=cortex-m33','-mthumb','-c',str(asm),'-o',str(d/'hook.o')],check=True)
 names=['carbox_music_progress_before','carbox_music_progress_after']+sorted({x['callee'] for x in m.HOOKS})
 syms={n:0x20000+i*0x100 for i,n in enumerate(names)}
 subprocess.run([p+'ld','-Ttext=0x10000','-e',m.HOOKS[0]['name'],str(d/'hook.o'),'-o',str(d/'hook.elf')]+['--defsym='+n+'='+hex(a|1) for n,a in syms.items()],check=True)
 with (d/'hook.elf').open('rb') as f:
  elf=ELFFile(f); sec=elf.get_section_by_name('.text'); code=sec.data(); tab=elf.get_section_by_name('.symtab');entries={s.name:s['st_value'] for s in tab.iter_symbols()}
 for h in [dict(x,result=v) for x in m.HOOKS for v in [0,0x1234,0xfffffff5]]:
  u=Uc(UC_ARCH_ARM,UC_MODE_THUMB);u.mem_map(0x10000,0x30000);u.mem_write(0x10000,code)
  for a in syms.values():u.mem_write(a,b'\x70\x47')
  stop=0x30000;u.mem_write(stop,b'\x00\xbe');stack=0x3f000
  initial=[0x100+i for i in range(13)]
  for i,v in enumerate(initial):u.reg_write(UC_ARM_REG_R0+i,v)
  u.reg_write(UC_ARM_REG_SP,stack);u.reg_write(UC_ARM_REG_LR,stop|1)
  u.mem_write(stack,b''.join((0xabcdef00+i).to_bytes(4,'little') for i in range(6)))
  seen=[]
  def hook(uc,addr,size,user):
   if addr==stop:uc.emu_stop();return
   if addr not in syms.values():return
   name=next(n for n,a in syms.items() if a==addr);seen.append(name)
   sp=uc.reg_read(UC_ARM_REG_SP);assert sp%8==0
   if name in ['carbox_music_progress_before','carbox_music_progress_after']:
    assert uc.reg_read(UC_ARM_REG_R0)==initial[h['ctx']]
    assert uc.reg_read(UC_ARM_REG_R1)==h['id']
    r=uc.reg_read(UC_ARM_REG_R2)
    assert int.from_bytes(uc.mem_read(r+24,4),'little')==initial[6]
    expected=initial[0] if name.endswith('before') else h['result']
    assert int.from_bytes(uc.mem_read(r,4),'little')==expected
    for i in range(4):uc.reg_write(UC_ARM_REG_R0+i,0xdead0000+i)
   else:
    assert name==h['callee']
    for i in range(4):assert uc.reg_read(UC_ARM_REG_R0+i)==initial[i]
    if h['stack']:assert bytes(uc.mem_read(sp,4*h['stack']))==bytes(uc.mem_read(stack,4*h['stack']))
    uc.reg_write(UC_ARM_REG_R0,h['result'])
  u.hook_add(UC_HOOK_CODE,hook);u.emu_start(entries[h['name']],stop,count=500)
  assert seen==['carbox_music_progress_before',h['callee'],'carbox_music_progress_after'],seen
  assert u.reg_read(UC_ARM_REG_R0)==h['result'] and u.reg_read(UC_ARM_REG_SP)==stack
  for i in range(4,12):assert u.reg_read(UC_ARM_REG_R0+i)==initial[i]
print('PASS: music probes preserve arguments/results and audited context registers; all six receive stack arguments forwarded')
