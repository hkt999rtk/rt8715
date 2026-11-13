#!/usr/bin/env python3
import pathlib, subprocess, tempfile
p=pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as d:
    exe=pathlib.Path(d)/'test'
    subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I'+str(p.parent),str(p/'test_source.c'),str(p.parent/'AirPlayMediaClockSource.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
