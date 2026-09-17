#!/usr/bin/env python3
"""Strict object-local RX adapter bindings. Changes symbols/relocations only.
Input customer archives are never written; publish derived archive atomically.
"""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

GUARDS = {
    'AirPlayReceiverSessionScreen.o': '9b051d06928cfb1c7e861ebfca52fd4e902cf0542353c64430c88cf6db1d2126',
    'AirPlayReceiverSession.o': 'e2284f7e576cd9ebe053ce25c40a07b6933a6a6ac339caabe06705148fe1aef6',
    'ScreenUtilsStub.o': '64dd1ef290c6bfc9024611adacf01cd3d4e72389dbc1de62afe1f567f22e7a67',
    'AppleCarPlay_AppStub.o': 'beab94a28bcda7b8ad9303086cc38e8c3d74f48fe1b1d23ba5b8d2c27ac00650',
    'AirPlayScreen.o': '8c1a6d380cd90ab4630ad1c77d232e92eafdd2e0a4b3db0c46de472f3fecd400',
}
SCREEN = [
    ('AirPlayReceiverSessionScreen_ProcessFrames', 0x20e, 'chacha20_poly1305_decrypt', 'carbox_screen_rx_decrypt'),
    ('AirPlayReceiverSessionScreen_ProcessFrames', 0x220, 'chacha20_poly1305_verify', 'carbox_screen_rx_verify'),
    ('AirPlayReceiverSessionScreen_ProcessFrames', 0x436, 'ScreenStreamProcessData', 'carbox_screen_rx_process'),
]
AUDIO = [
    ('_GeneralAudioDecryptPacket', 0x50, 'chacha20_poly1305_verify', 'carbox_audio_general_verify'),
    ('_GeneralAudioDecryptPacket', 0x40, 'chacha20_poly1305_decrypt', 'carbox_audio_general_decrypt'),
    ('_MainAltAudioThread', 0x128, 'RTPJitterBufferGetFreeNode', 'carbox_audio_get_node'),
    ('_MainAltAudioThread', 0x148, 'SocketRecvFrom', 'carbox_audio_recv'),
    ('_MainAltAudioThread', 0x160, 'RTPJitterBufferPutFreeNode', 'carbox_audio_put_free'),
    ('_MainAltAudioThread', 0x210, 'RTPJitterBufferPutBusyNode', 'carbox_audio_put_busy'),
    ('_MainAltAudioThread', 0x27a, 'chacha20_poly1305_decrypt', 'carbox_audio_main_decrypt'),
    ('_MainAltAudioThread', 0x294, 'chacha20_poly1305_verify', 'carbox_audio_main_verify'),
]

def fail(s):
    raise SystemExit('RX separate-buffer audit: ' + s)


def patch(path, routes):
    b = bytearray(path.read_bytes())
    h = struct.unpack_from('<16sHHIIIIIHHHHHH', b)
    if h[0][:6] != b'\x7fELF\x01\x01' or h[1:3] != (1, 40) or h[11] != 40:
        fail('expected ARM ELF32 relocatable')
    sec = [struct.unpack_from('<10I', b, h[6] + i*40) for i in range(h[12])]
    def cstr(table, off):
        return table[off:table.index(0, off)].decode('ascii')
    def data(s):
        return b[s[4]:s[4]+s[5]]
    names = data(sec[h[13]])
    sections = {cstr(names,s[0]): (i,s) for i,s in enumerate(sec)}
    tabs = [(i,s) for i,s in enumerate(sec) if s[1] == 2]
    if len(tabs) != 1: fail('symbol table count')
    tabidx, tab = tabs[0]
    strings = data(sec[tab[6]])
    symbols = {}
    for i in range(tab[5] // 16):
        off = tab[4] + i*16
        st = struct.unpack_from('<IIIBBH', b, off)
        name = cstr(strings,st[0])
        if name: symbols[name] = (i,off,st)
    if routes == SCREEN:
        release = symbols.get('carbox_screen_rx_free')
        if not release or release[2][5] != 0:
            fail('screen producer-free binding missing; run handover patch first')
    for function, offset, old, new in routes:
        si,s = sections['.text.'+function]
        _,rel = sections['.rel.text.'+function]
        if rel[1] != 9 or rel[6:8] != (tabidx,si): fail('relocation section')
        oi,_,ost = symbols[old]
        ni,no,nst = symbols[new]
        if ost[5] != 0 or nst[1] != 0 or nst[5] not in (0,0xfff1): fail('target symbol')
        matches = []
        for off in range(rel[4],rel[4]+rel[5],8):
            ro,ri = struct.unpack_from('<II',b,off)
            if ro == offset: matches.append((off,ri))
        if len(matches) != 1 or matches[0][1] != (oi << 8) | 10:
            fail(f'{function}+{offset:x} no longer calls {old}')
        code = bytes(data(s))
        struct.pack_into('<H',b,no+14,0)
        struct.pack_into('<I',b,matches[0][0]+4,(ni<<8)|10)
        assert bytes(data(s)) == code, 'instruction bytes changed'
    path.write_bytes(b)


def main(ar, objcopy, vendor, accessory, derived, screen, audio):
    screen, audio = int(screen), int(audio)
    if screen not in (0,1) or audio not in (0,1): fail('flags must be 0/1')
    if not (screen or audio): return
    required = (list(GUARDS) if screen else ['AirPlayReceiverSession.o'])
    if not audio and 'AirPlayReceiverSession.o' in required:
        required.remove('AirPlayReceiverSession.o')
    for member in required:
        archive = accessory if member == 'AirPlayScreen.o' else vendor
        obj = subprocess.check_output([ar,'p',archive,member])
        if hashlib.sha256(obj).hexdigest() != GUARDS[member]:
            fail(member + ' changed; re-audit ABI before enabling')
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        output = directory/'derived.a'
        output.write_bytes(Path(derived).read_bytes())
        for enabled, member, routes in [(screen,'AirPlayReceiverSessionScreen.o',SCREEN),
                                         (audio,'AirPlayReceiverSession.o',AUDIO)]:
            if not enabled: continue
            path = directory/member
            path.write_bytes(subprocess.check_output([ar,'p',str(output),member]))
            cmd = [objcopy]
            for name in sorted({r[3] for r in routes}):
                cmd += ['--add-symbol',name+'=0,global']
            if screen and member == 'AirPlayReceiverSessionScreen.o':
                cmd += ['--redefine-sym','carbox_video_handover_producer_free=carbox_screen_rx_free']
            subprocess.run(cmd+[str(path)],check=True)
            patch(path,routes)
            subprocess.run([ar,'r',str(output),str(path)],check=True)
        subprocess.run([ar,'s',str(output)],check=True)
        # Same-filesystem replace, so build interruption cannot publish half a file.
        staged = Path(derived + '.rx-tmp')
        staged.write_bytes(output.read_bytes())
        os.replace(staged,derived)
    print(f'RX separate-buffer audit: screen={screen} audio={audio}; scoped relocations verified')

if __name__ == '__main__':
    if len(sys.argv) != 8: fail('AR OBJCOPY VENDOR ACCESSORY DERIVED SCREEN AUDIO')
    main(*sys.argv[1:])
