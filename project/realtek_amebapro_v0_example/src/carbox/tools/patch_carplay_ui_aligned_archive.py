#!/usr/bin/env python3
"""Restore the old UI's aligned RGB->I420 and x264 dimensions.

Only audited Accessory.o revisions are accepted. The newer customer object
already selects aligned dimensions for carbox_ui_inplace; the x264 init
relocation still needs to be renamed to the aligned local entry point.
"""

import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


ACCESSORY_SHA256S = {
    "4f1b417b43f19b3bb70d41f312fd11a206f416ccbf4b19a914462a1de6c4fc45",
    "e0fc401a62a2a9a51548b259038d93f6945896785a8d9e96dc05d7db6f27434d",
    # Release 13 changes the CreateUi diagnostic string only; aligned input
    # instructions and the lib_x264_init relocation retain the audited layout.
    "1655c6a094eeb95837c75e724de7444b8d6c368d236354d24786ffbaae4e1e6a",
}
MONO_ACCESSORY_SHA256 = "829b9e730ae5e325bb4617248a1274f2de5c2de5423021e0119cac33b5951cdf"
# v14 仅将 UI 提示改为 Please Connect BT，四个字符串 addend 随之变化。
# 已核对转换/编码调用指令、位置及全部重定位，仍使用 mono 的调用布局。
V14_ACCESSORY_SHA256 = "6da7ef10a4dd8bfa2c2ad70579705552a2d7b2afa071973eca5ec205958dabe5"
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
    digest = hashlib.sha256(original).hexdigest()
    mono = digest in (MONO_ACCESSORY_SHA256, V14_ACCESSORY_SHA256)
    if digest not in ACCESSORY_SHA256S and not mono:
        raise ValueError("unknown customer Accessory.o; audit before patching")
    # Mono bundle keeps aligned RGB conversion but moves the callsites and
    # derives bpp from configuration. x264 still receives visible dimensions.
    call_offset = 0x4ae if mono else CALL_OFFSET
    init_offset = 0x4d0 if mono else 0x522
    convert_offset = 0x4b8 if mono else 0x50a
    accepted = (bytes.fromhex("089b90fbf1f1079a2846"),) if mono else (OLD, NEW)
    width = len(accepted[0])
    start, size = section_bytes(original, SECTION_NAME)
    original_instructions = original[start + call_offset:start + call_offset + width]
    if size <= call_offset + width or original_instructions not in accepted:
        raise ValueError("unexpected UI conversion instructions")

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ui-align-", dir=output.parent) as folder:
        work = Path(folder)
        member = work / "Accessory.o"
        renamed = work / "renamed.o"
        archive = work / "lib_SystemLib.a"
        member.write_bytes(original)
        before_relocs = relocations(objdump, member)
        expected = f"{init_offset:08x} R_ARM_THM_CALL    lib_x264_init"
        if before_relocs.count(expected) != 1 or f"{convert_offset:08x} R_ARM_THM_CALL    carbox_ui_inplace" not in before_relocs:
            raise ValueError("unexpected UI call relocations")
        subprocess.run([objcopy, "--redefine-sym=lib_x264_init=carbox_ui_x264_init_aligned",
                        str(member), str(renamed)], check=True)
        data = bytearray(renamed.read_bytes())
        renamed_start, renamed_size = section_bytes(data, SECTION_NAME)
        if renamed_size != size or data[renamed_start + call_offset:renamed_start + call_offset + width] != original_instructions:
            raise ValueError("objcopy changed UI code unexpectedly")
        if original_instructions == OLD:
            data[renamed_start + CALL_OFFSET:renamed_start + CALL_OFFSET + len(OLD)] = NEW
        member.write_bytes(data)
        after_relocs = relocations(objdump, member)
        if after_relocs != before_relocs.replace(
                expected, f"{init_offset:08x} R_ARM_THM_CALL    carbox_ui_x264_init_aligned"):
            raise ValueError("unexpected relocation change")
        shutil.copyfile(source, archive)
        subprocess.run([ar, "r", str(archive), str(member)], check=True)
        subprocess.run([ar, "s", str(archive)], check=True)
        os.replace(archive, output)
    print(f"patched {output}; RGB->I420 and x264 use aligned UI dimensions")


if __name__ == "__main__":
    main()
