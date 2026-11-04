#!/usr/bin/env python3
"""Check the linked output hooks, native forwarding and exact Thumb shim ABI."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
from patch_airplay_music_output import CALLS


def verify(path, prefix):
    image = path.read_bytes()
    if image[:6] != b'\x7fELF\x01\x01':
        raise ValueError('expected little-endian ELF32')
    phoff = struct.unpack_from('<I', image, 28)[0]
    phsize, phnum = struct.unpack_from('<HH', image, 42)
    if phsize != 32:
        raise ValueError('unexpected program header size')
    segments = [struct.unpack_from('<8I',image,phoff+i*phsize) for i in range(phnum)]

    def read(addr, size):
        for typ, off, vaddr, _, filesz, _, _, _ in segments:
            if typ == 1 and vaddr <= addr and addr+size <= vaddr+filesz:
                return image[off+addr-vaddr:off+addr-vaddr+size]
        raise ValueError(f'unmapped ELF address {addr:#x}')

    def run(*args):
        return subprocess.check_output(args, text=True)

    symbols, sizes = {}, {}
    for line in run(prefix+'nm','-n','-S',str(path)).splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[2] in ('t','T','w','W'):
            symbols[fields[3]] = int(fields[0],16) & ~1
            sizes[fields[3]] = int(fields[1],16)
    veneers = {addr for name, addr in symbols.items() if name.endswith('_veneer')}

    def resolve(addr):
        for _ in range(3):
            if addr not in veneers:
                return addr
            code = read(addr,16)
            if code[:12] != bytes.fromhex('01b40248844601bc604700bf'):
                raise ValueError(f'unreviewed veneer {addr:#x}')
            target = struct.unpack_from('<I',code,12)[0]
            if not target & 1:
                raise ValueError('non-Thumb veneer target')
            addr = target & ~1
        raise ValueError('veneer chain too deep')

    def targets(addr, size):
        text = run(prefix+'objdump','-d','--start-address='+hex(addr),
                   '--stop-address='+hex(addr+size),str(path))
        return [(int(m[1],16), resolve(int(m[2],16))) for m in re.finditer(
            r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{4}\s+)+(?:(?:bl|blx|b\.w)\s+)([0-9a-f]+)\s+<',text,re.M)]

    for (function,offset),(native,shim,_) in CALLS.items():
        pc = symbols[function]+offset
        if (pc,symbols[shim]) not in targets(pc,4):
            raise ValueError(f'{function}+{offset:#x} misses {shim}')
        helper = shim+'_c'
        called = [dst for _,dst in targets(symbols[helper],sizes[helper])]
        if symbols[native] not in called or symbols[shim] in called:
            raise ValueError('incorrect native forwarding in '+helper)
        for forbidden in ('carbox_rxhd_log','printf','malloc','pvPortMalloc'):
            if forbidden in symbols and symbols[forbidden] in called:
                raise ValueError('hot path logging/allocation in '+helper)
    for kind, argument in (('init','2246'),('push','2346'),('pop','2346'),('exit','2146')):
        shim = 'carbox_music_output_'+kind
        addr = symbols[shim]
        if sizes[shim] != 6 or read(addr,2) != bytes.fromhex(argument):
            raise ValueError('R4 stream argument adapter changed: '+shim)
        if (addr+2,symbols[shim+'_c']) not in targets(addr+2,4):
            raise ValueError('missing helper tail branch: '+shim)
    shim = 'carbox_music_output_sendto'
    addr = symbols[shim]
    if sizes[shim] != 30 or read(addr,22) != bytes.fromhex(
            '10b584b0ddf818c0cdf800c0ddf81cc0cdf804c00294'):
        raise ValueError('sendto adapter no longer preserves all six arguments/SP alignment')
    if read(addr+26,4) != bytes.fromhex('04b010bd'):
        raise ValueError('sendto adapter does not restore caller SP/R4/LR')
    if (addr+22,symbols[shim+'_c']) not in targets(addr+22,4):
        raise ValueError('sendto adapter misses helper call')
    print('music output: 5 linked hook sites, R4/stack ABI and native forwarding verified')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix',required=True)
    parser.add_argument('--elf',type=Path,required=True)
    args = parser.parse_args()
    verify(args.elf,args.prefix)
