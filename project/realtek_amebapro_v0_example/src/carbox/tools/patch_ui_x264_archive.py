#!/usr/bin/env python3
"""Interpose allocation inside ABI76 objects without modifying vendor archives."""
import hashlib
import pathlib
import shutil
import subprocess
import sys
import tempfile
from patch_carplay_ui565_archive import X264_ABI76_SHA256

def main():
    ar, objcopy, source, output = [pathlib.Path(s).resolve() for s in sys.argv[1:]]
    if hashlib.sha256(source.read_bytes()).hexdigest() != X264_ABI76_SHA256:
        raise SystemExit("UI x264 patch: unknown archive; re-audit all ABI offsets")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix="ui-x264-") as tmp:
        work=pathlib.Path(tmp)
        archive=work / "derived.a"
        shutil.copyfile(source,archive)
        subprocess.run([ar,"x",source,"common.o","encoder.o","x264.o"],cwd=work,check=True)
        for name in ("common.o","encoder.o","x264.o"):
            args=[str(objcopy),"--redefine-sym=malloc=carbox_ui_core_malloc",
                  "--redefine-sym=free=carbox_ui_core_free",
                  "--redefine-sym=strdup=carbox_ui_core_strdup"]
            if name=="common.o":
                args += ["--weaken-symbol=x264_malloc","--weaken-symbol=x264_free"]
            subprocess.run(args+[str(work/name)],check=True)
            subprocess.run([ar,"r",archive,work/name],check=True)
        subprocess.run([ar,"s",archive],check=True)
        archive.replace(output)

if __name__=="__main__": main()
