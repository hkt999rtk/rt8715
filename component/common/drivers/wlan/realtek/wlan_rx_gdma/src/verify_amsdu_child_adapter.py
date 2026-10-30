#!/usr/bin/env python3
"""Verify the functional A-MSDU fix independently of diagnostics wrappers."""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess

from patch_amsdu_child_adapter import check, elf_sections

HELPER = 'wlan_amsdu_child_init'
STORE = bytes.fromhex('c0f81080')  # str.w r8, [r0,#16]
INDICATE_SHA256 = 'da79b7d94933eefcbc971a54ffca925898e8098594d7ed0d4791d955aec9de8e'


def run(*args):
    return subprocess.check_output([str(x) for x in args])


def verify_archive(path, prefix):
    names = run(prefix+'ar', 't', path).decode().splitlines()
    for name in ('rtw_recv.o', 'freertos_recv.o', HELPER+'.o'):
        if names.count(name) != 1:
            raise ValueError('missing/duplicate archive member: '+name)
    check(run(prefix+'ar', 'p', path, 'rtw_recv.o'))
    if hashlib.sha256(run(prefix+'ar', 'p', path, 'freertos_recv.o')).hexdigest() != INDICATE_SHA256:
        raise ValueError('native indication/free contract changed')
    blob = run(prefix+'ar', 'p', path, HELPER+'.o')
    _, headers, sections = elf_sections(blob)
    fn = headers[sections['.wlan.text.'+HELPER]]
    code = blob[fn[4]:fn[4]+fn[5]]
    if code != STORE+bytes.fromhex('fff7febf'):
        raise ValueError('unexpected helper body/register/offset')
    reloc = headers[sections['.rel.wlan.text.'+HELPER]]
    if reloc[5] != 8:
        raise ValueError('helper must have one tail-call relocation')
    offset, info = struct.unpack_from('<II', blob, reloc[4])
    symtab = headers[reloc[6]]
    symbol = struct.unpack_from('<IIIBBH', blob, symtab[4]+(info >> 8)*16)
    strings = headers[symtab[6]]
    name = blob[strings[4]+symbol[0]:strings[4]+strings[5]].split(b'\0', 1)[0]
    if offset != 4 or info & 255 != 30 or name != b'rtw_init_listhead' or symbol[5] != 0:
        raise ValueError('helper must tail-call native rtw_init_listhead')
    print('A-MSDU adapter fix: archive identity, isolated relocation and 8-byte bridge verified')


def verify_elf(path, prefix):
    blob = path.read_bytes()
    if len(blob) < 52 or blob[:6] != b'\x7fELF\x01\x01':
        raise ValueError('expected little-endian ELF32')
    phoff = struct.unpack_from('<I', blob, 28)[0]
    phsize, phnum = struct.unpack_from('<HH', blob, 42)
    if phsize != 32 or phoff+phsize*phnum > len(blob):
        raise ValueError('invalid program headers')
    segments = [struct.unpack_from('<8I', blob, phoff+i*phsize) for i in range(phnum)]

    def read(addr, length):
        for typ, off, va, _, filesz, _, _, _ in segments:
            if typ == 1 and va <= addr and addr+length <= va+filesz:
                return blob[off+addr-va:off+addr-va+length]
        raise ValueError(f'unmapped code {addr:#x}')

    symbols = {}
    for line in run(prefix+'nm', '-n', path).decode().splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16) & ~1
    veneers = {addr for name, addr in symbols.items() if name.endswith('_veneer')}

    def resolve(addr):
        for _ in range(3):
            if addr not in veneers:
                return addr
            code = read(addr, 16)
            if code[:12] != bytes.fromhex('01b40248844601bc604700bf'):
                raise ValueError('unreviewed linker veneer')
            target = struct.unpack_from('<I', code, 12)[0]
            if not target & 1:
                raise ValueError('non-Thumb veneer target')
            addr = target & ~1
        raise ValueError('veneer loop')

    def branch(addr, mnemonic, target):
        text = run(prefix+'objdump', '-d', '--start-address='+hex(addr),
                   '--stop-address='+hex(addr+4), path).decode()
        match = re.search(r'^\s*'+format(addr, 'x')+r':\s+(?:[0-9a-f]{4}\s+)+('+
                          mnemonic+r')\s+([0-9a-f]+)\s+<', text, re.M)
        if not match or resolve(int(match[2], 16)) != symbols[target]:
            raise ValueError(f'wrong branch at {addr:#x}, expected {target}')

    amsdu = symbols['amsdu_to_msdu']
    helper = symbols[HELPER]
    if read(amsdu+0xc, 2) != bytes.fromhex('8046'):
        raise ValueError('processing adapter no longer in r8')
    if read(amsdu+0x6e, 8) != bytes.fromhex('0446002800f0f580'):
        raise ValueError('child NULL guard changed')
    if read(amsdu+0x260, 4) != bytes.fromhex('0020eee7'):
        raise ValueError('existing NULL-allocation fix lost')
    if read(helper, 4) != STORE:
        raise ValueError('wrong helper store/register/offset')
    branch(amsdu+0x76, 'bl', HELPER)
    branch(helper+4, r'b\.w', 'rtw_init_listhead')
    print('A-MSDU adapter fix: final ELF call, register ABI, store, native tail-call and NULL path verified')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prefix', required=True)
    p.add_argument('--archive', type=Path, required=True)
    p.add_argument('--elf', type=Path)
    args = p.parse_args()
    verify_archive(args.archive, args.prefix)
    if args.elf:
        verify_elf(args.elf, args.prefix)


if __name__ == '__main__':
    main()
