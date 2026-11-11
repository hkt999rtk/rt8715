#!/usr/bin/env python3
"""本帧先采时再填 header；保留原厂 global 更新、所有其它指令和重定位。"""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from patch_screen_session import ELF

FN = 'AirPlayScreen_SendScreenNormalFrame'
HASH = '87fd07c15cde187f7c4b5ad0632d419fc1640ae41bdc23533223b6a94066de96'
MEMBER = 'AirPlayScreen.o'

def main(ar, source, output):
    source, output = Path(source), Path(output)
    if source.resolve() == output.resolve():
        raise SystemExit('current timestamp: input and output must differ')
    members = subprocess.check_output([ar,'t',str(source)]).decode().splitlines()
    if members.count(MEMBER) != 1:
        raise SystemExit('current timestamp: missing/duplicate screen object')
    e = ELF(subprocess.check_output([ar,'p',str(source),MEMBER]))
    code = e.code(FN)
    if hashlib.sha256(code).hexdigest() != HASH:
        raise SystemExit('current timestamp: unknown/already patched function; review required')
    section = e.named['.text.'+FN]
    rel = e.named['.rel.text.'+FN]
    hits = []
    for off in range(rel[4],rel[4]+rel[5],8):
        pc,info = struct.unpack_from('<II',e.b,off)
        if 0x40 <= pc < 0x54:
            hits.append((pc,info&255,e.indices[info>>8][0],off))
    if [(pc,k,n) for pc,k,n,_ in hits] != [(0x48,10,'UpTicks'),(0x4c,10,'UpTicksToNTP')]:
        raise SystemExit('current timestamp: unexpected sampling relocations')
    # 原指令：读旧 global -> 填 header -> UpTicks -> 转换 -> 写新 global。
    # 新指令：NOP -> UpTicks -> 转换 -> 写新 global -> 填本帧 header。
    # BL 移动 4 字节，必须同步移动 R_ARM_THM_CALL，不能用 objcopy
    # --update-section，否则可能丢失该函数其它调用的重定位。
    if code[0x40:0x48] != bytes.fromhex('d4e90023 cde90423') or code[0x50:0x54] != bytes.fromhex('c4e90001'):
        raise SystemExit('current timestamp: unexpected native stores')
    replacement = bytes.fromhex('00bf00bf') + code[0x48:0x54] + bytes.fromhex('cde90401')
    e.b[section[4]+0x40:section[4]+0x54] = replacement
    for pc,_,_,off in hits:
        struct.pack_into('<I',e.b,off,pc-4)
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent,prefix='.screen-time-') as td:
        tmp=Path(td); obj=tmp/MEMBER; obj.write_bytes(e.b)
        archive=tmp/'archive.a'; archive.write_bytes(source.read_bytes())
        subprocess.run([ar,'r',str(archive),str(obj)],check=True)
        subprocess.run([ar,'s',str(archive)],check=True)
        os.replace(archive,output)
    print('screen timestamp: current frame sampled before header; native global retained')

if __name__=='__main__': main(*sys.argv[1:])
