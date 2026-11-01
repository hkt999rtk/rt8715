#!/usr/bin/env python3
"""Redirect exactly three validated CarPlay PCM TX call sites in AirPlayAudio.o."""

import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


MEMBER = "AirPlayAudio.o"
VENDOR_SHA256 = "c6b7b73d60de9bb1ff422b2b9aa2d972b9c44200dba33a0f2601937dbc27aaa7"
CALLS = {
    ("AirPlayAudio_SendAudio", 0x54):
        ("RingBuffer_push", "carbox_audio_pcm_tx_push", 0xA0),
    ("AirPlayAudio_AudioOutputThread", 0xC6):
        ("RingBuffer_pop", "carbox_audio_pcm_tx_pop", 0x180),
    ("AirPlayAudio_AudioOutputThread", 0x108):
        ("lwip_sendto", "carbox_audio_pcm_tx_sendto", 0x180),
}


def fail(message):
    raise SystemExit("PCM TX diagnostic patch: " + message)


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
    symtabs = [(index, section) for index, section in enumerate(sections)
               if section[1] == 2]
    if len(symtabs) != 1:
        fail("expected one symbol table")
    table_index, table = symtabs[0]
    strings = section_data(sections[table[6]])
    symbols = {}
    for index in range(table[5] // 16):
        offset = table[4] + index * 16
        record = struct.unpack_from("<IIIBBH", image, offset)
        name = string(strings, record[0])
        if name:
            symbols[name] = (index, offset, record)

    for (function, call_offset), (original, replacement, expected_size) in CALLS.items():
        section_name = ".text." + function
        relocation_name = ".rel" + section_name
        if section_name not in by_name or relocation_name not in by_name:
            fail("missing function or relocations for " + function)
        function_index, section = by_name[section_name]
        _, relocations = by_name[relocation_name]
        if section[5] != expected_size or relocations[1] != 9 or \
                relocations[6:8] != (table_index, function_index):
            fail("unexpected layout for " + function)
        if original not in symbols or replacement not in symbols:
            fail("missing symbol for " + function)
        old_index, _, old_record = symbols[original]
        new_index, new_offset, new_record = symbols[replacement]
        if old_record[5] != 0 or new_record[5] != 0xFFF1:
            fail("unexpected symbol binding for " + function)
        matches = []
        for position in range(relocations[4], relocations[4] + relocations[5], 8):
            offset, info = struct.unpack_from("<II", image, position)
            if offset == call_offset:
                matches.append((position, info))
        if len(matches) != 1 or matches[0][1] != (old_index << 8) | 10:
            fail("unexpected call relocation for %s+%#x" % (function, call_offset))
        instruction = bytes(image[section[4] + call_offset:section[4] + call_offset + 4])
        if instruction != bytes.fromhex("fff7feff"):
            fail("unexpected Thumb BL for %s+%#x" % (function, call_offset))
        struct.pack_into("<I", image, matches[0][0] + 4, (new_index << 8) | 10)
        struct.pack_into("<H", image, new_offset + 14, 0)  # ABS placeholder -> UND

    path.write_bytes(image)


def main(ar, objcopy, source, output):
    source = Path(source)
    output = Path(output)
    vendor = subprocess.check_output([ar, "p", str(source), MEMBER])
    if hashlib.sha256(vendor).hexdigest() != VENDOR_SHA256:
        fail(MEMBER + " changed; re-audit call sites before patching")
    with tempfile.TemporaryDirectory(prefix="pcm-tx-diag-", dir=output.parent) as tmp:
        work = Path(tmp)
        archive = work / "archive.a"
        archive.write_bytes(source.read_bytes())
        member = work / MEMBER
        member.write_bytes(vendor)
        command = [objcopy]
        command += ["--add-symbol=" + replacement + "=0,global"
                    for _, replacement, _ in CALLS.values()]
        subprocess.run(command + [str(member)], check=True)
        patch_member(member)
        subprocess.run([ar, "r", str(archive), str(member)], check=True)
        subprocess.run([ar, "s", str(archive)], check=True)
        os.replace(archive, output)


if __name__ == "__main__":
    if len(sys.argv) != 5:
        fail("usage: AR OBJCOPY INPUT OUTPUT")
    main(*sys.argv[1:])
