#!/usr/bin/env python3
"""Generate partition.json with flash layout values from carbox_flash_layout.h.

Reads the C header for the firmware, FatFS and LittleFS base/size macros and
patches them into partition.json so the single source of truth stays in the
header.
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
        "CARBOX_RECOVERY_FW_BASE",
        "CARBOX_RECOVERY_FW_SIZE",
        "CARBOX_MAIN_FW_BASE",
        "CARBOX_MAIN_FW_SIZE",
        "CARBOX_FIRMWARE_REGION_END",
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

    ptable = partab.get("ptable")
    if ptable is None:
        print("ERROR: partab.ptable missing from partition template", file=sys.stderr)
        sys.exit(1)

    items = ptable.get("items")
    if not isinstance(items, list):
        print("ERROR: partab.ptable.items must be a list", file=sys.stderr)
        sys.exit(1)

    # FW1 is an immutable recovery image. FW2 is the only field-upgrade target.
    # Keep their order deterministic because the ROM exports record indexes.
    non_fw_items = [item for item in items if item not in ("fw1", "fw2")]
    boot_pos = non_fw_items.index("boot") + 1 if "boot" in non_fw_items else 0
    non_fw_items[boot_pos:boot_pos] = ["fw1", "fw2"]
    ptable["items"] = non_fw_items

    hash_key = "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E5F"
    fw1 = partab.setdefault("fw1", {})
    fw1.update(
        {
            "start_addr": normalize_hex(macros["CARBOX_RECOVERY_FW_BASE"]),
            "length": normalize_hex(macros["CARBOX_RECOVERY_FW_SIZE"]),
            "type": "FW1",
            "dbg_skip": False,
            "hash_key": fw1.get("hash_key", hash_key),
        }
    )

    fw2 = partab.setdefault("fw2", {})
    fw2.update(
        {
            "start_addr": normalize_hex(macros["CARBOX_MAIN_FW_BASE"]),
            "length": normalize_hex(macros["CARBOX_MAIN_FW_SIZE"]),
            "type": "FW2",
            "dbg_skip": False,
            "hash_key": fw2.get("hash_key", fw1["hash_key"]),
        }
    )

    recovery_end = int(macros["CARBOX_RECOVERY_FW_BASE"], 16) + int(
        macros["CARBOX_RECOVERY_FW_SIZE"], 16
    )
    main_end = int(macros["CARBOX_MAIN_FW_BASE"], 16) + int(
        macros["CARBOX_MAIN_FW_SIZE"], 16
    )
    firmware_end = int(macros["CARBOX_FIRMWARE_REGION_END"], 16)
    if recovery_end != int(macros["CARBOX_MAIN_FW_BASE"], 16):
        print("ERROR: recovery and main firmware partitions are not contiguous", file=sys.stderr)
        sys.exit(1)
    if main_end != firmware_end:
        print("ERROR: main firmware does not end at CARBOX_FIRMWARE_REGION_END", file=sys.stderr)
        sys.exit(1)
    if int(macros["CARBOX_FATFS_BASE"], 16) != firmware_end:
        print("ERROR: FATFS must immediately follow the firmware region", file=sys.stderr)
        sys.exit(1)
    for name in ("CARBOX_RECOVERY_FW_BASE", "CARBOX_MAIN_FW_BASE"):
        if int(macros[name], 16) % 0x40000:
            print(f"ERROR: {name} must be 256 KiB aligned", file=sys.stderr)
            sys.exit(1)

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
