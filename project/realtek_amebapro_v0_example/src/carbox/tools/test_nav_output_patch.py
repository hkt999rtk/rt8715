#!/usr/bin/env python3
"""Pacing must alter only one additional relocation, preserving native instructions."""
import subprocess,tempfile
from pathlib import Path
import patch_airplay_music_output as patch
from test_patch_airplay_music_output import elf_object,ROOT,DEFAULT_PREFIX
archive=ROOT/'project/realtek_amebapro_v0_example/GCC-RELEASE/carplay_app/lib_Accessory2_resource450.a'
with tempfile.TemporaryDirectory(prefix='nav-pacing-patch-') as t:
 p=Path(t);normal=p/'normal.a';paced=p/'paced.a'
 patch.main(DEFAULT_PREFIX+'ar',DEFAULT_PREFIX+'objcopy',archive,normal)
 patch.main(DEFAULT_PREFIX+'ar',DEFAULT_PREFIX+'objcopy',archive,paced,pacing=True)
 def member(a):return subprocess.check_output([DEFAULT_PREFIX+'ar','p',str(a),patch.MEMBER])
 a,ar,_=elf_object(member(normal));b,br,_=elf_object(member(paced))
 for n in a:
  if n.startswith('.text.'):assert a[n]==b[n]
 changes=[]
 for n in ar:
  assert len(ar[n])==len(br[n])
  changes.extend((n,x,y) for x,y in zip(ar[n],br[n]) if x!=y)
 assert changes==[('.rel.text.AirPlayAudio_AudioOutputThread',(0x118,10,'RingBuffer_size'),(0x118,10,'carbox_nav_output_pace'))],changes
 # Validate the private LR adjustment belongs to this exact return site.
 assert 0x118+4-0x8c==0x90
 obj=p/'hook.o'
 subprocess.run([DEFAULT_PREFIX+'gcc','-mcpu=cortex-m33','-mthumb','-DCONFIG_AUDIO_MUSIC_DIAG=1','-DCONFIG_AUDIO_NAV_PACING=1','-c',str(ROOT/'project/realtek_amebapro_v0_example/src/carbox/audio_music_output_hooks.S'),'-o',str(obj)],check=True)
 sections,rels,_=elf_object(obj.read_bytes())
 assert '.text.carbox_nav_output_pace' in sections
 targets=[r[2] for r in rels['.rel.text.carbox_nav_output_pace']]
 assert targets==['carbox_nav_output_pace_c','RingBuffer_size'],targets
 dis=subprocess.check_output([DEFAULT_PREFIX+'objdump','-d',str(obj)],text=True)
 assert 'sub.w\tlr, lr, #140' in dis
 print('PASS pacing archive: exact pinned member, one additional relocation, unchanged instructions, guarded native fallback and loop-return ABI')
