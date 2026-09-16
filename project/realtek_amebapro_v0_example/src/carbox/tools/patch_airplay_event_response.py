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


def main(path, timing_prints=False):
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
    # The diagnostic customer archive adds GetTickCount/printf before and
    # after the original response/HID calls. Validate the entire call layout
    # before redirecting the response and optionally removing timing calls.
    layouts = {
        0x1A: (0x0A, {0x0A: "AirPlayEvent_SendResponse",
                      0x12: "AirPlayResponse_GetInfoHIDReportCommand"}),
        0x3C: (0x16, {0x06: "GetTickCount", 0x0E: "printf",
                      0x16: "AirPlayEvent_SendResponse",
                      0x1E: "AirPlayResponse_GetInfoHIDReportCommand",
                      0x22: "GetTickCount", 0x2A: "printf"}),
    }
    if (function["section"] != text["index"] or
            function["size"] != text["size"] or text["size"] not in layouts):
        fail("unrecognized HID handler layout")
    call_offset, expected_names = layouts[text["size"]]
    expected_calls = {offset: unique(name)["index"]
                      for offset, name in expected_names.items()}
    if old["section"] == SHN_UNDEF:
        fail("old response target unexpectedly undefined")
    if new["section"] != SHN_ABS or new["value"] != 0:
        fail("new target is not the injected ABS placeholder")
    matches = []
    seen_calls = {}
    for index in range(rel["size"] // ELF32_REL.size):
        pos = rel["offset"] + index * ELF32_REL.size
        r_offset, r_info = ELF32_REL.unpack_from(image, pos)
        if (r_info & 0xFF) == R_ARM_THM_CALL:
            if r_offset in seen_calls:
                fail("duplicate call relocation")
            seen_calls[r_offset] = r_info >> 8
        if r_offset == call_offset:
            matches.append((pos, r_info >> 8, r_info & 0xFF))
    if seen_calls != expected_calls:
        fail("unexpected HID handler call layout")
    if len(matches) != 1 or matches[0][1] != old["index"] or matches[0][2] != R_ARM_THM_CALL:
        fail("HID-handler+0x%x is not the expected SendResponse call" % call_offset)
    if text["size"] == 0x3C and not timing_prints:
        for offset in (0x06, 0x0E, 0x22, 0x2A):
            pos = text["offset"] + offset
            first, second = struct.unpack_from("<HH", image, pos)
            if first & 0xF800 != 0xF000 or second & 0xD000 != 0xD000:
                fail("timing call is not a Thumb BL instruction")
            # Two Thumb NOPs preserve all branch/literal offsets. R_ARM_NONE
            # prevents the linker from restoring the removed call.
            image[pos:pos + 4] = b"\x00\xbf\x00\xbf"
            for index in range(rel["size"] // ELF32_REL.size):
                reloc_pos = rel["offset"] + index * ELF32_REL.size
                if ELF32_REL.unpack_from(image, reloc_pos)[0] == offset:
                    struct.pack_into("<I", image, reloc_pos + 4, 0)
    struct.pack_into("<I", image, new["offset"] + 4, 0)
    struct.pack_into("<H", image, new["offset"] + 14, SHN_UNDEF)
    struct.pack_into("<I", image, matches[0][0] + 4,
                     (new["index"] << 8) | R_ARM_THM_CALL)
    open(path, "wb").write(image)
    print("AirPlay event response patch: redirected HID-handler+0x%x" % call_offset)
    if text["size"] == 0x3C:
        print("AirPlay event timing prints: " + ("enabled" if timing_prints else "disabled"))


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] not in ("0", "1")):
        fail("usage: patch_airplay_event_response.py AirPlayEvent.o [TIMING_PRINTS=0|1]")
    main(sys.argv[1], len(sys.argv) == 3 and sys.argv[2] == "1")
