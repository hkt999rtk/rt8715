#!/usr/bin/env python3
"""Exercise the actual make recipe with real archives in an isolated directory.

Run after building the local ChaCha replacement archive. Customer archives and
firmware outputs are read only. No board is required.
"""
from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile


BUILD = Path(__file__).resolve().parents[3] / 'GCC-RELEASE'
PREFIX = '/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'


def fingerprint(path):
    if not path.exists():
        return None
    return hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_mtime_ns


def main():
    mk = (BUILD / 'application.is.mk').read_text()
    start = mk.index('$(CARBOX_CARPLAY_HANDOVER_ARCHIVE):')
    recipe = mk[start:mk.index('\n\napplication:', start)]
    base = BUILD / 'carplay_app/chacha_m33/build/lib_CarPlay_chacha_m33.a'
    vendor = BUILD / 'carplay_app/lib_CarPlay.a'
    accessory = BUILD / 'carplay_app/lib_Accessory2.a'
    with tempfile.TemporaryDirectory(prefix='rx-archive-publish-') as directory:
        work = Path(directory)
        output = work / 'derived.a'
        stamp = work / 'config'
        stamp.touch()
        values = {
            'CARBOX_CARPLAY_HANDOVER_ARCHIVE': output,
            'CARBOX_CARPLAY_ARCHIVE': work / 'base.a',
            'CARBOX_CARPLAY_VENDOR_ARCHIVE': work / 'vendor.a',
            'CARBOX_ACCESSORY2_VENDOR_ARCHIVE': accessory,
            'CARBOX_VIDEO_HANDOVER_PATCH': BUILD / 'carplay_app/patch_video_handover_archive.sh',
            'RX_CRYPTO_STAMP': stamp,
            'AR': PREFIX + 'ar', 'OBJCOPY': PREFIX + 'objcopy',
            'SCREEN_RX_SEPARATE_BUFFER': 1, 'AUDIO_RX_SEPARATE_BUFFER': 1,
        }
        makefile = work / 'Makefile'
        makefile.write_text(''.join(f'{key} := {value}\n' for key, value in values.items()) + recipe + '\n')

        def restore_inputs():
            shutil.copyfile(base, work / 'base.a')
            shutil.copyfile(vendor, work / 'vendor.a')

        def run(success):
            p = subprocess.run(['make', '--no-print-directory', '-f', str(makefile), str(output)],
                               cwd=BUILD, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            assert (p.returncode == 0) == success, p.stdout
            if not success:
                assert 'AirPlayReceiverSession.o changed' in p.stdout, p.stdout
            assert not list(work.glob('.rx-archive.*')), 'staging directory leaked'

        restore_inputs()
        run(True)
        old = fingerprint(output)
        run(True)
        assert fingerprint(output) == old, 'unchanged inputs rebuilt the archive'

        # Append one byte to a valid ELF: still readable by ar/objcopy, but it
        # represents an unaudited customer audio object and must be rejected.
        member = work / 'AirPlayReceiverSession.o'
        member.write_bytes(subprocess.check_output([PREFIX + 'ar', 'p', str(vendor), member.name]) + b'\0')
        for archive in ('base.a', 'vendor.a'):
            subprocess.run([PREFIX + 'ar', 'r', str(work / archive), str(member)], check=True)
        for _ in range(2):
            run(False)
            assert fingerprint(output) == old, 'audit failure changed the previous archive'
        output.unlink()
        for _ in range(2):
            run(False)
            assert not output.exists(), 'audit failure published an archive'

        restore_inputs()
        run(True)
        symbols = subprocess.check_output([PREFIX + 'nm', '-u', str(output)], text=True)
        for name in ('carbox_screen_rx_decrypt', 'carbox_audio_general_decrypt', 'carbox_audio_main_decrypt'):
            assert name in symbols, f'missing RX adapter: {name}'
    print('PASS: repeated audit failures preserve old/missing target; valid inputs publish RX adapters; staging cleaned')


if __name__ == '__main__':
    main()
