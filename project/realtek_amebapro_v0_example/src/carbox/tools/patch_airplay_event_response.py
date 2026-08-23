#!/usr/bin/env python3
"""Redirect only the HID report's empty response to the cached responder."""

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
    raise SystemExit("AirPlay event response patch: " + message)


def c_string(blob, offset):
    if offset >= len(blob):
        fail("string offset outside table")
    end = blob.find(b"\0", offset)
    if end < 0:
        fail("unterminated string")
    return blob[offset:end].decode("ascii")


def main(path):
    image = bytearray(open(path, "rb").read())
    header = ELF32_HEADER.unpack_from(image, 0)
    if header[0][:6] != b"\x7fELF\x01\x01" or header[1] != 1 or header[2] != 40:
        fail("expected ARM ELF32 little-endian relocatable object")
    shoff, shentsize, shnum, shstrndx = header[6], header[11], header[12], header[13]
    if shentsize != ELF32_SECTION.size:
        fail("unexpected section header size")
    sections = []
    for index in range(shnum):
        fields = ELF32_SECTION.unpack_from(image, shoff + index * shentsize)
        sections.append({"index": index, "name_off": fields[0], "type": fields[1],
                         "offset": fields[4], "size": fields[5], "link": fields[6],
                         "info": fields[7], "entsize": fields[9]})
    shstr = sections[shstrndx]
    names = bytes(image[shstr["offset"]:shstr["offset"] + shstr["size"]])
    by_name = {}
    for section in sections:
        section["name"] = c_string(names, section["name_off"])
        by_name[section["name"]] = section
    symtabs = [s for s in sections if s["type"] == SHT_SYMTAB]
    if len(symtabs) != 1:
        fail("expected one symbol table")
    symtab = symtabs[0]
    strtab = sections[symtab["link"]]
    strings = bytes(image[strtab["offset"]:strtab["offset"] + strtab["size"]])
    symbols = []
    by_symbol = {}
    for index in range(symtab["size"] // symtab["entsize"]):
        offset = symtab["offset"] + index * symtab["entsize"]
        fields = ELF32_SYMBOL.unpack_from(image, offset)
        symbol = {"index": index, "offset": offset,
                  "name": c_string(strings, fields[0]), "value": fields[1],
                  "size": fields[2], "section": fields[5]}
        symbols.append(symbol)
        if symbol["name"]:
            by_symbol.setdefault(symbol["name"], []).append(symbol)

    def unique(name):
        found = by_symbol.get(name, [])
        if len(found) != 1:
            fail("expected one symbol named " + name)
        return found[0]

    function = unique("AirPlayEvent_DealWithSendHIDReport")
    old = unique("AirPlayEvent_SendResponse")
    new = unique("carbox_airplay_event_send_fast_response")
    text = by_name.get(".text.AirPlayEvent_DealWithSendHIDReport")
    rel = by_name.get(".rel.text.AirPlayEvent_DealWithSendHIDReport")
    if text is None or rel is None or rel["type"] != SHT_REL:
        fail("missing validated event-handler sections")
    if function["section"] != text["index"] or function["size"] != 0x1A or text["size"] != 0x1A:
        fail("unrecognized HID handler layout")
    if old["section"] == SHN_UNDEF:
        fail("old response target unexpectedly undefined")
    if new["section"] != SHN_ABS or new["value"] != 0:
        fail("new target is not the injected ABS placeholder")
    matches = []
    for index in range(rel["size"] // ELF32_REL.size):
        pos = rel["offset"] + index * ELF32_REL.size
        r_offset, r_info = ELF32_REL.unpack_from(image, pos)
        if r_offset == 0x0A:
            matches.append((pos, r_info >> 8, r_info & 0xFF))
    if len(matches) != 1 or matches[0][1] != old["index"] or matches[0][2] != R_ARM_THM_CALL:
        fail("HID-handler+0x0a is not the expected SendResponse call")
    struct.pack_into("<I", image, new["offset"] + 4, 0)
    struct.pack_into("<H", image, new["offset"] + 14, SHN_UNDEF)
    struct.pack_into("<I", image, matches[0][0] + 4,
                     (new["index"] << 8) | R_ARM_THM_CALL)
    open(path, "wb").write(image)
    print("AirPlay event response patch: redirected HID-handler+0x0a")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        fail("usage: patch_airplay_event_response.py AirPlayEvent.o")
    main(sys.argv[1])
