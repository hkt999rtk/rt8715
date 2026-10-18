#!/usr/bin/env python3
"""Fix the two RGB565/I420 dimension mistakes in customer Accessory.o.

The customer source is unavailable in this workspace.  This edits only the
validated Thumb instruction bytes in two ELF sections; section sizes and
relocations are left intact.  Equivalent source changes are:

    rows = 2 * (((width + 15) & ~15) * bytes_per_pixel);
    carbox_ui_inplace(pixels, 2, visible_width, visible_height);

Usage: patch_carplay_ui_stride_archive.py <arm-ar> <input.a> <output.a>
"""

import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


ACCESSORY_SHA256 = "c9367f8d10cee59a518fcd3e33475873527361b87452aa24a42293c95164a339"
EDITS = (
    # Align the pixel width before doubling it for a pair of RGB rows.
    (".text.carbox_ui_inplace", 0x46,
     bytes.fromhex("6d0025f00f05"), bytes.fromhex("25f00f056d00")),
    # Convert only visible pixels; x264_init receives these same dimensions.
    (".text.CAccessory_CreateUi", 0x502,
     bytes.fromhex("089b079a"), bytes.fromhex("0a9b099a")),
)


def sections(data):
    if data[:6] != b"\x7fELF\x01\x01" or struct.unpack_from("<H", data, 18)[0] != 40:
        raise ValueError("Accessory.o is not little-endian ARM ELF32")
    shoff = struct.unpack_from("<I", data, 32)[0]
    entsize, count, names_index = struct.unpack_from("<HHH", data, 46)
    if entsize != 40 or not count or names_index >= count:
        raise ValueError("unexpected ELF section table")
    table = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * entsize)
             for i in range(count)]
    shstr = table[names_index]
    names = data[shstr[4]:shstr[4] + shstr[5]]
    result = {}
    for entry in table:
        end = names.find(b"\x00", entry[0])
        if end < 0:
            raise ValueError("invalid ELF section name")
        result[names[entry[0]:end].decode("ascii")] = (entry[4], entry[5])
    return result


def patch_object(data):
    if hashlib.sha256(data).hexdigest() != ACCESSORY_SHA256:
        raise ValueError("unknown Accessory.o; audit its instructions before patching")
    result = bytearray(data)
    index = sections(data)
    for name, offset, old, new in EDITS:
        start, size = index[name]
        if offset + len(old) > size or result[start + offset:start + offset + len(old)] != old:
            raise ValueError(f"unexpected instruction bytes in {name}+0x{offset:x}")
        result[start + offset:start + offset + len(old)] = new
    return bytes(result)


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: patch_carplay_ui_stride_archive.py <arm-ar> <input.a> <output.a>")
    ar, source, output = sys.argv[1], Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve()
    original = subprocess.check_output([ar, "p", str(source), "Accessory.o"])
    replacement = patch_object(original)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix="ui-stride-") as tmp:
        work = Path(tmp)
        archive = work / "lib_SystemLib.a"
        member = work / "Accessory.o"
        shutil.copyfile(source, archive)
        member.write_bytes(replacement)
        subprocess.run([ar, "r", str(archive), str(member)], check=True)
        subprocess.run([ar, "s", str(archive)], check=True)
        os.replace(archive, output)
    print(f"patched {output}; Accessory.o SHA-256={hashlib.sha256(replacement).hexdigest()}")


if __name__ == "__main__":
    main()
