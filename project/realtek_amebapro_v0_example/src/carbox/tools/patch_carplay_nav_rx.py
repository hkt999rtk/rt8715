#!/usr/bin/env python3
"""Observation-only AirPlayUtils hooks. Reject unknown object/register layouts."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

SHA = 'c50725995f091b48199b9eff5b5eabe6028201bd228082734fbbe5aef8a1b0cf'
F='_MainAltAudioThread'
SITES = [
 (1,F,0x20,'SetCurrentThreadPriority',0),
 (2,F,0x148,'SocketRecvFrom',6),
 (3,F,0x294,'chacha20_poly1305_verify',0),
 (4,F,0x160,'RTPJitterBufferPutFreeNode',0),
 (5,F,0x210,'RTPJitterBufferPutBusyNode',0),
 (6,F,0x1b4,'pthread_exit',0),
 (7,F,0xcc,'lwip_select',1),
]
HOOKS=[dict(id=i,fn=f,off=o,callee=c,stack=n,name='carbox_nav_app_hook_%d'%i) for i,f,o,c,n in SITES]

def fail(s): raise SystemExit('Navigation diagnostic patch: '+s)

def patch(path):
 b=bytearray(path.read_bytes()); h=struct.unpack_from('<16sHHIIIIIHHHHHH',b)
 if h[0][:6]!=b'\x7fELF\x01\x01' or h[1:3]!=(1,40) or h[11]!=40: fail('invalid ELF')
 ss=[struct.unpack_from('<10I',b,h[6]+40*i) for i in range(h[12])]
 def data(s): return b[s[4]:s[4]+s[5]]
 def string(blob,o): return blob[o:].split(b'\0',1)[0].decode('ascii')
 names=data(ss[h[13]]); secs={string(names,s[0]):(i,s) for i,s in enumerate(ss)}
 if hashlib.sha256(data(secs['.text.'+F][1])).hexdigest()!=SHA: fail('MainAltAudioThread code changed')
 tabs=[(i,s) for i,s in enumerate(ss) if s[1]==2]
 if len(tabs)!=1: fail('symbol table')
 ti,tab=tabs[0]; strings=data(ss[tab[6]]); syms={}
 for i in range(tab[5]//16):
  off=tab[4]+i*16; rec=struct.unpack_from('<IIIBBH',b,off); name=string(strings,rec[0])
  if name: syms[name]=(i,off,rec)
 for x in HOOKS:
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
 s='/* Generated from patch_carplay_nav_rx.py; r4 is the audited context.\n * Preserve native arguments/results and all callee-saved registers.\n * Forward all stack arguments; after callback receives saved input r1-r12 and return r0. */\n.syntax unified\n.thumb\n'
 for x in HOOKS:
  name=x['name']; s+=f'.section .text.{name},"ax",%progbits\n.align 2\n.global {name}\n.type {name},%function\n.thumb_func\n{name}:\n'
  s+=f' push {{r0-r12,lr}}\n mov r0,r4\n movs r1,#{x["id"]}\n mov r2,sp\n bl carbox_nav_app_before\n ldmia sp,{{r0-r3}}\n'
  stack_bytes=(x['stack']*4+7)&~7
  if stack_bytes:
   s+=f' sub sp,sp,#{stack_bytes}\n'
   for n in range(x['stack']):
    s+=f' ldr r12,[sp,#{stack_bytes+56+n*4}]\n str r12,[sp,#{n*4}]\n'
  s+=f' bl {x["callee"]}\n'
  if stack_bytes: s+=f' add sp,sp,#{stack_bytes}\n'
  s+=f' str r0,[sp]\n mov r0,r4\n movs r1,#{x["id"]}\n mov r2,sp\n bl carbox_nav_app_after\n pop {{r0-r12,pc}}\n.size {name},.-{name}\n'
 return s+'.section .note.GNU-stack,"",%progbits\n'

def main(ar,objcopy,source,out):
 out=Path(out); vendor=subprocess.check_output([ar,'p',source,'AirPlayReceiverSession.o'])
 out.parent.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(dir=out.parent,prefix='.nav-diag-') as d:
  d=Path(d); a=d/'archive.a';a.write_bytes(Path(source).read_bytes());p=d/'AirPlayReceiverSession.o';p.write_bytes(vendor)
  args=[objcopy]
  args+=['--add-symbol='+x['name']+'=0,global' for x in HOOKS]
  subprocess.run(args+[str(p)],check=True);patch(p)
  subprocess.run([ar,'r',str(a),str(p)],check=True,stdout=subprocess.DEVNULL)
  subprocess.run([ar,'s',str(a)],check=True);os.replace(a,out)
 print('Navigation RX diagnostics: 7 call-site probes, no packet processing changes')
if __name__=='__main__':
 if sys.argv[1:]==['--assembly']: print(assembly(),end='')
 elif len(sys.argv)==5: main(*sys.argv[1:])
 else: fail('usage: AR OBJCOPY INPUT OUTPUT | --assembly')
