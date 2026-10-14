#!/usr/bin/env python3
"""Route only the two /info resource decisions through the 450.x shim.

The vendor AirPlayResponse.o is copied into a derived archive.  Its code and
all other relocations remain unchanged.  Reject an unknown member revision.
"""

import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


MEMBER = "AirPlayResponse.o"
VENDOR_SHA256 = "df2f9654df237f6fd28fe2e36a86eb46ebd2ca0f8a28db7ea0089b80972ac6a4"
FUNCTION = ".text.AirPlayResponse_GetInfoInfoModes"
CALLS = (0x1F8, 0x234)
ORIGINAL = "AirPlayResponse_ParseResource"
SHIM = "carbox_parse_resource_450_compat"


def fail(message):
    raise SystemExit("resource 450 patch: " + message)


def patch_member(path):
    image = bytearray(path.read_bytes())
    elf = struct.unpack_from("<16sHHIIIIIHHHHHH", image)
    if elf[0][:6] != b"\x7fELF\x01\x01" or elf[1:3] != (1, 40) or elf[11] != 40:
        fail("expected ARM ELF32 little-endian relocatable object")
    sections = [struct.unpack_from("<10I", image, elf[6] + 40 * n)
                for n in range(elf[12])]

    def section_data(section):
        return image[section[4]:section[4] + section[5]]

    def string(blob, offset):
        end = blob.find(0, offset)
        if end < 0:
            fail("unterminated ELF string")
        return blob[offset:end].decode("ascii")

    names = section_data(sections[elf[13]])
    by_name = {string(names, section[0]): (index, section)
               for index, section in enumerate(sections)}
    if FUNCTION not in by_name or ".rel" + FUNCTION not in by_name:
        fail("missing GetInfoInfoModes function or relocations")
    function_index, function = by_name[FUNCTION]
    _, relocations = by_name[".rel" + FUNCTION]
    tables = [(index, section) for index, section in enumerate(sections)
              if section[1] == 2]
    if len(tables) != 1:
        fail("expected one ELF symbol table")
    table_index, table = tables[0]
    if relocations[1] != 9 or relocations[6:8] != (table_index, function_index):
        fail("unexpected relocation section")
    strings = section_data(sections[table[6]])
    symbols = {}
    for index in range(table[5] // 16):
        offset = table[4] + index * 16
        record = struct.unpack_from("<IIIBBH", image, offset)
        name = string(strings, record[0])
        if name:
            symbols[name] = (index, offset, record)
    if ORIGINAL not in symbols or SHIM not in symbols:
        fail("missing original or shim symbol")
    original = symbols[ORIGINAL]
    shim = symbols[SHIM]
    if original[2][5] == 0 or shim[2][5] != 0xFFF1:
        fail("unexpected symbol binding")
    if function[5] < max(CALLS) + 4:
        fail("GetInfoInfoModes is too short")
    found = set()
    for position in range(relocations[4], relocations[4] + relocations[5], 8):
        call_offset, info = struct.unpack_from("<II", image, position)
        if call_offset not in CALLS:
            continue
        if call_offset in found or info != ((original[0] << 8) | 10):
            fail("unexpected resource call at +0x%x" % call_offset)
        code = image[function[4] + call_offset:function[4] + call_offset + 4]
        if code != bytes.fromhex("fff7feff"):
            fail("unexpected Thumb BL at +0x%x" % call_offset)
        struct.pack_into("<I", image, position + 4, (shim[0] << 8) | 10)
        found.add(call_offset)
    if found != set(CALLS):
        fail("missing resource call relocation")
    struct.pack_into("<H", image, shim[1] + 14, 0)  # ABS placeholder -> UND
    path.write_bytes(image)


def main(ar, objcopy, source, output):
    source = Path(source)
    output = Path(output)
    vendor = subprocess.check_output([ar, "p", str(source), MEMBER])
    if hashlib.sha256(vendor).hexdigest() != VENDOR_SHA256:
        fail(MEMBER + " changed; re-audit before patching")
    with tempfile.TemporaryDirectory(prefix="resource450-", dir=output.parent) as tmp:
        work = Path(tmp)
        archive = work / "archive.a"
        archive.write_bytes(source.read_bytes())
        member = work / MEMBER
        member.write_bytes(vendor)
        subprocess.run([objcopy, "--add-symbol=" + SHIM + "=0,global",
                        str(member)], check=True)
        patch_member(member)
        subprocess.run([ar, "r", str(archive), str(member)], check=True)
        subprocess.run([ar, "s", str(archive)], check=True)
        os.replace(archive, output)


if __name__ == "__main__":
    if len(sys.argv) != 5:
        fail("usage: AR OBJCOPY INPUT OUTPUT")
    main(*sys.argv[1:])
