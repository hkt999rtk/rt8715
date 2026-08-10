#!/usr/bin/env python3
"""Remove disabled image payloads from an elf2bin firmware profile."""

import json
import sys


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <profile.json> <image> [image ...]", file=sys.stderr)
        return 1

    path = sys.argv[1]
    excluded = set(sys.argv[2:])

    with open(path) as f:
        profile = json.load(f)

    images = profile["FIRMWARE"]["images"]
    profile["FIRMWARE"]["images"] = [
        image for image in images if image.get("img") not in excluded
    ]

    with open(path, "w") as f:
        json.dump(profile, f, indent="\t")
        f.write("\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
