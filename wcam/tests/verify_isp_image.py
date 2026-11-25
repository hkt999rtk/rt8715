#!/usr/bin/env python3
"""Check the flash artifact includes the required sensor ISP firmware."""
import pathlib
import sys

image = pathlib.Path(sys.argv[1]).read_bytes()
role = sys.argv[2]
sdk_images = pathlib.Path(__file__).resolve().parents[2] / (
    "component/soc/realtek/8195b/misc/bsp/image")
gc2053 = (sdk_images / "isp_gc2053.bin").read_bytes()
sc2232 = (sdk_images / "isp_sc2232.bin").read_bytes()

if role == "tx_gc2053":
    assert image.count(gc2053) == 1, "GC2053 ISP firmware missing or duplicated"
    assert sc2232 not in image, "SC2232 ISP firmware in GC2053 image"
    print("flash: GC2053 ISP firmware present exactly once")
else:
    assert gc2053 not in image, "GC2053 ISP firmware leaked into another role"
    print("flash: no GC2053 ISP firmware")
