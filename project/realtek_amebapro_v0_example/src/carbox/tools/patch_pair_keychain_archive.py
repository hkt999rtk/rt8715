#!/usr/bin/env python3
"""Route only KeychainLiteFile's locked read/write sites through a RAM cache.

The customer archive is never changed. A different vendor object fails the
build until its calls and cache lifetime have been audited again.
"""

import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


MEMBER = "KeychainLiteFile.o"
VENDOR_SHA256 = "b5606615a8a00022d8bf4f681239c13bac62f235af599273f1c944f242690730"
ROUTES = {
    ".text.SecItemAdd_compat": [(0x14, "read"), (0xDE, "write")],
    ".text.SecItemCopyMatching_compat": [(0x1A, "read")],
    ".text.SecItemDelete_compat": [(0x14, "read"), (0x116, "write")],
    ".text.SecItemUpdate_compat": [(0x14, "read"), (0x46, "write")],
}


def fail(message):
    raise SystemExit("pair keychain patch: " + message)


def patch_relocations(path):
    data = bytearray(path.read_bytes())
    elf = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    if elf[0][:6] != b"\x7fELF\x01\x01" or elf[1:3] != (1, 40) or elf[11] != 40:
        fail("expected ARM ELF32 relocatable")
    sections = [struct.unpack_from("<10I", data, elf[6] + 40 * n)
                for n in range(elf[12])]

    def section_data(section):
        return data[section[4]:section[4] + section[5]]

    def cstr(table, offset):
        end = table.find(0, offset)
        if end < 0:
            fail("unterminated string")
        return table[offset:end].decode("ascii")

    names = section_data(sections[elf[13]])
    by_name = {cstr(names, section[0]): (n, section)
               for n, section in enumerate(sections)}
    symbol_tables = [(n, section) for n, section in enumerate(sections)
                     if section[1] == 2]
    if len(symbol_tables) != 1:
        fail("symbol table count")
    table_index, table = symbol_tables[0]
    symbols = {}
    strings = section_data(sections[table[6]])
    for n in range(table[5] // 16):
        offset = table[4] + n * 16
        record = struct.unpack_from("<IIIBBH", data, offset)
        name = cstr(strings, record[0])
        if name:
            symbols[name] = (n, offset, record)

    targets = {}
    for kind in ("read", "write"):
        old_name = "carbox_pair_keychain_" + kind + "_uncached"
        new_name = "carbox_pair_keychain_" + kind + "_cached"
        if old_name not in symbols or new_name not in symbols:
            fail("missing " + kind + " symbols")
        old = symbols[old_name]
        new = symbols[new_name]
        if old[2][5] == 0 or new[2][5] != 0xFFF1:
            fail("unexpected " + kind + " symbol binding")
        # objcopy adds the wrapper as an absolute placeholder. Make it a true
        # undefined symbol so the application object supplies the function.
        struct.pack_into("<H", data, new[1] + 14, 0)
        targets[kind] = (old[0], new[0])

    for name, routes in ROUTES.items():
        if name not in by_name or ".rel" + name not in by_name:
            fail("missing section " + name)
        text_index, text_section = by_name[name]
        _, rel_section = by_name[".rel" + name]
        if rel_section[1] != 9 or rel_section[6:8] != (table_index, text_index):
            fail("bad relocation section " + name)
        before = bytes(section_data(text_section))
        for location, kind in routes:
            old_index, new_index = targets[kind]
            matches = []
            for offset in range(rel_section[4], rel_section[4] + rel_section[5], 8):
                call_offset, info = struct.unpack_from("<II", data, offset)
                if call_offset == location:
                    matches.append((offset, info))
            if len(matches) != 1 or matches[0][1] != ((old_index << 8) | 10):
                fail("unexpected " + name + "+0x%x" % location)
            struct.pack_into("<I", data, matches[0][0] + 4,
                             (new_index << 8) | 10)
        if bytes(section_data(text_section)) != before:
            fail("instruction bytes changed in " + name)
    path.write_bytes(data)


def main(ar, objcopy, source, output):
    source = Path(source)
    output = Path(output)
    vendor = subprocess.check_output([ar, "p", str(source), MEMBER])
    if hashlib.sha256(vendor).hexdigest() != VENDOR_SHA256:
        fail(MEMBER + " changed; re-audit before enabling cache")
    with tempfile.TemporaryDirectory(prefix="pair-keychain-", dir=output.parent) as tmp:
        work = Path(tmp)
        archive = work / "archive.a"
        archive.write_bytes(source.read_bytes())
        member = work / MEMBER
        member.write_bytes(vendor)
        subprocess.run([
            objcopy,
            "--redefine-sym=_ReadKeychain=carbox_pair_keychain_read_uncached",
            "--redefine-sym=_WriteKeychain=carbox_pair_keychain_write_uncached",
            "--globalize-symbol=carbox_pair_keychain_read_uncached",
            "--globalize-symbol=carbox_pair_keychain_write_uncached",
            "--globalize-symbol=gKeychainItems",
            "--add-symbol=carbox_pair_keychain_read_cached=0,global",
            "--add-symbol=carbox_pair_keychain_write_cached=0,global",
            str(member),
        ], check=True)
        patch_relocations(member)
        subprocess.run([ar, "r", str(archive), str(member)], check=True)
        subprocess.run([ar, "s", str(archive)], check=True)
        os.replace(archive, output)


if __name__ == "__main__":
    if len(sys.argv) != 5:
        fail("usage: AR OBJCOPY INPUT OUTPUT")
    main(*sys.argv[1:])
