#!/usr/bin/env python3
"""Derive a guarded RGB565 UI archive with a local encoder tail.

Edit ELF bytes directly; retain drawing/literal relocations and deliberately
replace only the retired encoding tail's calls with one local C bridge call.
"""

import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import hashlib


# The customer repacked the same 23 ABI76 members with newer ar timestamps.
X264_ABI76_SHA256S = {
    "e4a2c837959c7333c4b12cd692825e078365a8d85b86961d57aff8aa4e034cb4",
    "079e7e2ca73d26d3ed98e4859db568c7285901cf262c630bc72848df1c721d45",
}


def run(*args, cwd=None):
    return subprocess.run(args, cwd=cwd, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def ui_text_section(image):
    header = struct.Struct("<16sHHIIIIIHHHHHH")
    section = struct.Struct("<IIIIIIIIII")
    if len(image) < header.size:
        raise SystemExit("UI565 patch: short ELF object")
    values = header.unpack_from(image)
    if values[0][:6] != b"\x7fELF\x01\x01" or values[1:3] != (1, 40):
        raise SystemExit("UI565 patch: expected ARM ELF32 relocatable object")
    table, entry_size, count, names_index = values[6], values[11], values[12], values[13]
    if entry_size != section.size or table + count * entry_size > len(image):
        raise SystemExit("UI565 patch: bad ELF section table")
    sections = [section.unpack_from(image, table + n * entry_size)
                for n in range(count)]
    if names_index >= count:
        raise SystemExit("UI565 patch: missing section names")
    names = sections[names_index]
    strings = image[names[4]:names[4] + names[5]]
    for entry in sections:
        start = entry[0]
        end = strings.find(b"\0", start)
        if end < 0:
            raise SystemExit("UI565 patch: malformed section name")
        if strings[start:end] == b".text.CAccessory_CreateUi":
            if entry[5] != 0x600 or entry[4] + entry[5] > len(image):
                raise SystemExit("UI565 patch: unrecognized CAccessory_CreateUi size")
            return entry[4], entry[5]
    raise SystemExit("UI565 patch: CAccessory_CreateUi section missing")


def main():
    if len(sys.argv) != 6:
        raise SystemExit("usage: patch_carplay_ui565_archive.py AR OBJCOPY INPUT OUTPUT X264_ARCHIVE")
    ar, objcopy, source, output, x264 = map(pathlib.Path, sys.argv[1:])
    ar = ar.resolve()
    objcopy = objcopy.resolve()
    source = source.resolve()
    output = output.resolve()
    x264 = x264.resolve()
    if hashlib.sha256(x264.read_bytes()).hexdigest() not in X264_ABI76_SHA256S:
        raise SystemExit("UI565 patch: lib_x264.a changed; revalidate picture ABI offsets")
    objdump = objcopy.with_name(objcopy.name.replace("objcopy", "objdump"))
    nm = objcopy.with_name(objcopy.name.replace("objcopy", "nm"))
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="ui565-", dir=output.parent) as tmp:
        work = pathlib.Path(tmp)
        archive = work / "lib_SystemLib_ui565.a"
        shutil.copyfile(source, archive)
        run(str(ar), "x", str(source), "Accessory.o", cwd=work)
        member = work / "Accessory.o"
        weakened = work / "weakened.o"
        run(str(objcopy), "--weaken-symbol=RGB2YUV",
            "--redefine-sym=lib_x264_init=carbox_ui_encode_surface",
            str(member), str(weakened))
        image = bytearray(weakened.read_bytes())
        section_offset, section_size = ui_text_section(image)
        code = image[section_offset:section_offset + section_size]
        # Thumb movs r2,#0 before the main surface creation; movs r2,#3
        # before RGB2YUV.  Format 2 is RGB565, and bpp 2 reaches our converter.
        for offset, expected in ((0x4e, b"\x00\x22"),
                                 (0x536, b"\x03\x22")):
            if code[offset:offset + 2] != expected:
                raise SystemExit("UI565 patch: unexpected instruction at %#x" % offset)
        relocations = run(str(objdump), "-r", "-j", ".text.CAccessory_CreateUi",
                          str(weakened)).stdout
        required = (
            "00000056 R_ARM_THM_CALL    CSurface_CreateSurface",
            "00000542 R_ARM_THM_CALL    RGB2YUV",
            "00000562 R_ARM_THM_CALL    carbox_ui_encode_surface",
            "0000056c R_ARM_THM_CALL    lib_x264_encode",
            "00000578 R_ARM_THM_CALL    lib_x264_encode",
            "00000582 R_ARM_THM_CALL    lib_x264_exit",
        )
        for line in required:
            if line not in relocations:
                raise SystemExit("UI565 patch: missing expected relocation: " + line)
        if hashlib.sha256(code).hexdigest() != "7ba602f7d764bd79615e2f7208ab80e5edc9cd9e1b96ee8970791189b6a21d6c":
            raise SystemExit("UI patch: CreateUi code changed; re-audit tail ABI")
        code[0x4e] = 2
        # r0=surface, r1=width, r2=height, r3=fps, stack[0]=filename.
        # LDR r5 uses the retained literal at 0x5f8; B targets epilogue 0x5ac.
        stub = bytes.fromhex("2046 0899 099a 0a9b 364d 0095 fff7feff 41e0")
        code[0x516:0x5ac] = b"\x00\xbf" * ((0x5ac-0x516)//2)
        code[0x516:0x516+len(stub)] = stub
        image[section_offset:section_offset + section_size] = code
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", image)
        sections = [struct.unpack_from("<IIIIIIIIII", image, header[6]+i*header[11])
                    for i in range(header[12])]
        text_index = next(i for i,s in enumerate(sections) if s[4] == section_offset)
        changed = []
        for sec in sections:
            if sec[1] != 9 or sec[7] != text_index: continue  # SHT_REL / sh_info
            for pos in range(sec[4], sec[4]+sec[5], 8):
                offset, info = struct.unpack_from("<II",image,pos)
                if not 0x516 <= offset < 0x5ac: continue
                if offset == 0x562:
                    struct.pack_into("<II", image, pos, 0x522, info)
                else:
                    struct.pack_into("<II", image, pos, offset, info & ~255) # R_ARM_NONE
                changed.append(offset)
        if len(changed) != 15 or 0x562 not in changed:
            raise SystemExit("UI patch: unexpected retired-tail relocations: " + str(changed))
        member.write_bytes(image)
        after = run(str(objdump), "-r", "-j", ".text.CAccessory_CreateUi", str(member)).stdout
        if "00000522 R_ARM_THM_CALL    carbox_ui_encode_surface" not in after:
            raise SystemExit("UI patch: missing bridge relocation")
        # Every relocation outside the retired tail must remain byte identical.
        before_image = weakened.read_bytes()
        for sec in sections:
            if sec[1] != 9: continue
            for pos in range(sec[4],sec[4]+sec[5],8):
                offset = struct.unpack_from("<I",before_image,pos)[0]
                if sec[7] == text_index and 0x516 <= offset < 0x5ac: continue
                if image[pos:pos+8] != before_image[pos:pos+8]:
                    raise SystemExit("UI patch: unrelated relocation changed")
        run(str(ar), "r", str(archive), str(member))
        run(str(ar), "s", str(archive))
        archive.replace(output)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit("UI565 patch: tool failed: " + " ".join(error.cmd)
                         + "\n" + error.stderr) from error
