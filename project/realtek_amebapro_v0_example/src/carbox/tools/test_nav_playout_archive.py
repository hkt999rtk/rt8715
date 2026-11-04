#!/usr/bin/env python3
"""Verify member isolation, unchanged code/data and unknown-layout rejection."""
import hashlib,subprocess,tempfile
from pathlib import Path
from patch_carplay_nav_playout import main,validate,sections
B=Path(__file__).resolve().parents[3]/'GCC-RELEASE'
V=B/'carplay_app/lib_CarPlay.a'
P='/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
def member(a,n):return subprocess.check_output([P+'ar','p',str(a),n])
native=member(V,'AirPlayUtils.o')
with tempfile.TemporaryDirectory() as td:
 out=Path(td)/'out.a'
 for source in (V,B/'carplay_app/build/lib_CarPlay_nav_diag.a'):
  main(P+'ar',P+'objcopy',str(V),str(source),str(out))
  names=subprocess.check_output([P+'ar','t',str(source)],text=True).splitlines()
  assert names==subprocess.check_output([P+'ar','t',str(out)],text=True).splitlines()
  for name in names:
   if name!='AirPlayUtils.o':assert member(source,name)==member(out,name),name
  obj=member(out,'AirPlayUtils.o');validate(native,obj)
  f=Path(td)/'out.o';f.write_bytes(obj)
  undefined=subprocess.check_output([P+'nm','-u',str(f)],text=True).splitlines()
  assert any(l.endswith('carbox_nav_condition_signal') for l in undefined)
  assert not any(l.endswith(' pthread_cond_signal') or l.endswith(' pthread_cond_wait') for l in undefined)
  changed=bytearray(obj);sec=sections(obj)['.text.RTPJitterBufferInit'];changed[sec[4]]^=1
  try:validate(native,bytes(changed))
  except ValueError:pass
  else:raise AssertionError('unknown native code accepted')
print('PASS: diagnostics on/off archive routing, only AirPlayUtils member changes, unknown code rejected')
