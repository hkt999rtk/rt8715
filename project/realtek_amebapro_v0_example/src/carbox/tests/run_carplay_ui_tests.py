#!/usr/bin/env python3
"""Exercise local conversion and the real ARM x264 archive; requires QEMU/FFmpeg."""
import os
import hashlib
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
             CARBOX/"carplay_ui_memory.c",CARBOX/"carplay_ui_encoder.c",
             CARBOX/"carplay_ui_skip_cabac.c",work/"x264.a",
             "-Wl,-Ttext=0x10000,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup,--wrap=x264_frame_pop_unused,--wrap=x264_frame_copy_picture",
             "-lm","-lc","-lgcc","-o",work/"encoder"])
        def encode(width,height,failure=0,pattern="checker"):
            log=run(["qemu-arm","-cpu","max",work/"encoder",width,height,failure,pattern,work/"ui.h264"])
            assert "BADFREE" not in log and "GUARDFAIL" not in log,log
            assert len(re.findall(r"RESULT repeat=[01] live=0 peak=\d+ calls=\d+ guards=0",log))==2,log
            return log
        vendor_skip_hash={
            (32,32):"3245dbfc9b5e190dee15b57e69acebbd33baef24dd9f1e96aa381587fe94c274",
            (320,240):"6bcff9c2a1a9f81e9c7a0111e3b28517195f3224b389ccd6362f7371ecafdbee",
            (1920,1080):"b243f31aa893ed5695206dd2abc16e6ff31e6f5a8410246cc9d087b18b0944d9",
            (2560,1200):"0677c394718c1cf9244e57f1359e5f55727fe56c78afad4b297ba5f671b1ef69",
            (2560,1440):"a1bff9ad20f6d1a5a06c2a2ed36faac1d9149630b6b1b89914a1074f63a3eb0b",
        }
        vendor_stream_hash={
            (32,32,"checker"):"6904f47dc8045d8d3298276cf121d04602ed86b895d21b0f2db8355b1199a854",
            (320,240,"checker"):"b715d90af902fca51181561e01fc59b07a9b94be9d677c8899516f89fc3455d2",
            (1920,1080,"checker"):"ade0b2b42a3d36e835d7f34e93e7c22804d474a472ef4d846f3bff32f2e39567",
            (2560,1200,"checker"):"46a28d119f0612a813db04d592d9ebc207220d3bcb9ec606441ff5b5ab67c6b6",
            (2560,1440,"checker"):"838ae468beafe0ebf015bd42a440d127b421de3f02c99fb38d5dd17df62d9f07",
            (2560,1440,"random"):"825c08c1e856a5148f88f20b3473f9e5bcc93d549f539c8496f361d7ed1ce05d",
        }
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
            packets=(work/"ui.h264").read_bytes().split(b"\0\0\0\1")
            assert hashlib.sha256((work/"ui.h264").read_bytes()).hexdigest()==(
                vendor_stream_hash[width,height,pattern])
            assert len(packets)==24 and not packets[0]
            assert [p[0]&31 for p in packets[1:]]==[7,8,5]+[1]*20
            p_tail=b"".join(b"\0\0\0\1"+p for p in packets[4:])
            assert hashlib.sha256(p_tail).hexdigest()==vendor_skip_hash[width,height]
            if (width,height,pattern)==(2560,1440,"checker"):
                # The P NALs must match this archive's original CABAC helper,
                # not just decode in FFmpeg. Captured from vendor mode1.h264.
                assert packets[1].hex()=="27640032ac1314500a002d69b808083036860c96"
                assert packets[2].hex()=="28ee0fcb"
            print(f"PASS {width}x{height} {pattern}: 21 identical decoded frames; " +
                  re.findall(r"RESULT repeat=1 (.*)",log)[0],flush=True)
        baseline=encode(320,240)
        assert hashlib.sha256((work/"ui.h264").read_bytes()).hexdigest()==(
            vendor_stream_hash[320,240,"checker"])
        packets=(work/"ui.h264").read_bytes().split(b"\0\0\0\1")
        p_tail=b"".join(b"\0\0\0\1"+p for p in packets[4:])
        assert hashlib.sha256(p_tail).hexdigest()==vendor_skip_hash[320,240]
        calls=int(re.search(r"RESULT repeat=0 .* calls=(\d+)",baseline)[1])-2
        for failure in range(1,calls+1):
            log=encode(320,240,failure)
            assert log.count("[UI MEM] abort")==2,log
        print(f"PASS every allocation failure at 320x240: {calls} sites, each repeated twice",flush=True)

if __name__=="__main__":main()
