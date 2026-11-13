#!/usr/bin/env python3
from pathlib import Path
import sys,subprocess,tempfile
from test_patch_airplay_music_output import elf_object
T=Path(__file__).resolve().parent
A=T.parents[2]/'GCC-RELEASE/carplay_app/lib_Accessory2.a'
P='/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
expected={
 ('AirPlayScreen_SendScreenConfigure',0x14):'carbox_screen_time_config',
 ('AirPlayScreen_SendScreenNormalFrame',0x3e):'carbox_screen_time_video',
 ('AirPlayTimer_Start',4):'carbox_screen_time_start',
 ('AirPlayTimer_SetTimeRTCPData',0x32):'carbox_screen_time_receive',
 ('AirPlayTimer_SetTimeRTCPData',0x94):'carbox_screen_time_transmit',
 ('AirPlayTimer_Thread',0x120):'carbox_screen_timing_sendto'}
with tempfile.TemporaryDirectory() as td:
 out=Path(td)/'out.a'
 subprocess.run([sys.executable,str(T/'patch_screen_flow_evidence.py'),P+'ar',P+'objcopy',str(A),str(out)],check=True)
 count=0
 for member in ['AirPlayScreen.o','AirPlayTimer.o']:
  a,rels,_=elf_object(subprocess.check_output([P+'ar','p',str(A),member]))
  b,patched,_=elf_object(subprocess.check_output([P+'ar','p',str(out),member]))
  for n,v in a.items():
   if n.startswith('.text.'):assert b[n]==v,n
  for section,rs in rels.items():
   desired=[]
   for pc,kind,target in rs:
    key=(section.removeprefix('.rel.text.'),pc)
    if key in expected:target=expected[key];count+=1
    desired.append((pc,kind,target))
   assert patched[section]==desired,section
 assert count==6,count
 sentinel=Path(td)/'sentinel.a';sentinel.write_bytes(b'keep')
 r=subprocess.run([sys.executable,str(T/'patch_screen_flow_evidence.py'),P+'ar',P+'objcopy',str(out),str(sentinel)],capture_output=True)
 assert r.returncode and sentinel.read_bytes()==b'keep'
 print('screen flow six isolated relocations, instructions unchanged, duplicate rejected PASS')
