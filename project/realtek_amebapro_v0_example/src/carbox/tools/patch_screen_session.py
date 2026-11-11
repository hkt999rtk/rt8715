#!/usr/bin/env python3
"""Validated v17 symbol/relocation hooks. Never overwrite customer archives or function sections."""
import hashlib, os, pathlib, struct, subprocess, sys, tempfile
HASHES = {'AirPlayScreen.o': {'AirPlayScreen_StartVideo': 'e3ed8ff4877ccbe6995447b533ecddadc541b810a17baad25f0b159157a135e1',
                     'AirPlayScreen_StopVideo': '6954f326b09c7b40e1c3b9223b3f3fb84df945daa67cc3e96457b3e79897ca99',
                     'AirPlayScreen_StartScreen': '27322ff8222550d8ed482712cb4b574f5014ee9866fe73ec4ffcff4bfeeb22c8',
                     'AirPlayScreen_ScreenThread': 'bdbfee3b004f45c7fdb3a7fe3583142454bd0f4e89790926d538e917cf0245be',
                     'AirPlayScreen_SendScreenNormalFrame': '9c6a0a097cf3110ecc4152a2a70ff82b799707e74af30cd581a59e523c54dfca',
                     'AirPlayScreen_SendScreenData': 'ae8b172a01d1f41202c2b64580147de53a4a07b628376ade4eed2ca2ec71492b',
                     'AirPlayScreen_SendVideo': '7f62b4119cdf46fd83e763427f94ae8707b893a7da4edf2cb9a272cd1ddbf696',
                     'AirPlayScreen_EncryptData': '728d6b686efb31a07ebc1468cbc3cbac86184afebf6906bccdcdd4c425f1aeae'},
 'AirPlayResponse.o': {'AirPlayResponse_GetInfoInfoHIDDevices': '92674ccfa8b51f4d798f690c67e5f9f38ce02ea1b30a386a2bfdccaddf5fa4f9',
                       'AirPlayResponse_GetInfoInfoResponse': '0b0a50bc2eb0edc16787ba2766c5cb4aaa2e2028ef005688ff1d9536680a94bc'},
 'AirPlayClient.o': {'_HTTPClientCompletionFunc': '1cb1882364180a18285dbe3e37a37cf513054a4f01bdade1a609babf04b4ec3f',
                     'AirPlayClient_SendMessageSync': '21ace38ea54e88dc2a5f0c83e0a26c99d3aba60581edeea71414660a96c6b136',
                     'AirPlayClient_SendMessageRecord': 'b0aa592b6f17a19f0961b508b08678259cdbc83e64f96e6745415fd63713920a'}}

def fail(s): raise SystemExit('screen session patch: '+s)
class ELF:
 def __init__(self,b):
  self.b=bytearray(b);e=struct.unpack_from('<16sHHIIIIIHHHHHH',b)
  if e[0][:6]!=b'\x7fELF\x01\x01' or e[1:3]!=(1,40) or e[11]!=40:fail('expected ARM ELF32 relocatable')
  self.sections=[struct.unpack_from('<10I',b,e[6]+40*i) for i in range(e[12])]
  def data(s):return b[s[4]:s[4]+s[5]]
  def string(s,o):return s[o:s.index(0,o)].decode()
  names=data(self.sections[e[13]]);self.named={string(names,s[0]):s for s in self.sections}
  st=next(s for s in self.sections if s[1]==2);strings=data(self.sections[st[6]])
  self.symbols={};self.indices={}
  for i,p in enumerate(range(st[4],st[4]+st[5],16)):
   r=struct.unpack_from('<IIIBBH',b,p);n=string(strings,r[0]);self.indices[i]=(n,p,r)
   if n:self.symbols[n]=(i,p,r)
 def code(self,f):
  s=self.named['.text.'+f];return self.b[s[4]:s[4]+s[5]]
 def undefined(self,n):
  _,p,r=self.symbols[n]
  if r[5]!=0xfff1:fail('hook placeholder not ABS: '+n)
  struct.pack_into('<H',self.b,p+14,0)
 def redirect(self,old,new,section=None,pc=None,kind=None):
  oi=self.symbols[old][0];ni=self.symbols[new][0];found=0
  for name,s in self.named.items():
   if s[1]!=9 or (section and name!='.rel.text.'+section):continue
   for p in range(s[4],s[4]+s[5],8):
    at,info=struct.unpack_from('<II',self.b,p)
    if info>>8==oi and (pc is None or pc==at):
     if kind is not None and info&255!=kind:fail('unexpected relocation kind')
     struct.pack_into('<I',self.b,p+4,(ni<<8)|(info&255));found+=1
  if not found or (pc is not None and found!=1):fail('missing/ambiguous hook '+old)
 def validate(self,member):
  for f,h in HASHES[member].items():
   if hashlib.sha256(self.code(f)).hexdigest()!=h:fail('unknown v17 function '+f)
  if any(n.startswith('carbox_vendor_screen_') or n=='carbox_info_plist' for n in self.symbols):fail('already patched')

def main(ar,objcopy,source,output,mode=None):
 if mode not in (None,"--evidence-only"):fail("unknown mode")
 evidence_only=mode=="--evidence-only"
 source=pathlib.Path(source);output=pathlib.Path(output)
 if source.resolve()==output.resolve():fail('input and output must differ')
 with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
  d=pathlib.Path(tmp);archive=d/'archive.a';archive.write_bytes(source.read_bytes())
  for member in HASHES:
   p=d/member;p.write_bytes(subprocess.check_output([ar,'p',str(source),member]))
   e=ELF(p.read_bytes());e.validate(member)
   options=[];calls=[];aliases={}
   if member=='AirPlayScreen.o':
    if not evidence_only:
     for n,size in {'bScreenSend':1,'sockScreen':4,'screenFrames':4,'mutexScreen':96}.items():
      if e.symbols[n][2][2]!=size:fail('incompatible state ABI: '+n)
     state={'bScreenSend':'carbox_screen_sending','sockScreen':'carbox_screen_sock','screenFrames':'carbox_screen_frames','mutexScreen':'carbox_screen_mutex'}
     for old,new in state.items():options+=['--globalize-symbol='+new,'--redefine-sym='+old+'='+new]
     aliases={'AirPlayScreen_StartVideo':'carbox_vendor_screen_start','AirPlayScreen_StopVideo':'carbox_vendor_screen_stop','AirPlayScreen_SendScreenData':'carbox_vendor_screen_data','AirPlayScreen_SendVideo':'carbox_vendor_screen_video'}
     for old,new in {'lwip_write':'carbox_screen_write','lwip_poll':'carbox_screen_poll','pthread_attr_setdetachstate':'carbox_screen_detach','pthread_create':'carbox_screen_create','pthread_exit':'carbox_screen_thread_exit'}.items():
      if e.symbols[old][2][5]!=0:fail('expected undefined '+old)
      options+=['--redefine-sym='+old+'='+new]
    # EncryptData is observed before the customer's prefix rewrite, without altering bytes.
    s=e.named['.rel.text.AirPlayScreen_SendScreenNormalFrame']
    hits=[]
    for off in range(s[4],s[4]+s[5],8):
     pc,info=struct.unpack_from('<II',e.b,off)
     if e.indices[info>>8][0]=='AirPlayScreen_EncryptData':hits.append(pc)
    if len(hits)!=1:fail('EncryptData call changed')
    calls=[('AirPlayScreen_SendScreenNormalFrame',hits[0],10,'AirPlayScreen_EncryptData','carbox_screen_encrypt_observe')]
   elif member=='AirPlayResponse.o':
    calls=[('AirPlayResponse_GetInfoInfoResponse',24,10,'CFBinaryPlistV0CreateWithData','carbox_info_plist'),('AirPlayResponse_GetInfoInfoHIDDevices',20,10,'CFDictionaryGetTypedValue','carbox_info_hid_lookup')]
   else:
    options+=['--globalize-symbol=carbox_vendor_info_complete','--redefine-sym=_HTTPClientCompletionFunc=carbox_vendor_info_complete','--redefine-sym=HTTPClientSendMessage=carbox_info_send']
    calls=[('AirPlayClient_SendMessageSync',152,2,'carbox_vendor_info_complete','carbox_info_complete'),('AirPlayClient_SendMessageRecord',220,2,'carbox_vendor_info_complete','carbox_info_complete')]
   for old,new in aliases.items():options+=['--redefine-sym='+old+'='+new,'--add-symbol='+old+'=0,global']
   for new in sorted({c[4] for c in calls}):options+=['--add-symbol='+new+'=0,global']
   subprocess.run([objcopy,*options,str(p)],check=True)
   updated=ELF(p.read_bytes())
   for old,new in aliases.items():
    updated.undefined(old)
    # Internal calls must also reach the new implementation, not the renamed vendor copy.
    if old=='AirPlayScreen_SendScreenData':updated.redirect(new,old)
   for f,pc,kind,old,new in calls:updated.redirect(old,new,f,pc,kind)
   for new in sorted({c[4] for c in calls}):updated.undefined(new)
   # Every instruction byte and relocation table size survives; only names/bindings/targets change.
   for name,s in e.named.items():
    if name.startswith('.text.'):
     t=updated.named[name]
     if e.b[s[4]:s[4]+s[5]]!=updated.b[t[4]:t[4]+t[5]]:fail('instruction changed '+name)
    if s[1]==9 and s[5]!=updated.named[name][5]:fail('relocations dropped '+name)
   p.write_bytes(updated.b);subprocess.run([ar,'r',str(archive),str(p)],check=True)
  subprocess.run([ar,'s',str(archive)],check=True);os.replace(archive,output)
 print('screen session: verified v18 ABI; lifecycle_fix=%d; read-only /info/NAL evidence' % (not evidence_only))
if __name__=='__main__':main(*sys.argv[1:])
