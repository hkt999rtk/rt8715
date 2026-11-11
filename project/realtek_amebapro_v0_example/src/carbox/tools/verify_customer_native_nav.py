#!/usr/bin/env python3
"""校验 v18 原生导航与发送策略对象，拒绝未知版本。"""
import hashlib
from pathlib import Path
import subprocess
import sys

EXPECTED = {
    ('lib_CarPlay.a', 'AirPlayUtils.o'):
        # 2026-11-05 补交版：仅 Free 函数及其重定位变化，先停 decoder 再释放 logQueue。
        '5cb264b2c791e789325a25a102f8ae846ee2b6784808e0cdbe6d47697d897b14',
    ('lib_CarPlay.a', 'AudioConverter_FDKAAC_OPUS.o'):
        # v15：plcReady、格式/容量检查、Reset 清理；需要 GetStreamInfo 路由。
        'ddf653c580815e6e767caa49f27a0b360a0a8d3707acccaf1ee1d42d54cf07fb',
    ('lib_Accessory2.a', 'AirPlayAudio.o'):
        'acfb4ed06564cc3443dd7453101eb59a2550ebd9cbb45644a09a75207b67d43e',
    ('lib_Accessory2.a', 'AirPlayScreen.o'):
        'e01a7d141a9b68443a175160041465b330e56eab0417d8e6c0846559413c2399',
    ('lib_Accessory2.a', 'AirPlayTimer.o'):
        'efb9d89938b8a6856b38cdfde418381c74b0dc164939ee6308efce6224976f64',
    ('lib_Accessory2.a', 'AirPlayMediaClock.o'):
        '4f6645a25c9717c507e2704d93b039a112b21e617b10c5177a6f0fff56391e38',
}

def main(ar, directory):
    for (archive, member), expected in EXPECTED.items():
        blob = subprocess.check_output([ar, "p", str(Path(directory) / archive), member])
        if hashlib.sha256(blob).hexdigest() != expected:
            raise SystemExit("Customer native navigation: re-audit required: " + member)
    print("Customer v18 native navigation / pacing / priority / 4KB output / native media clock: verified")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: verify_customer_native_nav.py AR LIB_DIR")
    main(*sys.argv[1:])
