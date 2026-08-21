#!/usr/bin/env python3
"""Redirect exactly one AirPlayScreen.o vTaskDelay relocation.

The customer object is built with one ELF section per function.  This tool is
deliberately strict: it only accepts the validated AirPlayScreen_ScreenThread
layout and changes the R_ARM_THM_CALL at offset 0xee.  The TCP EAGAIN delays
live in other function sections and remain bound to vTaskDelay.
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
SHN_ABS = 0xFFF1
R_ARM_THM_CALL = 10


def fail(message):
    raise SystemExit("screen-wait relocation patch: " + message)


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
    symbol_indices = {}
    symbol_count = symtab["size"] // symtab["entry_size"]
    for index in range(symbol_count):
        offset = symtab["offset"] + index * symtab["entry_size"]
        fields = ELF32_SYMBOL.unpack_from(image, offset)
        name = c_string(strings, fields[0])
        symbol = {
            "index": index,
            "offset": offset,
            "name": name,
            "value": fields[1],
            "size": fields[2],
            "info": fields[3],
            "other": fields[4],
            "section": fields[5],
        }
        symbols.append(symbol)
        if name:
            symbol_indices.setdefault(name, []).append(index)

    def unique_symbol(name):
        indices = symbol_indices.get(name, [])
        if len(indices) != 1:
            fail("expected exactly one symbol named %s" % name)
        return symbols[indices[0]]

    screen = unique_symbol("AirPlayScreen_ScreenThread")
    old_target = unique_symbol("vTaskDelay")
    new_target = unique_symbol("carbox_screen_queue_wait")
    screen_section = by_name.get(".text.AirPlayScreen_ScreenThread")
    if screen_section is None or screen["section"] != screen_section["index"]:
        fail("ScreenThread symbol is not in its expected function section")
    if screen["value"] not in (0, 1) or screen["size"] != 0x140 or \
            screen_section["size"] != 0x140:
        fail("unrecognized ScreenThread layout (expected size 0x140)")
    if old_target["section"] != SHN_UNDEF:
        fail("vTaskDelay is unexpectedly defined in the customer object")
    if new_target["section"] != SHN_ABS or new_target["value"] != 0:
        fail("injected queue-wait symbol does not have the expected ABS placeholder")

    relocation_section = by_name.get(".rel.text.AirPlayScreen_ScreenThread")
    if relocation_section is None or relocation_section["type"] != SHT_REL or \
            relocation_section["info"] != screen_section["index"] or \
            relocation_section["link"] != symtab["index"] or \
            relocation_section["entry_size"] != ELF32_REL.size:
        fail("unexpected ScreenThread relocation section")

    matches = []
    relocation_count = relocation_section["size"] // ELF32_REL.size
    for index in range(relocation_count):
        offset = relocation_section["offset"] + index * ELF32_REL.size
        relocation_offset, relocation_info = ELF32_REL.unpack_from(image, offset)
        symbol_index = relocation_info >> 8
        relocation_type = relocation_info & 0xFF
        if relocation_offset == 0xEE:
            matches.append((offset, symbol_index, relocation_type))
    if len(matches) != 1:
        fail("expected one relocation at ScreenThread+0xee")
    relocation_file_offset, symbol_index, relocation_type = matches[0]
    if symbol_index != old_target["index"] or relocation_type != R_ARM_THM_CALL:
        fail("ScreenThread+0xee is not the expected vTaskDelay Thumb call")

    call_offset = screen_section["offset"] + 0xEE
    first_halfword, second_halfword = struct.unpack_from("<HH", image, call_offset)
    if (first_halfword & 0xF800) != 0xF000 or (second_halfword & 0xD000) != 0xD000:
        fail("ScreenThread+0xee does not contain a Thumb BL encoding")

    # Convert the symbol objcopy injected as ABS into a normal unresolved
    # reference, then retarget only the selected relocation to that symbol.
    struct.pack_into("<I", image, new_target["offset"] + 4, 0)
    struct.pack_into("<H", image, new_target["offset"] + 14, SHN_UNDEF)
    new_info = (new_target["index"] << 8) | R_ARM_THM_CALL
    struct.pack_into("<I", image, relocation_file_offset + 4, new_info)

    with open(path, "wb") as stream:
        stream.write(image)
    print("screen-wait relocation patch: redirected ScreenThread+0xee only")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        fail("usage: patch_screen_wait_relocation.py AirPlayScreen.o")
    main(sys.argv[1])

