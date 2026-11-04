#!/usr/bin/env python3
"""Check opt-in instrumentation changes only audited relocations, never code/data."""
import hashlib, pathlib, subprocess, tempfile, struct
HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[4]
PREFIX = '/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
SOURCE = ROOT/'project/realtek_amebapro_v0_example/GCC-RELEASE/carplay_app/lib_CarPlay.a'
PATCH = HERE/'patch_carplay_music_progress.py'
assert PATCH.exists(), 'navigation observation-only archive patch not implemented'
import importlib.util
spec=importlib.util.spec_from_file_location('navpatch',PATCH); m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
def sections(b):
 h=struct.unpack_from('<16sHHIIIIIHHHHHH',b); ss=[struct.unpack_from('<10I',b,h[6]+40*i) for i in range(h[12])]; ns=ss[h[13]]; names=b[ns[4]:ns[4]+ns[5]]
 return {names[s[0]:].split(b'\0',1)[0].decode(): b[s[4]:s[4]+s[5]] for s in ss}
with tempfile.TemporaryDirectory() as d:
 out=pathlib.Path(d)/'out.a'
 subprocess.run(['python3',str(PATCH),PREFIX+'ar',PREFIX+'objcopy',str(SOURCE),str(out)],check=True)
 before=subprocess.check_output([PREFIX+'ar','p',str(SOURCE),'AirPlayReceiverSession.o']); after=subprocess.check_output([PREFIX+'ar','p',str(out),'AirPlayReceiverSession.o'])
 a,b=sections(before),sections(after)
 for name,data in a.items():
  if name.startswith(('.text','.rodata','.data','.bss')): assert b[name]==data,name
 rel=subprocess.check_output([PREFIX+'objdump','-r',str(out)],text=True)
 for hook in m.HOOKS: assert hook['name'] in rel,hook
 # Wrong vendor must fail without replacing an existing valid output.
 bad=pathlib.Path(d)/'bad.a';bad.write_bytes(SOURCE.read_bytes()); obj=pathlib.Path(d)/'AirPlayReceiverSession.o';
 mutated=bytearray(before); signature=sections(before)['.text._GeneralAudioThread']; at=before.index(signature); mutated[at]^=1; obj.write_bytes(mutated)
 subprocess.run([PREFIX+'ar','r',str(bad),str(obj)],check=True,stdout=subprocess.DEVNULL)
 expected=hashlib.sha256(out.read_bytes()).digest()
 ret=subprocess.run(['python3',str(PATCH),PREFIX+'ar',PREFIX+'objcopy',str(bad),str(out)],capture_output=True)
 assert ret.returncode and b'changed' in ret.stderr
 assert hashlib.sha256(out.read_bytes()).digest()==expected
print('PASS: original instructions/data unchanged; all probes linked by relocation; wrong vendor rejected atomically')
