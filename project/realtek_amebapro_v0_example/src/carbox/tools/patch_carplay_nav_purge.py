#!/usr/bin/env python3
"""Retarget one audited call; preserve all ELF section bytes/relocations.

Run before navigation diagnostic patch. The companion Thumb trampoline refreshes
Read's saved head after purge; no buffering threshold/purge policy is changed.
"""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

SHA = '67600fdd873794d860e48f0b132bc2293fc831fcdd677b4465bfdfccfef2240c'
# 2026-11-03 customer mono bundle: native sections and semantic relocations
# audited identical; only compiler-generated symbol/section string names differ.
MONO_SHA = 'dfeac1e6c8f494e73e30942c98f29e762b781aa826b501010459dc907f11f1ad'
VENDOR_SHAS = (SHA, MONO_SHA)
NAME = 'carbox_nav_purge_read'
NATIVE = '_RTPJitterBufferDiscardExcess'

def fail(message):
    raise SystemExit('Navigation purge fix: ' + message)

def patch(path):
    b = bytearray(path.read_bytes())
    h = struct.unpack_from('<16sHHIIIIIHHHHHH', b)
    if h[0][:6] != b'\x7fELF\x01\x01' or h[1:3] != (1, 40) or h[11] != 40:
        fail('invalid ELF')
    sections = [struct.unpack_from('<10I', b, h[6] + 40*i) for i in range(h[12])]
    def data(s): return b[s[4]:s[4]+s[5]]
    def string(s, o): return s[o:].split(b'\0', 1)[0].decode('ascii')
    names = data(sections[h[13]])
    secs = {string(names,s[0]):(i,s) for i,s in enumerate(sections)}
    tabs = [(i,s) for i,s in enumerate(sections) if s[1] == 2]
    if len(tabs) != 1: fail('symbol table')
    ti, tab = tabs[0]
    strings = data(sections[tab[6]])
    syms = {}
    for i in range(tab[5]//16):
        off = tab[4]+16*i
        rec = struct.unpack_from('<IIIBBH', b, off)
        name = string(strings, rec[0])
        if name: syms[name] = (i,off,rec)
    si, sec = secs['.text.RTPJitterBufferRead']
    _, rel = secs['.rel.text.RTPJitterBufferRead']
    if rel[1] != 9 or rel[6:8] != (ti,si): fail('relocation layout')
    old, new = syms[NATIVE], syms[NAME]
    if new[2][5] != 0xfff1: fail('placeholder')
    matches = [off for off in range(rel[4], rel[4]+rel[5],8)
               if struct.unpack_from('<I',b,off)[0] == 0x6e]
    if len(matches) != 1: fail('call relocation missing')
    off = matches[0]
    if struct.unpack_from('<I',b,off+4)[0] != (old[0]<<8)|10:
        fail('unexpected original callee')
    # Validate both native head capture and post-call continuation. No section
    # content is replaced; do NOT use objcopy --update-section here.
    if data(sec)[0x60:0x7e].hex() != 'd4f8385115b9d4f8405145b32046fff7feff0021b54bd4f8dc61ed682046':
        fail('Read layout changed')
    struct.pack_into('<I',b,off+4,(new[0]<<8)|10)
    struct.pack_into('<H',b,new[1]+14,0)
    path.write_bytes(b)

def main(ar,objcopy,source,out):
    vendor = subprocess.check_output([ar,'p',source,'AirPlayUtils.o'])
    if hashlib.sha256(vendor).hexdigest() not in VENDOR_SHAS:
        fail('AirPlayUtils.o changed; re-audit before patching')
    out = Path(out); out.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out.parent,prefix='.nav-purge-') as td:
        td=Path(td); a=td/'archive.a'; a.write_bytes(Path(source).read_bytes())
        p=td/'AirPlayUtils.o'; p.write_bytes(vendor)
        subprocess.run([objcopy,'--globalize-symbol='+NATIVE,
                        '--add-symbol='+NAME+'=0,global',str(p)],check=True)
        patch(p)
        digest=hashlib.sha256(p.read_bytes()).hexdigest()
        subprocess.run([ar,'r',str(a),str(p)],check=True,stdout=subprocess.DEVNULL)
        subprocess.run([ar,'s',str(a)],check=True)
        os.replace(a,out)
    print('Navigation purge fix: refresh Read head after purge; AirPlayUtils.o sha256='+digest)

if __name__ == '__main__':
    if len(sys.argv) != 5: fail('usage: AR OBJCOPY INPUT OUTPUT')
    main(*sys.argv[1:])
