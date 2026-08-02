#!/usr/bin/env python3
"""Create an elf2bin firmware profile for a selected target/FW slot."""

import json
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) not in (6, 7):
        print(
            "usage: gen_firmware_json.py TEMPLATE TARGET AXF OUTPUT_BIN OUTPUT_JSON [recovery]",
            file=sys.stderr,
        )
        return 2

    template = Path(sys.argv[1])
    target = sys.argv[2]
    axf = Path(sys.argv[3])
    output_bin = Path(sys.argv[4])
    output_json = Path(sys.argv[5])
    recovery_profile = len(sys.argv) == 7 and sys.argv[6] == "recovery"
    if len(sys.argv) == 7 and not recovery_profile:
        print("optional profile must be 'recovery'", file=sys.stderr)
        return 2
    if target not in ("FW1", "FW2"):
        print("TARGET must be FW1 or FW2", file=sys.stderr)
        return 2

    profile = json.loads(template.read_text(encoding="utf-8"))
    profile["FIRMWARE"]["file"] = str(output_bin)
    profile["FIRMWARE"]["hash_key_src"] = target

    if recovery_profile:
        # Recovery never starts ISP or WoWLAN. FWLS belongs to the normal LP
        # application and is likewise absent. Keep both WLAN cuts because one
        # customer image must remain bootable across supported RTL8195B cuts.
        keep = {"CINIT", "XIP", "FWHS", "WLANB", "WLANC"}
        profile["FIRMWARE"]["images"] = [
            image
            for image in profile["FIRMWARE"]["images"]
            if image.get("img") in keep
        ]

    for section in profile.values():
        # Only ELF-backed sections follow the selected application AXF.
        # WLAN/ISP entries use literal binary files and must stay untouched.
        if (
            isinstance(section, dict)
            and isinstance(section.get("source"), str)
            and section["source"].endswith(".axf")
        ):
            section["source"] = str(axf)

    output_json.write_text(json.dumps(profile, indent=4) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
