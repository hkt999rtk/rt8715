#!/usr/bin/env python3
"""Generate ota_app.bin: Smart OTA single-image header + FW payload.

Smart header format (32 bytes):
  FileHdr  (8):  FwVer + HdrNum=1
  ImgHdr  (24):  Signature="OTA" + ImgHdrLen=24 + Checksum + ImgLen
                 + Offset=32 + ImgID=1 (OTA_IMGID_APP)

Usage:
  gen_ota_app.py <firmware.bin> <fw_ver> <ota_app.bin>
"""

import struct
import sys


SMART_FILE_HDR_LEN = 8
SMART_IMG_HDR_LEN = 24
SMART_HEADER_LEN = SMART_FILE_HDR_LEN + SMART_IMG_HDR_LEN  # 32
OTA_IMGID_APP = 1


def checksum(data):
    s = 0
    for b in data:
        s = (s + b) & 0xFFFFFFFF
    return s


def main():
    if len(sys.argv) != 4:
        print(
            f"Usage: {sys.argv[0]} <firmware.bin> <fw_ver> <ota_app.bin>",
            file=sys.stderr,
        )
        sys.exit(1)

    fw_path = sys.argv[1]
    fw_ver = int(sys.argv[2], 0)
    out_path = sys.argv[3]

    with open(fw_path, "rb") as f:
        payload = f.read()

    if len(payload) == 0:
        print("ERROR: firmware.bin is empty", file=sys.stderr)
        sys.exit(1)

    # FileHdr
    file_hdr = struct.pack("<II", fw_ver, 1)

    # ImgHdr
    img_hdr = struct.pack(
        "<4sIIIII",
        b"OTA\x00",
        SMART_IMG_HDR_LEN,
        checksum(payload),
        len(payload),
        SMART_HEADER_LEN,
        OTA_IMGID_APP,
    )

    with open(out_path, "wb") as f:
        f.write(file_hdr)
        f.write(img_hdr)
        f.write(payload)

    print(
        f"Generated {out_path}: header=32 payload={len(payload)} "
        f"checksum=0x{checksum(payload):08x} fw_ver={fw_ver}"
    )


if __name__ == "__main__":
    main()
