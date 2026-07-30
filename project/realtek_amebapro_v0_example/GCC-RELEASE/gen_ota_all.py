#!/usr/bin/env python3
"""Generate ota_all.bin: Smart OTA multi-image package (FW + FATFS).

Smart header format (56 bytes for 2 images):
  FileHdr  (8):   FwVer + HdrNum=2
  ImgHdr0 (24):   Signature="OTA" + ImgHdrLen=24 + Checksum(FW) + ImgLen
                  + Offset=56 + ImgID=1 (APP)
  ImgHdr1 (24):   Signature="OTA" + ImgHdrLen=24 + Checksum(FATFS) + ImgLen
                  + Offset=56+FW_Len + ImgID=2 (FATFS)

Usage:
  gen_ota_all.py <ota_app.bin> <fatfs.bin> <fw_ver> <ota_all.bin>
"""

import struct
import sys


SMART_FILE_HDR_LEN = 8
SMART_IMG_HDR_LEN = 24
SMART_IMGID_APP = 1
SMART_IMGID_FATFS = 2


def checksum(data):
    s = 0
    for b in data:
        s = (s + b) & 0xFFFFFFFF
    return s


def main():
    if len(sys.argv) != 5:
        print(
            f"Usage: {sys.argv[0]} <ota_app.bin> <fatfs.bin> <fw_ver> <ota_all.bin>",
            file=sys.stderr,
        )
        sys.exit(1)

    ota_app_path = sys.argv[1]
    fatfs_path = sys.argv[2]
    fw_ver = int(sys.argv[3], 0)
    out_path = sys.argv[4]

    with open(ota_app_path, "rb") as f:
        ota_app_data = f.read()

    with open(fatfs_path, "rb") as f:
        fatfs_data = f.read()

    # ota_app.bin is already Smart-wrapped: skip its 32-byte header
    app_header_size = SMART_FILE_HDR_LEN + SMART_IMG_HDR_LEN  # 32
    if len(ota_app_data) < app_header_size:
        print("ERROR: ota_app.bin too small for Smart header", file=sys.stderr)
        sys.exit(1)

    fw_payload = ota_app_data[app_header_size:]
    if len(fw_payload) == 0:
        print("ERROR: ota_app.bin has no FW payload", file=sys.stderr)
        sys.exit(1)

    if len(fatfs_data) == 0:
        print("ERROR: fatfs.bin is empty", file=sys.stderr)
        sys.exit(1)

    total_header = SMART_FILE_HDR_LEN + 2 * SMART_IMG_HDR_LEN  # 56
    fw_offset = total_header
    fatfs_offset = fw_offset + len(fw_payload)

    # FileHdr
    file_hdr = struct.pack("<II", fw_ver, 2)

    # ImgHdr[0] — FW
    img_hdr0 = struct.pack(
        "<4sIIIII",
        b"OTA\x00",
        SMART_IMG_HDR_LEN,
        checksum(fw_payload),
        len(fw_payload),
        fw_offset,
        SMART_IMGID_APP,
    )

    # ImgHdr[1] — FATFS
    img_hdr1 = struct.pack(
        "<4sIIIII",
        b"OTA\x00",
        SMART_IMG_HDR_LEN,
        checksum(fatfs_data),
        len(fatfs_data),
        fatfs_offset,
        SMART_IMGID_FATFS,
    )

    with open(out_path, "wb") as f:
        f.write(file_hdr)
        f.write(img_hdr0)
        f.write(img_hdr1)
        f.write(fw_payload)
        f.write(fatfs_data)

    print(
        f"Generated {out_path}: fw={len(fw_payload)} fatfs={len(fatfs_data)} "
        f"total={total_header + len(fw_payload) + len(fatfs_data)} "
        f"fw_ver={fw_ver}"
    )


if __name__ == "__main__":
    main()
