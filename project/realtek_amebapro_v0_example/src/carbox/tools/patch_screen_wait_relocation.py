#!/usr/bin/env python3
"""Redirect validated AirPlayScreen.o wait and frame-diagnostic relocations.

The customer object is built with one ELF section per function.  This tool is
deliberately strict: it only accepts the validated AirPlayScreen_ScreenThread
layout and changes the R_ARM_THM_CALL at offset 0xee.  The optional tx-poll
mode changes only the normal-frame EAGAIN delay at offset 0xe2.  Frame-ID
modes redirect only the normal-frame dispatch and its EAGAIN print.
The tx-write mode preserves early USB screen RTO opt-in in quiet builds.
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
R_ARM_THM_JUMP24 = 30


def fail(message):
    raise SystemExit("screen-wait relocation patch: " + message)


def c_string(blob, offset):
    if offset >= len(blob):
        fail("string offset is outside its table")
    end = blob.find(b"\0", offset)
    if end < 0:
        fail("unterminated ELF string")
    return blob[offset:end].decode("ascii")


def main(path, mode="queue"):
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

    if mode == "queue":
        function_name = "AirPlayScreen_ScreenThread"
        new_name = "carbox_screen_queue_wait"
        function_size = 0x140
        call_position = 0xEE
        old_name = "vTaskDelay"
        relocation_kind = R_ARM_THM_CALL
    elif mode == "tx-poll":
        function_name = "AirPlayScreen_SendScreenNormalFrame"
        new_name = "carbox_screen_tx_poll_wait"
        function_size = 0x13C
        call_position = 0xE2
        old_name = "vTaskDelay"
        relocation_kind = R_ARM_THM_CALL
    elif mode == "tx-write":
        function_name = "AirPlayScreen_SendScreenNormalFrame"
        new_name = "carbox_screen_tcp_write"
        function_size = 0x13C
        call_position = 0xCA
        old_name = "lwip_write"
        relocation_kind = R_ARM_THM_CALL
    elif mode == "frame-dispatch":
        function_name = "AirPlayScreen_SendScreenFrame"
        new_name = "carbox_screen_normal_frame_probe"
        function_size = 0x24
        call_position = 0x18
        old_name = "AirPlayScreen_SendScreenNormalFrame"
        relocation_kind = R_ARM_THM_JUMP24
    elif mode in ("frame-block", "frame-block-quiet"):
        function_name = "AirPlayScreen_SendScreenNormalFrame"
        new_name = ("carbox_screen_block_note" if mode == "frame-block"
                    else "carbox_screen_block_quiet")
        function_size = 0x13C
        call_position = 0xDC
        old_name = "puts"
        relocation_kind = R_ARM_THM_CALL
    else:
        fail("unknown patch mode: %s" % mode)

    screen = unique_symbol(function_name)
    old_target = unique_symbol(old_name)
    new_target = unique_symbol(new_name)
    screen_section = by_name.get(".text." + function_name)
    if screen_section is None or screen["section"] != screen_section["index"]:
        fail("function symbol is not in its expected section")
    if screen["value"] not in (0, 1) or screen["size"] != function_size or \
            screen_section["size"] != function_size:
        fail("unrecognized %s layout (expected size %#x)" %
             (function_name, function_size))
    if mode == "frame-dispatch":
        normal_section = by_name.get(".text.AirPlayScreen_SendScreenNormalFrame")
        if normal_section is None or old_target["section"] != normal_section["index"]:
            fail("normal-frame function is not in its expected section")
    elif old_target["section"] != SHN_UNDEF:
        fail("%s is unexpectedly defined in the customer object" % old_name)
    if new_target["section"] != SHN_ABS or new_target["value"] != 0:
        fail("injected queue-wait symbol does not have the expected ABS placeholder")

    relocation_section = by_name.get(".rel.text." + function_name)
    if relocation_section is None or relocation_section["type"] != SHT_REL or \
            relocation_section["info"] != screen_section["index"] or \
            relocation_section["link"] != symtab["index"] or \
            relocation_section["entry_size"] != ELF32_REL.size:
        fail("unexpected function relocation section")

    matches = []
    relocation_count = relocation_section["size"] // ELF32_REL.size
    for index in range(relocation_count):
        offset = relocation_section["offset"] + index * ELF32_REL.size
        relocation_offset, relocation_info = ELF32_REL.unpack_from(image, offset)
        symbol_index = relocation_info >> 8
        relocation_type = relocation_info & 0xFF
        if relocation_offset == call_position:
            matches.append((offset, symbol_index, relocation_type))
    if len(matches) != 1:
        fail("expected one relocation at %s+%#x" %
             (function_name, call_position))
    relocation_file_offset, symbol_index, relocation_type = matches[0]
    if symbol_index != old_target["index"] or relocation_type != relocation_kind:
        fail("target is not the expected %s Thumb branch" % old_name)

    call_offset = screen_section["offset"] + call_position
    first_halfword, second_halfword = struct.unpack_from("<HH", image, call_offset)
    second_mask = 0x9000 if relocation_kind == R_ARM_THM_JUMP24 else 0xD000
    if (first_halfword & 0xF800) != 0xF000 or \
            (second_halfword & 0xD000) != second_mask:
        fail("target does not contain the expected Thumb branch encoding")

    if mode == "tx-poll":
        # The preceding instruction passes the old 10-tick delay.  Pass the
        # current screen socket instead; r5 points to .data.sockScreen here.
        arg_offset = screen_section["offset"] + 0xE0
        if struct.unpack_from("<H", image, arg_offset)[0] != 0x200A:
            fail("normal-frame delay argument is not movs r0, #10")
        if struct.unpack_from("<H", image, screen_section["offset"] + 0xC8)[0] != 0x6828:
            fail("r5 does not contain the expected screen socket pointer")
        struct.pack_into("<H", image, arg_offset, 0x6828)  # ldr r0, [r5]

    # Convert the symbol objcopy injected as ABS into a normal unresolved
    # reference, then retarget only the selected relocation to that symbol.
    struct.pack_into("<I", image, new_target["offset"] + 4, 0)
    struct.pack_into("<H", image, new_target["offset"] + 14, SHN_UNDEF)
    new_info = (new_target["index"] << 8) | relocation_kind
    struct.pack_into("<I", image, relocation_file_offset + 4, new_info)

    with open(path, "wb") as stream:
        stream.write(image)
    print("screen-wait relocation patch: redirected %s+%#x only" %
          (function_name, call_position))


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        fail("usage: patch_screen_wait_relocation.py AirPlayScreen.o [queue|tx-poll|tx-write|frame-dispatch|frame-block|frame-block-quiet]")
    main(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else "queue")
