#!/usr/bin/env python3
"""Fail the build when a signed firmware image exceeds its flash slot."""

import re
import sys
from pathlib import Path


def read_hex_macro(header: Path, name: str) -> int:
    pattern = re.compile(
        rf"^\s*#define\s+{re.escape(name)}\s+(0x[0-9A-Fa-f]+)\s*$"
    )
    for line in header.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            return int(match.group(1), 16)
    raise ValueError(f"{name} not found in {header}")


def main() -> int:
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} HEADER SIZE_MACRO IMAGE", file=sys.stderr)
        return 2

    header = Path(sys.argv[1])
    macro = sys.argv[2]
    image = Path(sys.argv[3])
    try:
        capacity = read_hex_macro(header, macro)
        size = image.stat().st_size
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    free = capacity - size
    print(
        f"Firmware size check: {image}={size} bytes, "
        f"{macro}={capacity} bytes, free={free} bytes"
    )
    if free < 0:
        print(
            f"ERROR: {image} exceeds {macro} by {-free} bytes; "
            "refusing to create an overlapping flash image",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
