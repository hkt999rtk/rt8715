#!/usr/bin/env python3
"""Native v19 audio: redirect seven observed calls without changing instructions."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from patch_screen_session import ELF

SHA256 = '89e071d9f76e7966ae2d1d81d101ac1457db4f8c56f3d28bbfbbd396f50809d3'
MEMBER = 'AirPlayAudio.o'
OUTPUT = 'AirPlayAudio_AudioOutputThread'
HOOKS = [
    (OUTPUT,0xc2,'RingBuffer_init','carbox_audio_tx_ring_init'),
    (OUTPUT,0x128,'pthread_exit','carbox_audio_tx_thread_exit'),
    (OUTPUT,0x174,'RingBuffer_pop','carbox_audio_tx_ring_pop'),
    (OUTPUT,0x23e,'AirPlayAudio_SetAudioStreamPacket','carbox_audio_tx_packet'),
    (OUTPUT,0x696,'AirPlayAudio_SetAudioStreamPacket','carbox_audio_tx_packet'),
    (OUTPUT,0x3fc,'lwip_sendto','carbox_audio_tx_sendto'),
    ('AirPlayAudio_SendAudio',0xcc,'RingBuffer_push','carbox_audio_tx_ring_push'),
]

def relocations(elf):
    result={}
    for name,s in elf.named.items():
        if s[1]!=9: continue
        result[name]=[]
        for pos in range(s[4],s[4]+s[5],8):
            offset,info=struct.unpack_from('<II',elf.b,pos)
            result[name].append((offset,info&255,elf.indices[info>>8][0]))
    return result

def main(ar,objcopy,source,output):
    source,output=Path(source),Path(output)
    if source.resolve()==output.resolve(): raise SystemExit('audio evidence: input/output must differ')
    members=subprocess.check_output([ar,'t',str(source)]).decode().splitlines()
    if members.count(MEMBER)!=1: raise SystemExit('audio evidence: missing/duplicate audio member')
    original=subprocess.check_output([ar,'p',str(source),MEMBER])
    if hashlib.sha256(original).hexdigest()!=SHA256:
        raise SystemExit('audio evidence: unsupported/already patched AirPlayAudio.o')
    before=ELF(original)
    for name in ['mainAudioStream','altAudioStream']:
        symbol=before.symbols[name][2]
        if symbol[2]!=720 or symbol[3]!=17 or not symbol[5]:
            raise SystemExit('audio evidence: incompatible stream ABI')
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent,prefix='.audio-evidence-') as tmp:
        tmp=Path(tmp); obj=tmp/MEMBER; obj.write_bytes(original)
        names=sorted({h[3] for h in HOOKS})
        subprocess.run([objcopy,*['--add-symbol='+n+'=0,global' for n in names],str(obj)],check=True)
        after=ELF(obj.read_bytes())
        for n in names: after.undefined(n)
        for fn,pc,old,new in HOOKS: after.redirect(old,new,fn,pc,10)
        # 所有重定位逐条对比，禁止 objcopy 静默删除重定位。
        lookup={('.rel.text.'+fn,pc,old):new for fn,pc,old,new in HOOKS}
        expected={n:[(pc,kind,lookup.get((n,pc,sym),sym)) for pc,kind,sym in rows] for n,rows in relocations(before).items()}
        if relocations(after)!=expected: raise SystemExit('audio evidence: unexpected relocation change')
        for name,s in before.named.items():
            if name.startswith(('.text','.rodata','.data')):
                t=after.named[name]
                if before.b[s[4]:s[4]+s[5]]!=after.b[t[4]:t[4]+t[5]]:
                    raise SystemExit('audio evidence: instructions/data changed')
        obj.write_bytes(after.b)
        archive=tmp/'archive.a'; archive.write_bytes(source.read_bytes())
        subprocess.run([ar,'r',str(archive),str(obj)],check=True)
        subprocess.run([ar,'s',str(archive)],check=True)
        os.replace(archive,output)
    print('audio evidence: seven isolated native calls; PCM/RTP/pacing instructions preserved')

if __name__=='__main__': main(*sys.argv[1:])
