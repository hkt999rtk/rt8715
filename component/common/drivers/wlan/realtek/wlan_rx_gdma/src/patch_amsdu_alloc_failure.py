#!/usr/bin/env python3
"""Fix the vendor A-MSDU receive-frame allocation failure path.

The closed rtw_recv.o branches to a null dereference and UDF when
_rtw_alloc_recvframe returns NULL. Return zero instead: its caller,
recv_indicatepkts_in_order, already frees the original receive frame when
amsdu_to_msdu returns anything other than one.

This patch is tied to the exact known vendor function. A new vendor object
must be reviewed instead of silently applying old offsets.
"""

import argparse
import hashlib
import struct
from pathlib import Path


SECTION_NAME = b".wlan.text.amsdu_to_msdu"
ORIGINAL_SHA256 = "89d7b559a5ec53ce19c5efa1a7760ed8bd5376181dfcb569f03ab7fff9a451a4"
PATCHED_SHA256 = "1c9dd203e66c4dc8369b73901bdbeabab01586ed8f750b24c24cc740a151846a"
TRAP_OFFSET = 0x260
TRAP_BYTES = bytes.fromhex("0369ffde")  # ldr r3, [r0, #16]; udf #255
RETURN_BYTES = bytes.fromhex("0020eee7")  # movs r0, #0; b.n +0x242


def section_bounds(elf):
    if elf[:6] != b"\x7fELF\x01\x01":
        raise ValueError("expected little-endian ELF32 object")
    shoff = struct.unpack_from("<I", elf, 32)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 46)
    if shentsize != 40 or shstrndx >= shnum:
        raise ValueError("unexpected ELF section table")
    if shoff + shentsize * shnum > len(elf):
        raise ValueError("truncated ELF section table")
    headers = [struct.unpack_from("<10I", elf, shoff + i * shentsize)
               for i in range(shnum)]
    names_header = headers[shstrndx]
    names_start, names_size = names_header[4:6]
    names = elf[names_start:names_start + names_size]
    if len(names) != names_size:
        raise ValueError("truncated ELF section names")
    for header in headers:
        name_offset = header[0]
        if name_offset >= len(names):
            continue
        end = names.find(b"\0", name_offset)
        if end < 0:
            continue
        name = names[name_offset:end]
        if name == SECTION_NAME:
            start, size = header[4:6]
            if size != 0x274 or start + size > len(elf):
                raise ValueError("unexpected A-MSDU function size")
            return start, size
    raise ValueError("A-MSDU function section not found")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("object", type=Path)
    parser.add_argument("--check", action="store_true",
                        help="verify that the patch is present without writing")
    args = parser.parse_args()

    elf = bytearray(args.object.read_bytes())
    start, size = section_bounds(elf)
    section = elf[start:start + size]
    digest = hashlib.sha256(section).hexdigest()
    if args.check:
        if digest != PATCHED_SHA256:
            raise ValueError("A-MSDU null-allocation patch missing or object changed")
        print("A-MSDU null-allocation return verified")
        return
    if digest != ORIGINAL_SHA256:
        raise ValueError("vendor A-MSDU function changed; review before patching")
    if section[TRAP_OFFSET:TRAP_OFFSET + 4] != TRAP_BYTES:
        raise ValueError("unexpected A-MSDU null-allocation instructions")
    if section[0x72:0x76] != bytes.fromhex("00f0f580"):
        raise ValueError("unexpected A-MSDU null-allocation branch")
    if section[0x240:0x248] != bytes.fromhex("002009b0bde8f08f"):
        raise ValueError("unexpected A-MSDU failure return")

    elf[start + TRAP_OFFSET:start + TRAP_OFFSET + 4] = RETURN_BYTES
    if hashlib.sha256(elf[start:start + size]).hexdigest() != PATCHED_SHA256:
        raise ValueError("patched A-MSDU function verification failed")
    args.object.write_bytes(elf)
    print("A-MSDU receive-frame allocation failure now returns zero")


if __name__ == "__main__":
    main()
