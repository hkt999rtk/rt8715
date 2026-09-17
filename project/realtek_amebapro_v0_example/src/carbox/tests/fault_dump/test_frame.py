#!/usr/bin/env python3
"""Exercise the production frame validator with bounds/FP/fault cases."""
import pathlib
import subprocess
import tempfile

here = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="fault-frame-") as tmp:
    binary = str(pathlib.Path(tmp) / "test_frame")
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
        "-fsanitize=undefined,address", "-fno-omit-frame-pointer",
        "-I", str(here.parent.parent), str(here / "test_frame.c"),
        "-o", binary,
    ], check=True)
    subprocess.run([binary], check=True)
