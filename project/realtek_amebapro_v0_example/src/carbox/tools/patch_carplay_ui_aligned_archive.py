#!/usr/bin/env python3
"""Restore the old UI's aligned RGB->I420 and x264 dimensions.

Only the audited Accessory.o is accepted.  Two Thumb LDR immediates select
the aligned dimensions for carbox_ui_inplace; the x264 init relocation is
renamed to a local wrapper which applies the same alignment.  Other code and
relocations stay intact.
"""

import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


ACCESSORY_SHA256 = "4f1b417b43f19b3bb70d41f312fd11a206f416ccbf4b19a914462a1de6c4fc45"
SECTION_NAME = b".text.CAccessory_CreateUi"
CALL_OFFSET = 0x502
OLD = bytes.fromhex("0a9b099a")  # visible height, width: [sp,#40], [sp,#36]
NEW = bytes.fromhex("089b079a")  # aligned height, width: [sp,#32], [sp,#28]


def section_bytes(data, wanted):
    if data[:6] != b"\x7fELF\x01\x01" or struct.unpack_from("<H", data, 18)[0] != 40:
        raise ValueError("Accessory.o is not ARM ELF32 little-endian")
    offset = struct.unpack_from("<I", data, 32)[0]
    entry_size, count, names_index = struct.unpack_from("<HHH", data, 46)
    if entry_size != 40 or not count or names_index >= count:
        raise ValueError("unexpected ELF sections")
    entries = [struct.unpack_from("<IIIIIIIIII", data, offset + i * entry_size)
               for i in range(count)]
    names = entries[names_index]
    strings = data[names[4]:names[4] + names[5]]
    for entry in entries:
        end = strings.find(b"\0", entry[0])
        if end < 0:
            raise ValueError("malformed section name")
        if strings[entry[0]:end] == wanted:
            start, size = entry[4], entry[5]
            if start + size > len(data):
                raise ValueError("section extends past ELF")
            return start, size
    raise ValueError(f"missing section {wanted!r}")


def relocations(objdump, object_path):
    return subprocess.check_output(
        [objdump, "-r", "-j", SECTION_NAME.decode(), str(object_path)],
        text=True,
    )


def main():
    if len(sys.argv) != 5:
        raise SystemExit("usage: patch_carplay_ui_aligned_archive.py AR OBJCOPY INPUT.a OUTPUT.a")
    ar, objcopy, source, output = sys.argv[1], sys.argv[2], Path(sys.argv[3]).resolve(), Path(sys.argv[4]).resolve()
    objdump = objcopy.replace("objcopy", "objdump")
    original = subprocess.check_output([ar, "p", str(source), "Accessory.o"])
    if hashlib.sha256(original).hexdigest() != ACCESSORY_SHA256:
        raise ValueError("unknown customer Accessory.o; audit before patching")
    start, size = section_bytes(original, SECTION_NAME)
    if size <= CALL_OFFSET + len(OLD) or original[start + CALL_OFFSET:start + CALL_OFFSET + len(OLD)] != OLD:
        raise ValueError("unexpected UI conversion instructions")

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ui-align-", dir=output.parent) as folder:
        work = Path(folder)
        member = work / "Accessory.o"
        renamed = work / "renamed.o"
        archive = work / "lib_SystemLib.a"
        member.write_bytes(original)
        before_relocs = relocations(objdump, member)
        expected = "00000522 R_ARM_THM_CALL    lib_x264_init"
        if before_relocs.count(expected) != 1 or "0000050a R_ARM_THM_CALL    carbox_ui_inplace" not in before_relocs:
            raise ValueError("unexpected UI call relocations")
        subprocess.run([objcopy, "--redefine-sym=lib_x264_init=carbox_ui_x264_init_aligned",
                        str(member), str(renamed)], check=True)
        data = bytearray(renamed.read_bytes())
        renamed_start, renamed_size = section_bytes(data, SECTION_NAME)
        if renamed_size != size or data[renamed_start + CALL_OFFSET:renamed_start + CALL_OFFSET + len(OLD)] != OLD:
            raise ValueError("objcopy changed UI code unexpectedly")
        data[renamed_start + CALL_OFFSET:renamed_start + CALL_OFFSET + len(OLD)] = NEW
        member.write_bytes(data)
        after_relocs = relocations(objdump, member)
        if after_relocs != before_relocs.replace(
                expected, "00000522 R_ARM_THM_CALL    carbox_ui_x264_init_aligned"):
            raise ValueError("unexpected relocation change")
        shutil.copyfile(source, archive)
        subprocess.run([ar, "r", str(archive), str(member)], check=True)
        subprocess.run([ar, "s", str(archive)], check=True)
        os.replace(archive, output)
    print(f"patched {output}; RGB->I420 and x264 use aligned UI dimensions")


if __name__ == "__main__":
    main()
