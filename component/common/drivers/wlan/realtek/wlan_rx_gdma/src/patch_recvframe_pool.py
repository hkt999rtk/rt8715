#!/usr/bin/env python3
"""Double the known vendor receive-frame pool from 16 to 32 entries.

The pool contains 192-byte frames with up to 255 bytes of alignment padding.
Patch the count, allocation, iteration limit, and matching free size together.
Reject a changed vendor object instead of guessing new instruction offsets.
"""

import argparse
import hashlib
import struct
from pathlib import Path


SECTIONS = {
    b".wlan.text._rtw_init_recv_priv": {
        "size": 0xB4,
        "sha256": "8505f18f6b426b986e057e0a78409295278fcbdae36a041be225ce77660e6c5e",
        "patches": {
            0x2C: ("1023", "2023"),  # count: 16 -> 32
            0x3A: ("4ff45060", "4ff4c850"),  # allocation: 0xd00 -> 0x1900
            0x54: ("04f54068", "04f5c058"),  # end: 0xc00 -> 0x1800
        },
    },
    b".wlan.text._rtw_free_recv_priv": {
        "size": 0x34,
        "sha256": "38366b07f4ae31cd778597ddca15a4390a2e13e57398412b7ae33576fdb9662f",
        "patches": {
            0x22: ("4ff45061", "4ff4c851"),  # free size: 0xd00 -> 0x1900
        },
    },
    b".wlan.text.rtw_free_recvframe": {
        "size": 0xA8,
        "sha256": "60bc716641e5811ca89be164da26695e7867f5b3a0fbb85cc2ca33eb4b7d6524",
        "patches": {
            0x56: ("102b", "202b"),  # free-count diagnostic limit: 16 -> 32
            0x6C: ("1023", "2023"),  # diagnostic's expected count: 16 -> 32
        },
    },
}


def section_bounds(elf):
    if elf[:6] != b"\x7fELF\x01\x01":
        raise ValueError("expected little-endian ELF32 object")
    shoff = struct.unpack_from("<I", elf, 32)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 46)
    if shentsize != 40 or shstrndx >= shnum or shoff + shentsize * shnum > len(elf):
        raise ValueError("unexpected ELF section table")
    headers = [struct.unpack_from("<10I", elf, shoff + i * shentsize)
               for i in range(shnum)]
    names_header = headers[shstrndx]
    names = elf[names_header[4]:names_header[4] + names_header[5]]
    if len(names) != names_header[5]:
        raise ValueError("truncated ELF section names")
    found = {}
    for header in headers:
        name_offset = header[0]
        if name_offset >= len(names):
            continue
        end = names.find(b"\0", name_offset)
        if end < 0:
            continue
        name = bytes(names[name_offset:end])
        if name in SECTIONS:
            start, size = header[4:6]
            if size != SECTIONS[name]["size"] or start + size > len(elf):
                raise ValueError(f"unexpected section size: {name!r}")
            found[name] = (start, size)
    if set(found) != set(SECTIONS):
        raise ValueError("receive-frame init/free sections missing")
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("object", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    elf = bytearray(args.object.read_bytes())
    for name, (start, size) in section_bounds(elf).items():
        spec = SECTIONS[name]
        section = bytearray(elf[start:start + size])
        for offset, (old_hex, new_hex) in spec["patches"].items():
            old, new = bytes.fromhex(old_hex), bytes.fromhex(new_hex)
            expected = new if args.check else old
            if section[offset:offset + len(expected)] != expected:
                raise ValueError(f"unexpected instructions in {name!r} at {offset:#x}")
            if not args.check:
                section[offset:offset + len(new)] = new
        if not args.check and hashlib.sha256(elf[start:start + size]).hexdigest() != spec["sha256"]:
            raise ValueError(f"vendor {name!r} changed; review before patching")
        elf[start:start + size] = section
    if not args.check:
        args.object.write_bytes(elf)
    print("receive-frame pool verified: 32 entries, 6400-byte allocation/free and diagnostics")


if __name__ == "__main__":
    main()
