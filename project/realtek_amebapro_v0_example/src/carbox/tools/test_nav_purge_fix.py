#!/usr/bin/env python3
"""Exercise actual vendor Read prologue/continuation around the production fix.

Checks native regression, PCM/encoded replacement, no removal, and complete
purge with a retired head. Also checks relocation preservation and
rejects an unsupported object. Requires pyelftools and Unicorn.
"""
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import sys
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import *

HERE=Path(__file__).resolve().parent
PREFIX='/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
spec=importlib.util.spec_from_file_location('purge_patch',HERE/'patch_carplay_nav_purge.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
original=Path(sys.argv[1]) if len(sys.argv)>1 else Path('/tmp/carplay_jitter_analysis/AirPlayUtils.o')
assert hashlib.sha256(original.read_bytes()).hexdigest() in m.VENDOR_SHAS

def readelf(path):
    with path.open('rb') as f:
        e=ELFFile(f); syms=e.get_section_by_name('.symtab')
        code=e.get_section_by_name('.text.RTPJitterBufferRead').data()
        rel=e.get_section_by_name('.rel.text.RTPJitterBufferRead')
        calls={r['r_offset']:syms.get_symbol(r['r_info_sym']).name for r in rel.iter_relocations() if r['r_info_type']==10}
        relocations=[]
        for s in e.iter_sections():
            if s['sh_type']=='SHT_REL':
                table=e.get_section(s['sh_link'])
                relocations += [(s.name,r['r_offset'],r['r_info_type'],table.get_symbol(r['r_info_sym']).name) for r in s.iter_relocations()]
        return code,calls,relocations

def bl(pc,target):
    offset=target-(pc+4)
    assert offset%2==0 and -(1<<24)<=offset<(1<<24)
    v=offset&0x1ffffff; s=(v>>24)&1; i1=(v>>23)&1;i2=(v>>22)&1
    return struct.pack('<HH',0xf000|(s<<10)|((v>>12)&0x3ff),0xd000|((1^(i1^s))<<13)|((1^(i2^s))<<11)|((v>>1)&0x7ff))

with tempfile.TemporaryDirectory() as td:
    td=Path(td); obj=td/'AirPlayUtils.o';obj.write_bytes(original.read_bytes())
    subprocess.run([PREFIX+'objcopy','--globalize-symbol='+m.NATIVE,'--add-symbol='+m.NAME+'=0,global',str(obj)],check=True)
    before=readelf(obj);m.patch(obj);after=readelf(obj)
    assert before[0]==after[0], 'instruction bytes changed'
    changed=[(x,y) for x,y in zip(before[2],after[2]) if x!=y]
    assert len(before[2])==len(after[2]) and changed==[(('.rel.text.RTPJitterBufferRead',0x6e,10,m.NATIVE),('.rel.text.RTPJitterBufferRead',0x6e,10,m.NAME))],changed
    try: m.patch(obj)
    except SystemExit: pass
    else: raise AssertionError('repatch must fail closed')
    for diag in (0,1):
        subprocess.run([PREFIX+'gcc','-mcpu=cortex-m33','-mthumb','-DCONFIG_AUDIO_NAV_DIAG='+str(diag),'-c',str(HERE.parent/'audio_nav_purge_fix.S'),'-o',str(td/'fix.o')],check=True)
        subprocess.run([PREFIX+'ld','-Ttext=0x18000','-e',m.NAME,str(td/'fix.o'),'-o',str(td/'fix.elf'),'--defsym='+m.NATIVE+'=0x20001','--defsym=carbox_nav_hook_21=0x20101'],check=True)
        with (td/'fix.elf').open('rb') as f:
            e=ELFFile(f);fix=e.get_section_by_name('.text').data();entry=next(s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols() if s.name==m.NAME)
        for mode in ('unchanged','next_pcm','encoded_only','empty'):
            for patched in (False,True):
                u=Uc(UC_ARCH_ARM,UC_MODE_THUMB)
                u.mem_map(0x10000,0x50000)
                base=0x10000;ctx=0x30000;old=0x31000;new=0x32000;out=0x33000;stack=0x5f000;stop=0x25000
                raw=bytearray(before[0]);calls=after[1] if patched else before[1]
                names=sorted(set(calls.values())-{m.NAME,m.NATIVE})
                stubs={name:0x21000+i*0x100 for i,name in enumerate(names)}
                stubs[m.NATIVE]=0x20000;stubs['carbox_nav_hook_21']=0x20100;stubs[m.NAME]=entry&~1
                for off,name in calls.items(): raw[off:off+4]=bl(base+off,stubs[name])
                u.mem_write(base,bytes(raw));u.mem_write(0x18000,fix)
                for name,addr in stubs.items():
                    if name!=m.NAME:u.mem_write(addr,b'\x70\x47')
                def put(addr,v):u.mem_write(addr,struct.pack('<I',v))
                def get(addr):return struct.unpack('<I',u.mem_read(addr,4))[0]
                put(ctx,0x1234);put(ctx+0x138,old);put(ctx+0x140,0);put(ctx+0x188,4)
                put(ctx+0x150,44100);put(ctx+0x1c0,1);put(ctx+0x1c4,0)
                u.mem_write(ctx+0x1c9,b'\x01');put(old+12,0);put(new+12,480)
                u.mem_write(out,b'\xaa'*1920)
                initial={UC_ARM_REG_R4+i:0xa000+i for i in range(8)}
                for reg,v in initial.items():u.reg_write(reg,v)
                u.reg_write(UC_ARM_REG_R0,ctx);u.reg_write(UC_ARM_REG_R1,out);u.reg_write(UC_ARM_REG_R2,1920)
                u.reg_write(UC_ARM_REG_SP,stack);u.reg_write(UC_ARM_REG_LR,stop|1)
                seen=[];point=[]
                def hook(uc,addr,size,_):
                    if addr in (stop,base+0xbe):point.append(addr);uc.emu_stop();return
                    name=next((n for n,a in stubs.items() if a==addr),None)
                    if not name or name==m.NAME:return
                    seen.append(name)
                    assert uc.reg_read(UC_ARM_REG_SP)%8==0
                    if name in (m.NATIVE,'carbox_nav_hook_21'):
                        if mode=='next_pcm':put(ctx+0x138,new)
                        elif mode=='encoded_only':put(ctx+0x138,0);put(ctx+0x140,new)
                        elif mode=='empty':put(ctx+0x138,0);put(ctx+0x140,0)
                        for i in range(4):uc.reg_write(UC_ARM_REG_R0+i,0xb000+i)
                    elif name=='UpTicks':uc.reg_write(UC_ARM_REG_R0,100);uc.reg_write(UC_ARM_REG_R1,0)
                    elif name=='_RTPJitterBufferBufferedSamples':uc.reg_write(UC_ARM_REG_R0,480)
                    elif name=='memset':
                        dst=uc.reg_read(UC_ARM_REG_R0);val=uc.reg_read(UC_ARM_REG_R1);n=uc.reg_read(UC_ARM_REG_R2)
                        assert (dst,val,n)==(out,0,1920)
                        uc.mem_write(dst,bytes([val])*n)
                u.hook_add(UC_HOOK_CODE,hook);u.emu_start(base|1,0,count=2000)
                purge_name='carbox_nav_hook_21' if patched and diag else m.NATIVE
                assert seen.count(purge_name)==1,seen
                if patched and mode=='empty':
                    assert point==[stop] and u.reg_read(UC_ARM_REG_SP)==stack
                    assert bytes(u.mem_read(out,1920))==bytes(1920)
                    assert u.mem_read(ctx+0x1c9,1)==b'\x01'
                    assert 'dispatch_semaphore_signal' in seen
                    for reg,v in initial.items():assert u.reg_read(reg)==v
                else:
                    assert point==[base+0xbe],point
                    expected=480 if patched and mode in ('next_pcm','encoded_only') else 0
                    assert u.reg_read(UC_ARM_REG_R5)==expected,(diag,mode,patched,u.reg_read(UC_ARM_REG_R5))
                    assert u.reg_read(UC_ARM_REG_R8)==480
                    assert u.mem_read(ctx+0x1c9,1)==b'\x00'
                    assert bytes(u.mem_read(out,1920))==b'\xaa'*1920
print('PASS: only audited call relocation changed; actual Read stale-head regression reproduced; PCM/encoded refresh, no purge and empty wait verified with diagnostics on/off')
