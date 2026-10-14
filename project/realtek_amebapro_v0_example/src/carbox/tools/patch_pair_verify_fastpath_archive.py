#!/usr/bin/env python3
"""Guarded Pair Verify first path for already paired CarPlay peers.

Only AirPlayClient_SendMessagesNormal is changed. A rejected Verify retries the
original Setup path, and changed customer objects stop the build for re-audit.
"""

import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


MEMBER = "AirPlayClient.o"
VENDOR_SHA256 = "07458e274e8717a6cbccdd473995c50d43e5ba6c16d5badaf864b6887f558352"
FUNCTION = ".text.AirPlayClient_SendMessagesNormal"
ROUTES = (
    (0x04, "GetAirPlayInfo", "carbox_pair_fastpath_begin"),
    (0x68, "AirPlayInfo_IsPaired", "carbox_pair_fastpath_is_paired"),
    (0x6E, "AirPlayInfo_GetFailNum", "carbox_pair_fastpath_fail_num"),
)
CODE = (
    # On an existing pair, skip Setup and initialize the original Verify path.
    (0x6C, bytes.fromhex("48b9"), bytes.fromhex("a0bb")),
    # Both legacy and fast paths enter +0xd8. Restore r7's HTTP message
    # pointer and clear the old Setup result flag before Verify begins.
    (0xDA, bytes.fromhex("2b68"), bytes.fromhex("224f")),
    (0xDC, bytes.fromhex("03b1"), bytes.fromhex("0024")),
)


def fail(message):
    raise SystemExit("pair verify fastpath patch: " + message)


def patch_member(path):
    data = bytearray(path.read_bytes())
    elf = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    if elf[0][:6] != b"\x7fELF\x01\x01" or elf[1:3] != (1, 40) or elf[11] != 40:
        fail("expected ARM ELF32 relocatable")
    sections = [struct.unpack_from("<10I", data, elf[6] + 40 * n)
                for n in range(elf[12])]

    def secdata(section):
        return data[section[4]:section[4] + section[5]]

    def cstr(table, offset):
        end = table.find(0, offset)
        if end < 0:
            fail("unterminated string")
        return table[offset:end].decode("ascii")

    names = secdata(sections[elf[13]])
    by_name = {cstr(names, section[0]): (n, section)
               for n, section in enumerate(sections)}
    if FUNCTION not in by_name or ".rel" + FUNCTION not in by_name:
        fail("missing target function")
    text_index, text_section = by_name[FUNCTION]
    _, rel_section = by_name[".rel" + FUNCTION]
    tables = [(n, section) for n, section in enumerate(sections)
              if section[1] == 2]
    if len(tables) != 1:
        fail("symbol table count")
    table_index, table = tables[0]
    if rel_section[1] != 9 or rel_section[6:8] != (table_index, text_index):
        fail("bad relocation section")
    strings = secdata(sections[table[6]])
    symbols = {}
    for n in range(table[5] // 16):
        offset = table[4] + n * 16
        record = struct.unpack_from("<IIIBBH", data, offset)
        name = cstr(strings, record[0])
        if name:
            symbols[name] = (n, offset, record)

    for location, old_name, new_name in ROUTES:
        if old_name not in symbols or new_name not in symbols:
            fail("missing route symbols")
        old, new = symbols[old_name], symbols[new_name]
        if old[2][5] != 0 or new[2][5] != 0xFFF1:
            fail("unexpected symbol binding for " + new_name)
        matches = []
        for offset in range(rel_section[4], rel_section[4] + rel_section[5], 8):
            call_offset, info = struct.unpack_from("<II", data, offset)
            if call_offset == location:
                matches.append((offset, info))
        if len(matches) != 1 or matches[0][1] != ((old[0] << 8) | 10):
            fail("unexpected relocation at +0x%x" % location)
        struct.pack_into("<H", data, new[1] + 14, 0)
        struct.pack_into("<I", data, matches[0][0] + 4, (new[0] << 8) | 10)

    code_offset, code_size = text_section[4:6]
    if code_size < 0x178:
        fail("unexpected function size")
    for location, before, after in CODE:
        start = code_offset + location
        if data[start:start + len(before)] != before:
            fail("unexpected instruction at +0x%x" % location)
        data[start:start + len(after)] = after
    path.write_bytes(data)


def main(ar, objcopy, source, output):
    source, output = Path(source), Path(output)
    vendor = subprocess.check_output([ar, "p", str(source), MEMBER])
    if hashlib.sha256(vendor).hexdigest() != VENDOR_SHA256:
        fail(MEMBER + " changed; re-audit the state machine")
    with tempfile.TemporaryDirectory(prefix="pair-verify-", dir=output.parent) as tmp:
        work = Path(tmp)
        archive = work / "archive.a"
        archive.write_bytes(source.read_bytes())
        member = work / MEMBER
        member.write_bytes(vendor)
        subprocess.run([
            objcopy,
            "--add-symbol=carbox_pair_fastpath_begin=0,global",
            "--add-symbol=carbox_pair_fastpath_is_paired=0,global",
            "--add-symbol=carbox_pair_fastpath_fail_num=0,global",
            str(member),
        ], check=True)
        patch_member(member)
        subprocess.run([ar, "r", str(archive), str(member)], check=True)
        subprocess.run([ar, "s", str(archive)], check=True)
        os.replace(archive, output)


if __name__ == "__main__":
    if len(sys.argv) != 5:
        fail("usage: AR OBJCOPY INPUT OUTPUT")
    main(*sys.argv[1:])
