#!/usr/bin/env python3
"""v18只读边界观测：只改六个调用重定位，不改原始archive或任何指令。"""
import hashlib,os,pathlib,subprocess,sys,tempfile
from patch_screen_session import ELF,fail
HOOKS={
 'AirPlayScreen.o':[
  ('AirPlayScreen_SendScreenConfigure','734406708624d3a71b2064a1aa6dbd3f09108f9d40686d7f175fb0951de6762e',0x14,'AirPlayClock_VideoTimestamp','carbox_screen_time_config'),
  ('AirPlayScreen_SendScreenNormalFrame','9c6a0a097cf3110ecc4152a2a70ff82b799707e74af30cd581a59e523c54dfca',0x3e,'AirPlayClock_VideoTimestamp','carbox_screen_time_video')],
 'AirPlayTimer.o':[
  ('AirPlayTimer_Start','ec70422a9bf663e13f89d71e5a5c6f50ce3264c7985d4166b376bb357611b4fb',4,'AirPlayClock_StartTime','carbox_screen_time_start'),
  ('AirPlayTimer_SetTimeRTCPData','7b6eafced00e4caed995b0f073c2af3eab745c984dc0fb9b4422592930cc1e6a',0x32,'AirPlayClock_GetTime','carbox_screen_time_receive'),
  ('AirPlayTimer_SetTimeRTCPData','7b6eafced00e4caed995b0f073c2af3eab745c984dc0fb9b4422592930cc1e6a',0x94,'AirPlayClock_GetTime','carbox_screen_time_transmit'),
  ('AirPlayTimer_Thread','369d8749d21638fb4b402d20e4ff89bb823b178f740d3456c56f9fef33f04206',0x120,'lwip_sendto','carbox_screen_timing_sendto')]}
def main(ar,objcopy,source,output):
 source=pathlib.Path(source);output=pathlib.Path(output)
 if source.resolve()==output.resolve():fail('same input/output')
 with tempfile.TemporaryDirectory(dir=output.parent) as td:
  d=pathlib.Path(td);a=d/'a.a';a.write_bytes(source.read_bytes())
  for member,hooks in HOOKS.items():
   p=d/member;p.write_bytes(subprocess.check_output([ar,'p',str(source),member]));before=ELF(p.read_bytes())
   for f,h,pc,old,new in hooks:
    if hashlib.sha256(before.code(f)).hexdigest()!=h:fail('unreviewed v18 function '+f)
    if new in before.symbols:fail('already observed '+new)
   options=['--add-symbol='+new+'=0,global' for new in sorted({x[4] for x in hooks})]
   subprocess.run([objcopy,*options,str(p)],check=True);after=ELF(p.read_bytes())
   for f,h,pc,old,new in hooks:
    after.undefined(new);after.redirect(old,new,f,pc,10)
   for name,s in before.named.items():
    if name.startswith('.text.'):
     t=after.named[name]
     if before.b[s[4]:s[4]+s[5]]!=after.b[t[4]:t[4]+t[5]]:fail('instruction changed '+name)
    if s[1]==9 and s[5]!=after.named[name][5]:fail('relocation count changed')
   p.write_bytes(after.b);subprocess.run([ar,'r',str(a),str(p)],check=True)
  subprocess.run([ar,'s',str(a)],check=True);os.replace(a,output)
 print('screen flow: six v18 call relocations verified; instructions unchanged')
if __name__=='__main__':main(*sys.argv[1:])
