#!/usr/bin/env python3
"""Retry the vehicle event parser before returning to socket select()."""

import struct
import sys

ELF32_HEADER = struct.Struct("<16sHHIIIIIHHHHHH")
ELF32_SECTION = struct.Struct("<IIIIIIIIII")
ELF32_SYMBOL = struct.Struct("<IIIBBH")
ELF32_REL = struct.Struct("<II")
SHT_SYMTAB = 2
SHT_REL = 9
R_ARM_THM_CALL = 10


def fail(message):
    raise SystemExit("AirPlay event read-ahead patch: " + message)


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
    symtabs = [section for section in sections if section["type"] == SHT_SYMTAB]
    if len(symtabs) != 1:
        fail("expected one symbol table")
    symtab = symtabs[0]
    strtab = sections[symtab["link"]]
    strings = bytes(image[strtab["offset"]:strtab["offset"] + strtab["size"]])
    symbols = []
    by_symbol = {}
    for index in range(symtab["size"] // symtab["entsize"]):
        fields = ELF32_SYMBOL.unpack_from(image,
            symtab["offset"] + index * symtab["entsize"])
        symbol = {"index": index, "name": c_string(strings, fields[0]),
                  "value": fields[1], "size": fields[2], "section": fields[5]}
        symbols.append(symbol)
        if symbol["name"]:
            by_symbol.setdefault(symbol["name"], []).append(symbol)

    def unique(name):
        found = by_symbol.get(name, [])
        if len(found) != 1:
            fail("expected one symbol named " + name)
        return found[0]

    function = unique("AirPlayEvent_TCPClientThread")
    read_message = unique("HTTPMessageReadMessage")
    reset = unique("HTTPMessageReset")
    text = by_name.get(".text.AirPlayEvent_TCPClientThread")
    rel = by_name.get(".rel.text.AirPlayEvent_TCPClientThread")
    if text is None or rel is None or rel["type"] != SHT_REL:
        fail("missing validated TCPClient sections")
    if (function["section"] != text["index"] or function["value"] != 1 or
            function["size"] != 0x25C or text["size"] != 0x25C):
        fail("unrecognized TCPClient layout")

    expected_calls = {0x10E: read_message["index"], 0x136: reset["index"]}
    matched = set()
    for index in range(rel["size"] // ELF32_REL.size):
        r_offset, r_info = ELF32_REL.unpack_from(
            image, rel["offset"] + index * ELF32_REL.size)
        if r_offset in expected_calls:
            if (r_info >> 8) != expected_calls[r_offset] or (r_info & 0xFF) != R_ARM_THM_CALL:
                fail("unexpected relocation at TCPClient+0x%x" % r_offset)
            matched.add(r_offset)
    if matched != set(expected_calls):
        fail("missing read/reset relocation in TCPClient")

    # Vendor code branches from immediately after HTTPMessageReset back to
    # select() at +0xda.  Redirect it to the existing parser entry at +0x104.
    # HTTPMessageReadMessage consumes retained bytes first and reads only the
    # missing portion; EAGAIN still follows its original path back to select.
    branch_offset = text["offset"] + 0x13A
    old_branch = struct.unpack_from("<H", image, branch_offset)[0]
    if old_branch != 0xE7CE:
        fail("TCPClient+0x13a is not the expected branch to select")
    struct.pack_into("<H", image, branch_offset, 0xE7E3)
    open(path, "wb").write(image)
    print("AirPlay event read-ahead patch: reset now retries parser before select")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        fail("usage: patch_airplay_event_readahead.py AirPlayEvent.o")
    main(sys.argv[1])
