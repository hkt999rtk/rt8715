#!/usr/bin/env python3
"""Build a GCC MP-only A-MSDU repair; preserve all other vendor members.

Do not substitute normal-driver recovered RX/skbuff objects: MP adapter
offsets differ. The vendor input is immutable and the output is atomic.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from verify_amsdu_child_adapter import verify_archive

HERE = Path(__file__).resolve().parent
VENDOR_SHA256 = 'cab792c356dfece332ea395784163a369144198917e95597564e00c0630c26dd'
HELPER = 'wlan_amsdu_child_init.o'


def run(*args, **kwargs):
    return subprocess.check_output([str(arg) for arg in args], **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vendor', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--prefix', required=True)
    args = parser.parse_args()
    vendor, output = args.vendor.resolve(), args.output.resolve()
    if vendor == output:
        raise ValueError('output must not overwrite the original vendor archive')
    if hashlib.sha256(vendor.read_bytes()).hexdigest() != VENDOR_SHA256:
        raise ValueError('unknown GCC lib_wlan_mp.a; review the MP ABI before patching')
    ar = args.prefix+'ar'
    names = run(ar, 't', vendor).decode().splitlines()
    if (not names or len(names) != len(set(names)) or HELPER in names or
            any(Path(name).name != name or name in ('.', '..') for name in names)):
        raise ValueError('unsupported archive member inventory')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.mp-amsdu-', dir=output.parent) as td:
        stage = Path(td)
        run(ar, 'x', vendor, cwd=stage)
        before = {name: hashlib.sha256((stage/name).read_bytes()).digest() for name in names}
        for script in ('patch_amsdu_alloc_failure.py', 'patch_amsdu_child_adapter.py'):
            run(sys.executable, HERE/script, stage/'rtw_recv.o')
        (stage/HELPER).write_bytes(args.helper.read_bytes())
        result = stage/'fixed.a'
        run(ar, 'qcD', result, *(stage/name for name in [*names, HELPER]))
        run(ar, 'sD', result)
        verify_archive(result, args.prefix)
        if run(ar, 't', result).decode().splitlines() != [*names, HELPER]:
            raise ValueError('output member order changed')
        for name in names:
            if name != 'rtw_recv.o' and hashlib.sha256(run(ar, 'p', result, name)).digest() != before[name]:
                raise ValueError('unrelated MP member changed: '+name)
        result.replace(output)
    print('MP A-MSDU repair: native 16-frame pool retained; only rtw_recv.o changed + helper added')
    print(hashlib.sha256(output.read_bytes()).hexdigest(), output)


if __name__ == '__main__':
    main()
