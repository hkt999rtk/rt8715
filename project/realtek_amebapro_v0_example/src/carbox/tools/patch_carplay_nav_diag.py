#!/usr/bin/env python3
"""Observation-only AirPlayUtils hooks. Reject unknown object/register layouts."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

SHA = '67600fdd873794d860e48f0b132bc2293fc831fcdd677b4465bfdfccfef2240c'
# 2026-11-03 customer mono bundle: native sections and semantic relocations
# audited identical; only compiler-generated symbol/section string names differ.
MONO_SHA = 'dfeac1e6c8f494e73e30942c98f29e762b781aa826b501010459dc907f11f1ad'
VENDOR_SHAS = (SHA, MONO_SHA)
D='_RTPJitterBufferDecodeThread'; R='RTPJitterBufferRead'; P='RTPJitterBufferPutBusyNode'
# id, function, call offset, original callee, number of stack arguments.
SITES = [
 (1,D,0x5c,'pthread_cond_wait',0), (2,D,0x7a,'dispatch_semaphore_wait',0),
 (3,D,0xa8,'dispatch_semaphore_wait',0), (4,D,0xe2,'AudioConverterFillComplexBuffer',2),
 (5,D,0x112,'dispatch_semaphore_wait',0), (6,D,0xae,'dispatch_semaphore_signal',0),
 (7,D,0x202,'dispatch_semaphore_signal',0), (8,R,0x1e,'dispatch_semaphore_wait',0),
 (9,R,0x54,'dispatch_semaphore_signal',0), (10,R,0x4e,'memset',0),
 (11,R,0xe6,'memset',0), (12,R,0x2c6,'memset',0), (13,R,0x32a,'memset',0),
 (14,R,0x284,'_RTPJitterBufferBufferedSamples',0), (15,P,0x12,'dispatch_semaphore_wait',0),
 (16,P,0x62,'dispatch_semaphore_signal',0), (17,P,0xa4,'pthread_cond_signal',0),
 (18,R,0x184,'pthread_cond_signal',0), (19,D,0x1c2,'_RTPJitterBufferBufferedSamples',0),
 (20,P,0x26,'_RTPJitterBufferDiscardExcess',0), (21,R,0x6e,'_RTPJitterBufferDiscardExcess',0),
 (22,D,0x50,'pthread_mutex_lock',0), (23,P,0x9c,'pthread_mutex_lock',0),
 (24,R,0x17c,'pthread_mutex_lock',0), (25,R,0x88,'_RTPJitterBufferBufferedSamples',0),
]
HOOKS=[dict(id=i,fn=f,off=o,callee=c,stack=n,name='carbox_nav_hook_%d'%i) for i,f,o,c,n in SITES]

def fail(s): raise SystemExit('Navigation diagnostic patch: '+s)

FIXED_SHA = 'c7c09e8a04277b6c70c2e1b66dd9fd3d564990c22fb960949bb29f6eec2a82e5'
MONO_FIXED_SHA = '77a187d24b9d20c2fa90a3ae7b442a5424fa76229b53058c925ac88661155c44'
FIXED_SHAS = (FIXED_SHA, MONO_FIXED_SHA)

def patch(path, fixed=False):
 b=bytearray(path.read_bytes()); h=struct.unpack_from('<16sHHIIIIIHHHHHH',b)
 if h[0][:6]!=b'\x7fELF\x01\x01' or h[1:3]!=(1,40) or h[11]!=40: fail('invalid ELF')
 ss=[struct.unpack_from('<10I',b,h[6]+40*i) for i in range(h[12])]
 def data(s): return b[s[4]:s[4]+s[5]]
 def string(blob,o): return blob[o:].split(b'\0',1)[0].decode('ascii')
 names=data(ss[h[13]]); secs={string(names,s[0]):(i,s) for i,s in enumerate(ss)}
 tabs=[(i,s) for i,s in enumerate(ss) if s[1]==2]
 if len(tabs)!=1: fail('symbol table')
 ti,tab=tabs[0]; strings=data(ss[tab[6]]); syms={}
 for i in range(tab[5]//16):
  off=tab[4]+i*16; rec=struct.unpack_from('<IIIBBH',b,off); name=string(strings,rec[0])
  if name: syms[name]=(i,off,rec)
 for x in HOOKS:
  if fixed and x['id']==21:
   # Production outer trampoline invokes this diagnostic shim itself.
   new=syms[x['name']]
   if new[2][5]!=0xfff1: fail('placeholder')
   struct.pack_into('<H',b,new[1]+14,0)
   continue
  si,sec=secs['.text.'+x['fn']]; _,rel=secs['.rel.text.'+x['fn']]
  if rel[1]!=9 or rel[6:8]!=(ti,si): fail('relocation layout')
  old=syms[x['callee']]; new=syms[x['name']]
  if new[2][5]!=0xfff1: fail('placeholder')
  matches=[off for off in range(rel[4],rel[4]+rel[5],8) if struct.unpack_from('<I',b,off)[0]==x['off']]
  if len(matches)!=1 or struct.unpack_from('<I',b,matches[0]+4)[0]!=(old[0]<<8)|10: fail('unexpected call '+str(x))
  if b[sec[4]+x['off']:sec[4]+x['off']+4]!=bytes.fromhex('fff7feff'): fail('unexpected instruction')
  struct.pack_into('<I',b,matches[0]+4,(new[0]<<8)|10); struct.pack_into('<H',b,new[1]+14,0)
 path.write_bytes(b)

def assembly():
 s='/* Generated from patch_carplay_nav_diag.py; r4 is the audited context.\n * Preserve native arguments/results and all callee-saved registers.\n * Converter arguments 5/6 remain at the callee stack top. */\n.syntax unified\n.thumb\n'
 for x in HOOKS:
  name=x['name']; s+=f'.section .text.{name},"ax",%progbits\n.align 2\n.global {name}\n.type {name},%function\n.thumb_func\n{name}:\n'
  s+=f' push {{r0-r12,lr}}\n mov r0,r4\n movs r1,#{x["id"]}\n mov r2,sp\n bl carbox_nav_before\n ldmia sp,{{r0-r3}}\n'
  if x['stack']:
   s+=' ldr r12,[sp,#56]\n sub sp,sp,#8\n str r12,[sp]\n ldr r12,[sp,#68]\n str r12,[sp,#4]\n'
  if x['callee'] in ('pthread_cond_wait','pthread_cond_signal'):
   target='carbox_nav_condition_'+('wait' if x['callee'].endswith('wait') else 'signal')
   s+=f'#if CONFIG_AUDIO_NAV_PLAYOUT\n bl {target}\n#else\n bl {x["callee"]}\n#endif\n'
  elif x['id'] in (4,11,13,21):
   target={4:'carbox_nav_convert_hook',11:'carbox_nav_future_hook',13:'carbox_nav_gap_hook',21:'carbox_nav_startup_purge'}[x['id']]
   s+=f'#if CONFIG_AUDIO_NAV_CONTINUITY\n bl {target}\n#else\n bl {x["callee"]}\n#endif\n'
  else:
   s+=f' bl {x["callee"]}\n'
  if x['stack']: s+=' add sp,sp,#8\n'
  s+=f' stmia sp,{{r0-r3}}\n mov r0,r4\n movs r1,#{x["id"]}\n mov r2,sp\n bl carbox_nav_after\n pop {{r0-r12,pc}}\n.size {name},.-{name}\n'
 return s+'.section .note.GNU-stack,"",%progbits\n'

def main(ar,objcopy,source,out):
 out=Path(out); vendor=subprocess.check_output([ar,'p',source,'AirPlayUtils.o'])
 digest=hashlib.sha256(vendor).hexdigest()
 if digest not in VENDOR_SHAS + FIXED_SHAS: fail('AirPlayUtils.o changed; re-audit before patching')
 out.parent.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(dir=out.parent,prefix='.nav-diag-') as d:
  d=Path(d); a=d/'archive.a';a.write_bytes(Path(source).read_bytes());p=d/'AirPlayUtils.o';p.write_bytes(vendor)
  args=[objcopy,'--globalize-symbol=_RTPJitterBufferBufferedSamples','--globalize-symbol=_RTPJitterBufferDiscardExcess']
  args+=['--add-symbol='+x['name']+'=0,global' for x in HOOKS]
  subprocess.run(args+[str(p)],check=True);patch(p, digest in FIXED_SHAS)
  subprocess.run([ar,'r',str(a),str(p)],check=True,stdout=subprocess.DEVNULL)
  subprocess.run([ar,'s',str(a)],check=True);os.replace(a,out)
 print('Navigation diagnostics: 25 probes; purge fix='+str(digest in FIXED_SHAS))
if __name__=='__main__':
 if sys.argv[1:]==['--assembly']: print(assembly(),end='')
 elif len(sys.argv)==5: main(*sys.argv[1:])
 else: fail('usage: AR OBJCOPY INPUT OUTPUT | --assembly')
