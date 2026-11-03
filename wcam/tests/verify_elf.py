#!/usr/bin/env python3
"""Inspect the actual linked UVC descriptor bytes, using only Python stdlib."""
import pathlib
import struct
import sys

data = pathlib.Path(sys.argv[1]).read_bytes()
assert data[:6] == b"\x7fELF\x01\x01", "Expected little-endian ELF32"
header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
sections = [struct.unpack_from("<10I", data, header[6] + i * header[11])
            for i in range(header[12])]
symbols = {}
for section in sections:
    if section[1] != 2:
        continue
    strings = sections[section[6]]
    strings = data[strings[4]:strings[4] + strings[5]]
    for pos in range(section[4], section[4] + section[5], section[9]):
        name, address, size, info, other, index = struct.unpack_from("<IIIBBH", data, pos)
        if name and index:
            name = strings[name:strings.index(b"\0", name)].decode()
            symbols[name] = (address, size)


def read(address, length):
    for section in sections:
        if section[1] != 8 and section[3] <= address and address + length <= section[3] + section[5]:
            offset = section[4] + address - section[3]
            return data[offset:offset + length]
    raise AssertionError(f"Address not file-backed: {address:#x}")


def descriptors(name):
    address, size = symbols[name]
    for offset in range(0, size, 4):
        pointer, = struct.unpack("<I", read(address + offset, 4))
        if pointer == 0:
            return
        length = read(pointer, 1)[0]
        assert length >= 3, "Malformed descriptor length"
        yield read(pointer, length)
    raise AssertionError("Missing descriptor-list terminator")


for name in ("usbd_uvc_descriptors_FS", "usbd_uvc_descriptors_HS"):
    streaming = False
    formats, frames, inputs = [], [], []
    for desc in descriptors(name):
        if desc[1] == 4:
            streaming = desc[5:7] == b"\x0e\x02"
        if not streaming or desc[1] != 0x24:
            continue
        if desc[2] == 1:
            inputs.append(desc)
        elif desc[2] == 0x10:
            formats.append(desc)
        elif desc[2] == 0x11:
            frames.append(desc)
        else:
            assert desc[2] == 0x0d, "Unexpected advertised format"
    assert len(inputs) == len(formats) == len(frames) == 1
    assert inputs[0][0] == 14 and inputs[0][3] == 1
    fmt, frame = formats[0], frames[0]
    assert fmt[3:5] == b"\x01\x01"
    assert fmt[5:21] == b"H264\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"
    assert fmt[27] == 1
    assert frame[0] == 30 and frame[3] == 1
    assert struct.unpack_from("<HH", frame, 5) == (1280, 720)
    assert struct.unpack_from("<I", frame, 17)[0] == 333333
    assert frame[21] == 1 and struct.unpack_from("<I", frame, 26)[0] == 333333
    print(f"{name}: H264 only, 1280x720, 30 fps")

for required in ("wcam_start", "h264_encode", "usbd_uvc_init", "uvc_events_process",
                 "__wrap_usb_composite_probe"):
    assert required in symbols, required
for forbidden in ("isp_init", "isp_stream_start", "uvcd_handle"):
    assert forbidden not in symbols, forbidden
assert not any(any(word in name.lower() for word in ("carbox", "carplay", "airplay", "usbsmart"))
               for name in symbols)
# The archived init uses a relocated function pointer for registration.
# Verify the linker really redirected it to our pre-enumeration callback setup.
address, size = symbols["usbd_uvc_init"]
body = read(address & ~1, size)
wrapped = symbols["__wrap_usb_composite_probe"][0] | 1
assert struct.pack("<I", wrapped) in body, "UVC init bypasses callback wrapper"
print("ELF: native encoder/USB present, wrapper active, no sensor pipeline/customer symbols")
