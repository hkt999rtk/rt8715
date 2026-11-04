#!/usr/bin/env python3
"""Check opt-in instrumentation changes only audited relocations, never code/data."""
import hashlib, pathlib, subprocess, tempfile, struct, sys
HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[4]
PREFIX = '/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
SOURCE = pathlib.Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'project/realtek_amebapro_v0_example/GCC-RELEASE/carplay_app/lib_CarPlay.a'
PATCH = HERE/'patch_carplay_nav_diag.py'
assert PATCH.exists(), 'navigation observation-only archive patch not implemented'
import importlib.util
spec=importlib.util.spec_from_file_location('navpatch',PATCH); m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
def sections(b):
 h=struct.unpack_from('<16sHHIIIIIHHHHHH',b); ss=[struct.unpack_from('<10I',b,h[6]+40*i) for i in range(h[12])]; ns=ss[h[13]]; names=b[ns[4]:ns[4]+ns[5]]
 return {names[s[0]:].split(b'\0',1)[0].decode(): b[s[4]:s[4]+s[5]] for s in ss}
with tempfile.TemporaryDirectory() as d:
 out=pathlib.Path(d)/'out.a'
 subprocess.run(['python3',str(PATCH),PREFIX+'ar',PREFIX+'objcopy',str(SOURCE),str(out)],check=True)
 before=subprocess.check_output([PREFIX+'ar','p',str(SOURCE),'AirPlayUtils.o']); after=subprocess.check_output([PREFIX+'ar','p',str(out),'AirPlayUtils.o'])
 a,b=sections(before),sections(after)
 for name,data in a.items():
  if name.startswith(('.text','.rodata','.data','.bss')): assert b[name]==data,name
 rel=subprocess.check_output([PREFIX+'objdump','-r',str(out)],text=True)
 for hook in m.HOOKS: assert hook['name'] in rel,hook
 fixed=pathlib.Path(d)/'fixed.a'; diagfixed=pathlib.Path(d)/'fixed_diag.a'
 subprocess.run(['python3',str(HERE/'patch_carplay_nav_purge.py'),PREFIX+'ar',PREFIX+'objcopy',str(SOURCE),str(fixed)],check=True)
 subprocess.run(['python3',str(PATCH),PREFIX+'ar',PREFIX+'objcopy',str(fixed),str(diagfixed)],check=True)
 fixedrel=subprocess.check_output([PREFIX+'objdump','-r',str(diagfixed)],text=True)
 assert 'carbox_nav_purge_read' in fixedrel
 for hook in m.HOOKS:
  if hook['id']!=21: assert hook['name'] in fixedrel,hook
 # Wrong vendor must fail without replacing an existing valid output.
 bad=pathlib.Path(d)/'bad.a';bad.write_bytes(SOURCE.read_bytes()); obj=pathlib.Path(d)/'AirPlayUtils.o';obj.write_bytes(before+b'bad')
 subprocess.run([PREFIX+'ar','r',str(bad),str(obj)],check=True,stdout=subprocess.DEVNULL)
 expected=hashlib.sha256(out.read_bytes()).digest()
 ret=subprocess.run(['python3',str(PATCH),PREFIX+'ar',PREFIX+'objcopy',str(bad),str(out)],capture_output=True)
 assert ret.returncode and b'changed' in ret.stderr
 assert hashlib.sha256(out.read_bytes()).digest()==expected
print('PASS: original instructions/data unchanged; all probes linked by relocation; wrong vendor rejected atomically')
