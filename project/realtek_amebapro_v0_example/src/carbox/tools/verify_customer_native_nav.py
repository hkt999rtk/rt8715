#!/usr/bin/env python3
"""校验 v19 原生导航与发送策略对象，拒绝未知版本。"""
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
        '89e071d9f76e7966ae2d1d81d101ac1457db4f8c56f3d28bbfbbd396f50809d3',
    ('lib_Accessory2.a', 'AirPlayScreen.o'):
        '1bd7c24a72bfa5e10c889f3f2b0c9fcb2b5940eddbfa452b6baf6efe125ab74c',
    ('lib_Accessory2.a', 'AirPlayTimer.o'):
        'fd7b70bda6e39ccec05e49f5b0a5e37149fec8c391b18ff24d7f4f9f6b4e0055',
    ('lib_Accessory2.a', 'AirPlayClock.o'):
        '4d6c19fb946bc622f1dd439b0cbf64a5d53001f1453b494b40e7eddf3ba149f4',
    ('lib_Accessory2.a', 'AudioFeedback.o'):
        'c94cf314596a87bb91277c8509352c499d76c9ed1e13da5c1bbafe0a4ea1300b',
    ('lib_Accessory2.a', 'AudioSamplePacer.o'):
        'ebadad599149f36953555e8b3fcdf836f1a1707d813d59a5425c122d24700c0b',
}

def main(ar, directory):
    for (archive, member), expected in EXPECTED.items():
        blob = subprocess.check_output([ar, "p", str(Path(directory) / archive), member])
        if hashlib.sha256(blob).hexdigest() != expected:
            raise SystemExit("Customer native navigation: re-audit required: " + member)
    print("Customer v19 native navigation / pacing / priority / 4KB output / native HAL clock / continuous feedback / sample pacing: verified")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: verify_customer_native_nav.py AR LIB_DIR")
    main(*sys.argv[1:])
