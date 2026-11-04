#!/usr/bin/env python3
"""Validate production relocation changes and execute every trampoline in Unicorn."""
import importlib.util, subprocess, tempfile, io
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import *
H=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('patch',H/'patch_carplay_music_lifecycle.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
p='/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
source=H.parents[1]/'GCC-RELEASE/carplay_app/lib_CarPlay.a'
# H is src/carbox/tools: project root is H.parents[2].
source=H.parents[2]/'GCC-RELEASE/carplay_app/lib_CarPlay.a'
asm=H.parent/'audio_music_lifecycle_hooks.S';assert asm.read_text()==m.assembly()
def sections(blob):
 e=ELFFile(io.BytesIO(blob));return {s.name:s.data() for s in e.iter_sections() if s['sh_type']!='SHT_NOBITS'}
with tempfile.TemporaryDirectory() as d:
 d=Path(d);out=d/'patched.a';m.main(p+'ar',p+'objcopy',str(source),str(out))
 for member in {x['member'] for x in m.HOOKS}:
  a=subprocess.check_output([p+'ar','p',str(source),member]);b=subprocess.check_output([p+'ar','p',str(out),member]);sa=sections(a);sb=sections(b)
  for name,data in sa.items():
   if name.startswith('.text'):assert sb[name]==data, name
   if name.startswith('.rel'):
    assert len(sb[name])==len(data),name
    changed=[i for i in range(0,len(data),8) if sb[name][i:i+8]!=data[i:i+8]]
    expected=[x for x in m.HOOKS if x['member']==member and name=='.rel.text.'+x['fn']]
    assert len(changed)==len(expected),(name,changed,expected)
  # No double application: already-modified relocations must fail.
  f=d/member;f.write_bytes(b)
  try:m.patch(f,member)
  except SystemExit:pass
  else:raise AssertionError('double patch accepted')
 subprocess.run([p+'gcc','-mcpu=cortex-m33','-mthumb','-c',str(asm),'-o',str(d/'hooks.o')],check=True)
 names=['carbox_music_life_record']+sorted({x['callee'] for x in m.HOOKS})+['__real_'+f for _,f in m.WRAPS]
 syms={n:0x20000+i*0x100 for i,n in enumerate(names)}
 subprocess.run([p+'ld','-Ttext=0x10000','-e',m.HOOKS[0]['name'],str(d/'hooks.o'),'-o',str(d/'hooks.elf')]+['--defsym='+n+'='+hex(a|1) for n,a in syms.items()],check=True)
 with (d/'hooks.elf').open('rb') as f:
  e=ELFFile(f);code=e.get_section_by_name('.text').data();entries={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()}
 tests=[dict(x,tail=False) for x in m.HOOKS]+[dict(id=i,name='__wrap_'+f,callee='__real_'+f,tail=True) for i,f in m.WRAPS]
 for h in tests:
  u=Uc(UC_ARCH_ARM,UC_MODE_THUMB);u.mem_map(0x10000,0x30000);u.mem_write(0x10000,code)
  for a in syms.values():u.mem_write(a,b'\x70\x47')
  stop=0x30000;u.mem_write(stop,b'\x00\xbe');stack=0x3f000;initial=[0x100+i for i in range(13)]
  for i,v in enumerate(initial):u.reg_write(UC_ARM_REG_R0+i,v)
  u.reg_write(UC_ARM_REG_SP,stack);u.reg_write(UC_ARM_REG_LR,stop|1)
  extra=b''.join((0xabcdef00+i).to_bytes(4,'little') for i in range(8));u.mem_write(stack,extra)
  seen=[]
  def hook(uc,addr,size,user):
   if addr==stop:uc.emu_stop();return
   if addr not in syms.values():return
   name=next(n for n,a in syms.items() if a==addr);seen.append(name);sp=uc.reg_read(UC_ARM_REG_SP);assert not sp%8
   if name=='carbox_music_life_record':
    assert uc.reg_read(UC_ARM_REG_R0)==h['id'];phase=uc.reg_read(UC_ARM_REG_R1);r=uc.reg_read(UC_ARM_REG_R2)
    for i in range(4):assert int.from_bytes(uc.mem_read(r+4*i,4),'little')==(0x98760000+i if phase else initial[i])
    assert int.from_bytes(uc.mem_read(r+52,4),'little')==stop|1
    for i in [0,1,2,3,12]:uc.reg_write(UC_ARM_REG_R0+i,0xdead0000+i)
   else:
    assert name==h['callee']
    for i in range(4):assert uc.reg_read(UC_ARM_REG_R0+i)==initial[i]
    if h['tail']:
     assert sp==stack and bytes(uc.mem_read(sp,len(extra)))==extra
     assert uc.reg_read(UC_ARM_REG_LR)==stop|1
    for i in [0,1,2,3,12]:uc.reg_write(UC_ARM_REG_R0+i,0x98760000+i)
  u.hook_add(UC_HOOK_CODE,hook);u.emu_start(entries[h['name']],stop,count=500)
  assert seen==(['carbox_music_life_record',h['callee']] if h['tail'] else ['carbox_music_life_record',h['callee'],'carbox_music_life_record'])
  assert u.reg_read(UC_ARM_REG_SP)==stack
  for i in range(4):assert u.reg_read(UC_ARM_REG_R0+i)==0x98760000+i
  for i in range(4,12):assert u.reg_read(UC_ARM_REG_R0+i)==initial[i]
print('PASS: lifecycle instruction/relocation preservation; all 11 trampolines preserve ABI and int64 returns; tail wrappers preserve caller LR and stack args')
