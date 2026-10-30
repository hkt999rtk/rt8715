#!/usr/bin/env python3
"""Redirect exactly one vendor A-MSDU child-init relocation to the ABI bridge.

Input is the known rtw_recv.o after the existing NULL-allocation and 32-frame
pool patches. No instruction, existing symbol index, or other relocation is
changed. New symbol/string tables are appended; their old bytes remain unused.
Full input/output identities make unknown revisions and partial patches fail.
"""
import argparse
import hashlib
from pathlib import Path
import struct

INPUT_SHA256 = 'a6dc25893e3fb17e8e0a8a4dd0fc1fe08e1a258c267cf73374c294689e8f6e4a'
OUTPUT_SHA256 = '452b1a0782e9ac7f0bb6cb7d57db72f375e2af15f711b640174e1e269cea0f8d'
HELPER = b'wlan_amsdu_child_init'
FUNCTION = '.wlan.text.amsdu_to_msdu'
CALL_OFFSET = 0x76


def elf_sections(blob):
    if len(blob) < 52 or blob[:6] != b'\x7fELF\x01\x01':
        raise ValueError('expected little-endian ELF32')
    if struct.unpack_from('<HH', blob, 16) != (1, 40):
        raise ValueError('expected relocatable ARM object')
    shoff = struct.unpack_from('<I', blob, 32)[0]
    size, count, names_index = struct.unpack_from('<HHH', blob, 46)
    if size != 40 or names_index >= count or shoff+size*count > len(blob):
        raise ValueError('invalid section table')
    headers = [struct.unpack_from('<10I', blob, shoff+i*40) for i in range(count)]
    for h in headers:
        if h[1] != 8 and h[4]+h[5] > len(blob):
            raise ValueError('truncated section')
    h = headers[names_index]
    strings = blob[h[4]:h[4]+h[5]]
    names = {}
    for i, h in enumerate(headers):
        end = strings.find(b'\0', h[0])
        if h[0] >= len(strings) or end < 0:
            raise ValueError('bad section name')
        names[strings[h[0]:end].decode('ascii')] = i
    return shoff, headers, names


def check(blob):
    if hashlib.sha256(blob).hexdigest() != OUTPUT_SHA256:
        raise ValueError('A-MSDU child-adapter patch missing or object changed')


def patch(blob):
    digest = hashlib.sha256(blob).hexdigest()
    if digest == OUTPUT_SHA256:
        return blob  # deterministic and idempotent
    if digest != INPUT_SHA256:
        raise ValueError('unknown rtw_recv.o; review vendor ABI and preceding patches')
    shoff, headers, names = elf_sections(blob)
    fn = headers[names[FUNCTION]]
    code = blob[fn[4]:fn[4]+fn[5]]
    if (fn[5] != 0x274 or code[0xc:0xe] != bytes.fromhex('8046') or
            code[0x6e:0x76] != bytes.fromhex('0446002800f0f580')):
        raise ValueError('processing-adapter register or child NULL guard changed')
    sym_index = next(i for i, h in enumerate(headers) if h[1] == 2)
    sym = headers[sym_index]
    str_index = sym[6]
    strings_header = headers[str_index]
    symbols = blob[sym[4]:sym[4]+sym[5]]
    strings = blob[strings_header[4]:strings_header[4]+strings_header[5]]
    rel = headers[names['.rel'+FUNCTION]]
    if sym[9] != 16 or rel[9] != 8 or rel[6] != sym_index or rel[7] != names[FUNCTION]:
        raise ValueError('unexpected symbol/relocation format')
    matches = []
    for pos in range(rel[4], rel[4]+rel[5], 8):
        offset, info = struct.unpack_from('<II', blob, pos)
        if offset == CALL_OFFSET:
            symbol = struct.unpack_from('<IIIBBH', symbols, (info >> 8)*16)
            name = strings[symbol[0]:].split(b'\0', 1)[0]
            if info & 255 != 10 or name != b'rtw_init_listhead' or symbol[5] != 0:
                raise ValueError('child-init call no longer targets undefined rtw_init_listhead')
            matches.append(pos)
    if len(matches) != 1:
        raise ValueError('missing/duplicate child-init relocation')

    out = bytearray(blob)
    new_string_offset = len(out)
    out += strings+HELPER+b'\0'
    new_string_size = len(out)-new_string_offset
    out += bytes((-len(out)) % 4)
    new_symbol_offset = len(out)
    out += symbols+struct.pack('<IIIBBH', len(strings), 0, 0, 0x12, 0, 0)
    struct.pack_into('<II', out, shoff+str_index*40+16, new_string_offset, new_string_size)
    struct.pack_into('<II', out, shoff+sym_index*40+16, new_symbol_offset, len(symbols)+16)
    struct.pack_into('<I', out, matches[0]+4, ((len(symbols)//16) << 8) | 10)
    check(out)
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('object', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    original = args.object.read_bytes()
    if args.check:
        check(original)
    else:
        output = patch(original)
        if output != original:
            args.object.write_bytes(output)
    print('A-MSDU child adapter: pinned init-call relocation verified')


if __name__ == '__main__':
    main()
