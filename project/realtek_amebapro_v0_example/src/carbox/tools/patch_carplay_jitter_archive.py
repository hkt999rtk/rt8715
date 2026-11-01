#!/usr/bin/env python3
"""Audit and instrument the customer RTP jitter buffer object.

Only the validated AirPlayUtils.o function sections and six call relocations
are changed. The vendor archive is never modified.
"""

import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

HDR = struct.Struct("<16sHHIIIIIHHHHHH")
SEC = struct.Struct("<IIIIIIIIII")
SYM = struct.Struct("<IIIBBH")
REL = struct.Struct("<II")
UNDEF = 0
ABS = 0xFFF1
THM_CALL = 10


def checked(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def fail(why):
    raise SystemExit("CarPlay jitter patch: " + why)


def cstr(data, off):
    end = data.find(b"\0", off)
    if off >= len(data) or end < 0:
        fail("bad ELF string")
    return data[off:end].decode("ascii")


def patch_object(path):
    image = bytearray(path.read_bytes())
    h = HDR.unpack_from(image)
    if h[0][:6] != b"\x7fELF\x01\x01" or h[1:3] != (1, 40) or h[11] != SEC.size:
        fail("expected ELF32 ARM relocatable object")
    sections = []
    for i in range(h[12]):
        f = SEC.unpack_from(image, h[6] + i * SEC.size)
        sections.append(dict(idx=i, nameoff=f[0], type=f[1], off=f[4], size=f[5],
                             link=f[6], info=f[7], entsize=f[9]))
    names = sections[h[13]]
    nameblob = image[names["off"]:names["off"] + names["size"]]
    by_name = {}
    for s in sections:
        s["name"] = cstr(nameblob, s["nameoff"])
        by_name[s["name"]] = s
    tabs = [s for s in sections if s["type"] == 2]
    if len(tabs) != 1 or tabs[0]["entsize"] != SYM.size:
        fail("unexpected symbol table")
    tab = tabs[0]
    strings = sections[tab["link"]]
    strblob = image[strings["off"]:strings["off"] + strings["size"]]
    symbols = {}
    for i in range(tab["size"] // SYM.size):
        off = tab["off"] + i * SYM.size
        f = SYM.unpack_from(image, off)
        name = cstr(strblob, f[0])
        if name:
            symbols.setdefault(name, []).append(dict(idx=i, off=off, value=f[1],
                                                      size=f[2], section=f[5]))

    def symbol(name):
        found = symbols.get(name, [])
        if len(found) != 1:
            fail("expected one symbol " + name)
        return found[0]

    for name in ("carbox_jitter_discard_probe", "carbox_jitter_zero_pcm"):
        target = symbol(name)
        if target["section"] != ABS or target["value"] != 0:
            fail("invalid placeholder " + name)
        struct.pack_into("<I", image, target["off"] + 4, 0)
        struct.pack_into("<H", image, target["off"] + 14, UNDEF)

    expected = {
        ".text.RTPJitterBufferPutBusyNode": {
            0x26: ("_RTPJitterBufferDiscardExcess", "carbox_jitter_discard_probe")},
        ".text.RTPJitterBufferRead": {
            0x6e: ("_RTPJitterBufferDiscardExcess", "carbox_jitter_discard_probe"),
            0x4e: ("memset", "carbox_jitter_zero_pcm"),
            0xe6: ("memset", "carbox_jitter_zero_pcm"),
            0x2c6: ("memset", "carbox_jitter_zero_pcm"),
            0x32a: ("memset", "carbox_jitter_zero_pcm")},
    }
    for section_name, calls in expected.items():
        section = by_name.get(section_name)
        reloc = by_name.get(".rel" + section_name)
        if not section or not reloc or reloc["type"] != 9 or reloc["info"] != section["idx"]:
            fail("missing function/relocation section " + section_name)
        seen = set()
        for i in range(reloc["size"] // REL.size):
            roff = reloc["off"] + i * REL.size
            at, info = REL.unpack_from(image, roff)
            if at not in calls:
                continue
            old, new = calls[at]
            if info != ((symbol(old)["idx"] << 8) | THM_CALL):
                fail("unexpected call at %s+0x%x" % (section_name, at))
            instruction = struct.unpack_from("<HH", image, section["off"] + at)
            if (instruction[0] & 0xF800) != 0xF000 or (instruction[1] & 0xD000) != 0xD000:
                fail("expected Thumb BL at %s+0x%x" % (section_name, at))
            struct.pack_into("<I", image, roff + 4, (symbol(new)["idx"] << 8) | THM_CALL)
            seen.add(at)
        if seen != set(calls):
            fail("missing validated calls in " + section_name)

    init = by_name.get(".text.RTPJitterBufferInit")
    if not init or init["size"] < 0x234:
        fail("unrecognized RTPJitterBufferInit")
    # Minimum node count: 50 -> 64. The latency negotiation remains unchanged.
    for at, old, new in ((0x22e, 0x2b31, 0x2b3f), (0x232, 0x2332, 0x2340)):
        off = init["off"] + at
        if struct.unpack_from("<H", image, off)[0] != old:
            fail("unexpected node-count instruction at 0x%x" % at)
        struct.pack_into("<H", image, off, new)
    path.write_bytes(image)


def main(ar, objcopy, source, destination):
    source, destination = pathlib.Path(source), pathlib.Path(destination)
    with tempfile.TemporaryDirectory(prefix="carplay-jitter-") as tmp:
        tmp = pathlib.Path(tmp)
        archive = tmp / "lib_CarPlay.a"
        shutil.copy2(source, archive)
        members = subprocess.check_output((ar, "t", str(archive)), text=True).splitlines()
        if members.count("AirPlayUtils.o") != 1:
            fail("expected one AirPlayUtils.o archive member")
        subprocess.run((ar, "x", str(archive), "AirPlayUtils.o"), cwd=tmp, check=True)
        obj = tmp / "AirPlayUtils.o"
        checked(objcopy, "--globalize-symbol=_RTPJitterBufferDiscardExcess",
                "--globalize-symbol=_RTPJitterBufferBufferedSamples",
                "--add-symbol=carbox_jitter_discard_probe=0,global",
                "--add-symbol=carbox_jitter_zero_pcm=0,global", str(obj))
        patch_object(obj)
        subprocess.run((ar, "r", str(archive), "AirPlayUtils.o"), cwd=tmp, check=True,
                       stdout=subprocess.DEVNULL)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(archive, destination)
    print("CarPlay jitter patch: 64 nodes; bounded emergency purge and four PCM zero-fill probes")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        fail("usage: patch_carplay_jitter_archive.py AR OBJCOPY input.a output.a")
    main(*sys.argv[1:])
