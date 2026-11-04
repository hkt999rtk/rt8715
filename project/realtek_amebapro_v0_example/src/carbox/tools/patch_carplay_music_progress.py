#!/usr/bin/env python3
"""Observation-only AirPlayUtils hooks. Reject unknown object/register layouts."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

HASHES = {
 '_GeneralAudioThread':'3d008a4d029088bc3a54e31040adb76fd92c2fabde5b3e9ab39cf5ce824e8ed5',
 '_GeneralAudioReceiveRTP':'1ff0fc66a478bd0577debc74a82831584e07ef3360849a4451ce3cf5bc308418',
 'AirPlayReceiverSessionReadAudio':'2dc40d6d6c2edc9cccf686f4bea55e4f993df891f37530f404be98dc1b92de19',
}
T='_GeneralAudioThread'; R='_GeneralAudioReceiveRTP'; P='AirPlayReceiverSessionReadAudio'
SITES = [
 (1,T,0x36,'SetCurrentThreadPriority',0,5),
 (2,R,0xfa,'SocketRecvFrom',6,4),
 (3,T,0xea,'lwip_select',1,5),
 (4,P,0x4c,'_SessionLock.isra.1',0,4),
 (5,P,0x60e,'_SessionUnlock.isra.2',0,4),
 (6,P,0x354,'_GeneralAudioDecryptPacket',3,4),
 (7,P,0x3c2,'_GeneralAudioDecryptPacket',3,4),
 (8,P,0x374,'_AudioDecoderDecodeFrame.constprop.13',2,4),
 (9,P,0x3f8,'_AudioDecoderDecodeFrame.constprop.13',2,4),
 (10,P,0xe8,'memset',0,4),
 (11,P,0x4c0,'memset',0,4),
 (12,P,0x53c,'memcpy',0,4),
 (13,T,0x188,'pthread_exit',0,5),
 (14,R,0xcc,'_SessionUnlock.isra.2',0,4),
]
HOOKS=[dict(id=i,fn=f,off=o,callee=c,stack=n,ctx=r,name='carbox_music_progress_hook_%d'%i) for i,f,o,c,n,r in SITES]

def fail(s): raise SystemExit('Music progress patch: '+s)

def patch(path):
 b=bytearray(path.read_bytes()); h=struct.unpack_from('<16sHHIIIIIHHHHHH',b)
 if h[0][:6]!=b'\x7fELF\x01\x01' or h[1:3]!=(1,40) or h[11]!=40: fail('invalid ELF')
 ss=[struct.unpack_from('<10I',b,h[6]+40*i) for i in range(h[12])]
 def data(s): return b[s[4]:s[4]+s[5]]
 def string(blob,o): return blob[o:].split(b'\0',1)[0].decode('ascii')
 names=data(ss[h[13]]); secs={string(names,s[0]):(i,s) for i,s in enumerate(ss)}
 for fn,digest in HASHES.items():
  if hashlib.sha256(data(secs['.text.'+fn][1])).hexdigest()!=digest: fail(fn+' code changed')
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
 s='/* Generated from patch_carplay_music_progress.py; each context register is audited.\n * Preserve native arguments/results and all callee-saved registers.\n * Forward all stack arguments; after callback receives saved input r1-r12 and return r0. */\n.syntax unified\n.thumb\n'
 for x in HOOKS:
  name=x['name']; s+=f'.section .text.{name},"ax",%progbits\n.align 2\n.global {name}\n.type {name},%function\n.thumb_func\n{name}:\n'
  s+=f' push {{r0-r12,lr}}\n mov r0,r{x["ctx"]}\n movs r1,#{x["id"]}\n mov r2,sp\n bl carbox_music_progress_before\n ldmia sp,{{r0-r3}}\n'
  stack_bytes=(x['stack']*4+7)&~7
  if stack_bytes:
   s+=f' sub sp,sp,#{stack_bytes}\n'
   for n in range(x['stack']):
    s+=f' ldr r12,[sp,#{stack_bytes+56+n*4}]\n str r12,[sp,#{n*4}]\n'
  s+=f' bl {x["callee"]}\n'
  if stack_bytes: s+=f' add sp,sp,#{stack_bytes}\n'
  s+=f' str r0,[sp]\n mov r0,r{x["ctx"]}\n movs r1,#{x["id"]}\n mov r2,sp\n bl carbox_music_progress_after\n pop {{r0-r12,pc}}\n.size {name},.-{name}\n'
 return s+'.section .note.GNU-stack,"",%progbits\n'

def main(ar,objcopy,source,out):
 out=Path(out); vendor=subprocess.check_output([ar,'p',source,'AirPlayReceiverSession.o'])
 out.parent.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(dir=out.parent,prefix='.nav-diag-') as d:
  d=Path(d); a=d/'archive.a';a.write_bytes(Path(source).read_bytes());p=d/'AirPlayReceiverSession.o';p.write_bytes(vendor)
  args=[objcopy]
  args+=['--globalize-symbol='+n for n in sorted({x['callee'] for x in HOOKS if x['callee'].startswith('_')})]
  args+=['--add-symbol='+x['name']+'=0,global' for x in HOOKS]
  subprocess.run(args+[str(p)],check=True);patch(p)
  subprocess.run([ar,'r',str(a),str(p)],check=True,stdout=subprocess.DEVNULL)
  subprocess.run([ar,'s',str(a)],check=True);os.replace(a,out)
 print('Music progress diagnostics: 14 call-site probes, no packet processing changes')
if __name__=='__main__':
 if sys.argv[1:]==['--assembly']: print(assembly(),end='')
 elif len(sys.argv)==5: main(*sys.argv[1:])
 else: fail('usage: AR OBJCOPY INPUT OUTPUT | --assembly')
