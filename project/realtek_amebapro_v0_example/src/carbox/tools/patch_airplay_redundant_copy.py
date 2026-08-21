#!/usr/bin/env python3
"""Remove the validated duplicate payload copy in AirPlayScreen.o.

The customer AirPlayScreen_SendScreenNormalFrame function first calls memcpy
for the complete payload, then immediately copies the same source/destination
range again with a byte loop before AirPlayScreen_EncryptData.  The direct
crypto hook deliberately defers the first memcpy and encrypts from its original
source; the byte loop therefore defeats that optimization.  In the fallback
case the first memcpy has already materialized the payload, so the second copy
is redundant there as well.

This patch is intentionally tied to the fully validated relocatable-object
layout.  It changes only the BGT at function offset 0xa4 into a Thumb NOP, so
execution falls through to the crypto call.  A customer library layout change
must fail the build instead of receiving a guessed binary patch.
"""

import struct
import sys


ELF32_HEADER = struct.Struct("<16sHHIIIIIHHHHHH")
ELF32_SECTION = struct.Struct("<IIIIIIIIII")
ELF32_SYMBOL = struct.Struct("<IIIBBH")
ELF32_REL = struct.Struct("<II")

SHT_SYMTAB = 2
SHT_REL = 9
SHN_UNDEF = 0
R_ARM_THM_CALL = 10


def fail(message):
    raise SystemExit("AirPlay redundant-copy patch: " + message)


def c_string(blob, offset):
    if offset >= len(blob):
        fail("string offset is outside its table")
    end = blob.find(b"\0", offset)
    if end < 0:
        fail("unterminated ELF string")
    return blob[offset:end].decode("ascii")


def main(path):
    with open(path, "rb") as stream:
        image = bytearray(stream.read())

    if len(image) < ELF32_HEADER.size:
        fail("file is too short")
    header = ELF32_HEADER.unpack_from(image, 0)
    ident = header[0]
    if ident[:4] != b"\x7fELF" or ident[4] != 1 or ident[5] != 1:
        fail("expected ELF32 little-endian input")
    if header[1] != 1 or header[2] != 40:
        fail("expected an ARM relocatable object")

    section_offset = header[6]
    section_entry_size = header[11]
    section_count = header[12]
    section_names_index = header[13]
    if section_entry_size != ELF32_SECTION.size:
        fail("unexpected ELF section-header size")
    if section_offset + section_count * section_entry_size > len(image):
        fail("section table is outside the file")

    sections = []
    for index in range(section_count):
        fields = ELF32_SECTION.unpack_from(
            image, section_offset + index * section_entry_size)
        sections.append({
            "index": index,
            "name_offset": fields[0],
            "type": fields[1],
            "offset": fields[4],
            "size": fields[5],
            "link": fields[6],
            "info": fields[7],
            "entry_size": fields[9],
        })

    if section_names_index >= section_count:
        fail("invalid section-name string table")
    section_names = sections[section_names_index]
    names_blob = bytes(image[
        section_names["offset"]:section_names["offset"] + section_names["size"]])
    by_name = {}
    for section in sections:
        section["name"] = c_string(names_blob, section["name_offset"])
        by_name[section["name"]] = section

    symtabs = [section for section in sections if section["type"] == SHT_SYMTAB]
    if len(symtabs) != 1:
        fail("expected exactly one symbol table")
    symtab = symtabs[0]
    if symtab["entry_size"] != ELF32_SYMBOL.size or symtab["link"] >= section_count:
        fail("unexpected symbol-table layout")
    string_table = sections[symtab["link"]]
    strings = bytes(image[
        string_table["offset"]:string_table["offset"] + string_table["size"]])

    symbols = []
    symbols_by_name = {}
    symbol_count = symtab["size"] // symtab["entry_size"]
    for index in range(symbol_count):
        fields = ELF32_SYMBOL.unpack_from(
            image, symtab["offset"] + index * symtab["entry_size"])
        symbol = {
            "index": index,
            "name": c_string(strings, fields[0]),
            "value": fields[1],
            "size": fields[2],
            "section": fields[5],
        }
        symbols.append(symbol)
        if symbol["name"]:
            symbols_by_name.setdefault(symbol["name"], []).append(symbol)

    def unique_symbol(name):
        matches = symbols_by_name.get(name, [])
        if len(matches) != 1:
            fail("expected exactly one symbol named %s" % name)
        return matches[0]

    function = unique_symbol("AirPlayScreen_SendScreenNormalFrame")
    copy_target = unique_symbol("carbox_airplay_screen_memcpy")
    crypto_target = unique_symbol("AirPlayScreen_EncryptData")
    function_section = by_name.get(
        ".text.AirPlayScreen_SendScreenNormalFrame")
    if function_section is None or function["section"] != function_section["index"]:
        fail("normal-frame symbol is not in its expected function section")
    if function["value"] != 1 or function["size"] != 0x148 or \
            function_section["size"] != 0x148:
        fail("unrecognized normal-frame layout (expected Thumb size 0x148)")
    if copy_target["section"] != SHN_UNDEF:
        fail("expected the memcpy hook target to be unresolved")
    crypto_section = by_name.get(".text.AirPlayScreen_EncryptData")
    if crypto_section is None or crypto_target["section"] != crypto_section["index"] or \
            crypto_target["value"] != 1 or crypto_target["size"] != 0x84:
        fail("AirPlayScreen_EncryptData is not in its validated function section")

    relocation_section = by_name.get(
        ".rel.text.AirPlayScreen_SendScreenNormalFrame")
    if relocation_section is None or relocation_section["type"] != SHT_REL or \
            relocation_section["info"] != function_section["index"] or \
            relocation_section["link"] != symtab["index"] or \
            relocation_section["entry_size"] != ELF32_REL.size:
        fail("unexpected normal-frame relocation section")

    relocations = {}
    relocation_count = relocation_section["size"] // ELF32_REL.size
    for index in range(relocation_count):
        offset = relocation_section["offset"] + index * ELF32_REL.size
        relocation_offset, relocation_info = ELF32_REL.unpack_from(image, offset)
        relocations.setdefault(relocation_offset, []).append(
            (relocation_info >> 8, relocation_info & 0xFF))

    def require_call(offset, target):
        matches = relocations.get(offset, [])
        if matches != [(target["index"], R_ARM_THM_CALL)]:
            fail("function+0x%x is not the expected call to %s" %
                 (offset, target["name"]))

    require_call(0x94, copy_target)
    require_call(0xAC, crypto_target)

    # Validate all control/data-flow instructions that identify the duplicate
    # loop, not merely the halfword being changed.
    expected = {
        0x98: (0x464B,),             # mov r3, r9 (source cursor)
        0x9A: (0xF107, 0x027F),     # add.w r2, r7, #127 (dst - 1)
        0x9E: (0xEBA3, 0x0109),     # sub.w r1, r3, r9 (copied count)
        0xA2: (0x428E,),             # cmp r6, r1 (payload length)
        0xA4: (0xDC23,),             # bgt +0xee (copy one byte)
        0xA6: (0x4632, 0x4651, 0x4668),  # crypto arguments
        0xEE: (0xF813, 0x1B01),     # ldrb.w r1, [r3], #1
        0xF2: (0xF802, 0x1F01),     # strb.w r1, [r2, #1]!
        0xF6: (0xE7D2,),             # loop back to +0x9e
    }
    for relative_offset, halfwords in expected.items():
        file_offset = function_section["offset"] + relative_offset
        actual = struct.unpack_from("<" + "H" * len(halfwords), image, file_offset)
        if actual != halfwords:
            fail("opcode mismatch at function+0x%x: expected %s, got %s" %
                 (relative_offset,
                  "/".join("%04x" % value for value in halfwords),
                  "/".join("%04x" % value for value in actual)))

    # BGT -> NOP.  The fall-through at +0xa6 prepares arguments and calls
    # AirPlayScreen_EncryptData; the now-unreachable byte loop remains intact.
    struct.pack_into("<H", image, function_section["offset"] + 0xA4, 0xBF00)

    with open(path, "wb") as stream:
        stream.write(image)
    print("AirPlay redundant-copy patch: normal-frame+0xa4 BGT replaced by NOP")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        fail("usage: patch_airplay_redundant_copy.py AirPlayScreen.o")
    main(sys.argv[1])
