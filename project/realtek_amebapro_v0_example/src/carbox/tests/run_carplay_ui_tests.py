#!/usr/bin/env python3
"""Exercise local conversion and the real ARM x264 archive; requires QEMU/FFmpeg."""
import os
import pathlib
import re
import subprocess
import tempfile

HERE=pathlib.Path(__file__).resolve().parent
CARBOX=HERE.parent
GCC=CARBOX.parent.parent / "GCC-RELEASE"
PREFIX=os.environ.get("CROSS_COMPILE", "/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-")

def run(args):
    p=subprocess.run([str(a) for a in args],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    if p.returncode: raise RuntimeError(f"{args}\n{p.stdout}")
    return p.stdout

def main():
    with tempfile.TemporaryDirectory(prefix="carplay-ui-test-") as tmp:
        work=pathlib.Path(tmp)
        run(["cc","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-O1","-g",
             HERE/"carplay_ui_memory_host.c",CARBOX/"carplay_ui_memory.c","-o",work/"host"])
        print(run([work/"host"]),end="",flush=True)
        run(["python3",CARBOX/"tools/patch_ui_x264_archive.py",PREFIX+"ar",PREFIX+"objcopy",
             GCC/"carplay_app/lib_x264.a",work/"x264.a"])
        run([PREFIX+"gcc","-march=armv8-m.main+dsp","-mthumb","-mfloat-abi=softfp",
             "-mfpu=fpv5-sp-d16","-O2","-g","-nostartfiles","-DCARBOX_UI_ENCODER_QEMU",
             HERE/"carplay_ui_qemu_start.S",HERE/"carplay_ui_encoder_qemu.c",
             CARBOX/"carplay_ui_memory.c",CARBOX/"carplay_ui_encoder.c",work/"x264.a",
             "-Wl,-Ttext=0x10000,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup,--wrap=x264_frame_pop_unused,--wrap=x264_frame_copy_picture",
             "-lm","-lc","-lgcc","-o",work/"encoder"])
        def encode(width,height,failure=0,pattern="checker"):
            log=run(["qemu-arm","-cpu","max",work/"encoder",width,height,failure,pattern,work/"ui.h264"])
            assert "BADFREE" not in log and "GUARDFAIL" not in log,log
            assert len(re.findall(r"RESULT repeat=[01] live=0 peak=\d+ calls=\d+ guards=0",log))==2,log
            return log
        for width,height,pattern in [(32,32,"checker"),(1920,1080,"checker"),
                (2560,1200,"checker"),(2560,1440,"checker"),(2560,1440,"random")]:
            log=encode(width,height,pattern=pattern)
            assert log.count("status=OK")==2 and "abort" not in log,log
            errors=run(["ffmpeg","-v","error","-i",work/"ui.h264","-f","framemd5","-y",work/"ui.md5"])
            assert not errors,errors
            data=(work/"ui.md5").read_text()
            frames=[s for s in data.splitlines() if not s.startswith("#")]
            assert len(frames)==21 and len({s.split(",")[-1] for s in frames})==1,data
            assert f"{width}x{height}" in data,data
            print(f"PASS {width}x{height} {pattern}: 21 identical decoded frames; " +
                  re.findall(r"RESULT repeat=1 (.*)",log)[0],flush=True)
        baseline=encode(320,240)
        calls=int(re.search(r"RESULT repeat=0 .* calls=(\d+)",baseline)[1])-2
        for failure in range(1,calls+1):
            log=encode(320,240,failure)
            assert log.count("[UI MEM] abort")==2,log
        print(f"PASS every allocation failure at 320x240: {calls} sites, each repeated twice",flush=True)

if __name__=="__main__":main()
