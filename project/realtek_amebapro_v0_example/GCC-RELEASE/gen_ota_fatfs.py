#!/usr/bin/env python3
"""Generate ota_fatfs.bin: Smart OTA header + fatfs.bin payload.

Smart header format (32 bytes):
  FileHdr  (8):  FwVer(4) + HdrNum(4) = 1
  ImgHdr  (24):  Signature(4)="OTA" + ImgHdrLen(4)=24 + Checksum(4)
                 + ImgLen(4) + Offset(4)=32 + ImgID(4)=2 (OTA_IMGID_FATFS)
"""

import struct
import sys


SMART_FILE_HDR_LEN = 8
SMART_IMG_HDR_LEN = 24
SMART_HEADER_LEN = SMART_FILE_HDR_LEN + SMART_IMG_HDR_LEN  # 32
OTA_IMGID_FATFS = 2


def checksum(data):
    """Byte-wise sum of data, truncated to 32 bits."""
    s = 0
    for b in data:
        s = (s + b) & 0xFFFFFFFF
    return s


def build_header(fw_ver, payload):
    """Return 32-byte Smart OTA header."""
    img_len = len(payload)
    csum = checksum(payload)

    # FileHdr: FwVer + HdrNum=1
    file_hdr = struct.pack("<II", fw_ver, 1)

    # ImgHdr: Signature + ImgHdrLen=24 + Checksum + ImgLen + Offset=32 + ImgID=FATFS
    img_hdr = struct.pack(
        "<4sIIIII",
        b"OTA\x00",          # Signature (4 bytes, "OTA" + NUL)
        SMART_IMG_HDR_LEN,   # ImgHdrLen
        csum,                # Checksum
        img_len,             # ImgLen
        SMART_HEADER_LEN,    # Offset (data starts after 32-byte header)
        OTA_IMGID_FATFS,     # ImgID
    )

    assert len(file_hdr + img_hdr) == SMART_HEADER_LEN
    return file_hdr + img_hdr


def main():
    if len(sys.argv) != 4:
        print(
            f"Usage: {sys.argv[0]} <fatfs.bin> <fw_ver> <ota_fatfs.bin>",
            file=sys.stderr,
        )
        sys.exit(1)

    fatfs_bin = sys.argv[1]
    fw_ver = int(sys.argv[2], 0)
    out_bin = sys.argv[3]

    with open(fatfs_bin, "rb") as f:
        payload = f.read()

    if len(payload) == 0:
        print("ERROR: fatfs.bin is empty", file=sys.stderr)
        sys.exit(1)

    header = build_header(fw_ver, payload)

    with open(out_bin, "wb") as f:
        f.write(header)
        f.write(payload)

    print(
        f"Generated {out_bin}: header=32 payload={len(payload)} "
        f"checksum=0x{checksum(payload):08x} fw_ver={fw_ver}"
    )


if __name__ == "__main__":
    main()
