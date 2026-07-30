#!/usr/bin/env python3
"""Generate partition.json with flash layout values from carbox_flash_layout.h.

Reads the C header for the FatFS and LittleFS base/size macros and patches them
into partition.json so the single source of truth stays in the header.
"""

import json
import re
import sys


def extract_hex_macros(h_path):
    """Return {NAME: '0x...'} for every CARBOX_* #define with a hex value."""
    macros = {}
    with open(h_path) as f:
        for line in f:
            m = re.match(r"#define\s+(CARBOX_\w+)\s+(0x[0-9A-Fa-f]+)", line)
            if m:
                macros[m.group(1)] = m.group(2)
    return macros


def normalize_hex(hex_str):
    """Strip leading zeros after 0x; '0x00440000' -> '0x440000'."""
    body = hex_str[2:].lstrip("0")
    return "0x" + (body or "0")


def main():
    if len(sys.argv) not in (3, 4):
        print(
            f"Usage: {sys.argv[0]} <carbox_flash_layout.h> <partition.json> [output.json]",
            file=sys.stderr,
        )
        sys.exit(1)

    h_path = sys.argv[1]
    json_path = sys.argv[2]
    out_path = sys.argv[3] if len(sys.argv) == 4 else json_path

    macros = extract_hex_macros(h_path)

    required = (
        "CARBOX_FATFS_BASE",
        "CARBOX_FATFS_SIZE",
        "CARBOX_LITTLEFS_BASE",
        "CARBOX_LITTLEFS_SIZE",
    )
    for k in required:
        if k not in macros:
            print(f"ERROR: macro {k} not found in {h_path}", file=sys.stderr)
            sys.exit(1)

    with open(json_path) as f:
        data = json.load(f)

    partab = data.get("partab", {})

    fatfs = partab.get("fatfs")
    if fatfs is not None:
        fatfs["start_addr"] = normalize_hex(macros["CARBOX_FATFS_BASE"])
        fatfs["length"] = normalize_hex(macros["CARBOX_FATFS_SIZE"])

    littlefs = partab.get("littlefs")
    if littlefs is not None:
        littlefs["start_addr"] = normalize_hex(macros["CARBOX_LITTLEFS_BASE"])
        littlefs["length"] = normalize_hex(macros["CARBOX_LITTLEFS_SIZE"])

    with open(out_path, "w") as f:
        json.dump(data, f, indent=4)
        f.write("\n")


if __name__ == "__main__":
    main()
