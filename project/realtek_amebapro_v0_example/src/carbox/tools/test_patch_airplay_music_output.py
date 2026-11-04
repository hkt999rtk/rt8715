#!/usr/bin/env python3
"""Check actual customer relocation preservation and reject changed native ABI."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import patch_airplay_music_output as patch

ROOT = Path(__file__).resolve().parents[5]
DEFAULT_PREFIX = '/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'


def elf_object(image):
    eh = struct.unpack_from('<16sHHIIIIIHHHHHH',image)
    sections = [struct.unpack_from('<10I',image,eh[6]+40*i) for i in range(eh[12])]

    def data(s):
        return image[s[4]:s[4]+s[5]]

    def string(blob,off):
        return blob[off:blob.index(0,off)].decode('ascii')

    names = data(sections[eh[13]])
    named = {string(names,s[0]):s for s in sections}
    symtab = next(s for s in sections if s[1]==2)
    strings = data(sections[symtab[6]])
    symbols = [struct.unpack_from('<IIIBBH',image,off)
               for off in range(symtab[4],symtab[4]+symtab[5],16)]
    symbol_names = [string(strings,s[0]) for s in symbols]
    relocs = {}
    for name,s in named.items():
        if s[1] != 9:
            continue
        relocs[name] = []
        for off in range(s[4],s[4]+s[5],8):
            pc,info = struct.unpack_from('<II',image,off)
            relocs[name].append((pc,info&255,symbol_names[info>>8]))
    return {n:data(s) for n,s in named.items()},relocs,dict(zip(symbol_names,symbols))


def main(prefix):
    archive = ROOT/'project/realtek_amebapro_v0_example/GCC-RELEASE/carplay_app/lib_Accessory2_resource450.a'
    vendor = subprocess.check_output([prefix+'ar','p',str(archive),patch.MEMBER])
    assert hashlib.sha256(vendor).hexdigest()==patch.VENDOR_SHA256
    original_sections,original_relocs,_ = elf_object(vendor)
    with tempfile.TemporaryDirectory(prefix='music-output-patch-test-') as d:
        tmp = Path(d)
        output = tmp/'patched.a'
        patch.main(prefix+'ar',prefix+'objcopy',archive,output)
        updated = subprocess.check_output([prefix+'ar','p',str(output),patch.MEMBER])
        sections,relocs,symbols = elf_object(updated)
        for name,code in original_sections.items():
            if name.startswith('.text.'):
                assert sections[name]==code, 'native instructions changed: '+name
        replacements = {('.rel.text.'+fn,pc):(native,hook)
                        for (fn,pc),(native,hook,_) in patch.CALLS.items()}
        changes = 0
        assert relocs.keys()==original_relocs.keys()
        for name,before in original_relocs.items():
            assert len(before)==len(relocs[name]), 'relocations were dropped'
            for left,right in zip(before,relocs[name]):
                if (name,left[0]) in replacements:
                    native,hook = replacements[(name,left[0])]
                    assert left==(right[0],10,native) and right==(left[0],10,hook)
                    assert symbols[hook][5]==0, 'hook must remain undefined for linker'
                    changes+=1
                else:
                    assert left==right, 'unrelated relocation changed'
        assert changes==5
        # A malformed or already instrumented member must fail before output write.
        member = tmp/patch.MEMBER
        bad = bytearray(vendor); bad[64]^=1
        member.write_bytes(bad)
        altered = tmp/'altered.a'
        subprocess.run([prefix+'ar','rcs',str(altered),str(member)],check=True)
        for source in (altered,output):
            sentinel=tmp/'must-remain.a'; sentinel.write_bytes(b'unchanged')
            try:
                patch.main(prefix+'ar',prefix+'objcopy',source,sentinel)
            except SystemExit:
                pass
            else:
                raise AssertionError('changed/duplicate native instrumentation accepted')
            assert sentinel.read_bytes()==b'unchanged'
        # Compile the real shims and inspect their relocation and stack contract.
        asm = ROOT/'project/realtek_amebapro_v0_example/src/carbox/audio_music_output_hooks.S'
        obj = tmp/'shims.o'
        subprocess.run([prefix+'gcc','-mcpu=cortex-m33','-mthumb',
                        '-DCONFIG_AUDIO_MUSIC_DIAG=1','-c',str(asm),'-o',str(obj)],check=True)
        sections,relocs,symbols=elf_object(obj.read_bytes())
        for kind,mov in (('init','2246'),('push','2346'),('pop','2346'),('exit','2146')):
            name='carbox_music_output_'+kind
            assert sections['.text.'+name][:6]==bytes.fromhex(mov+'fff7febf')
            assert relocs['.rel.text.'+name]==[(2,30,name+'_c')]
            assert symbols[name][2]==6
        name='carbox_music_output_sendto'
        assert sections['.text.'+name][:30]==bytes.fromhex(
            '10b584b0ddf818c0cdf800c0ddf81cc0cdf804c00294fff7feff04b010bd')
        assert relocs['.rel.text.'+name]==[(22,10,name+'_c')]
        assert symbols[name][2]==30
    print('PASS music output patch: pinned customer ABI, five redirects, all native code/other relocations preserved, rejection guards, Thumb adapters')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix',default=DEFAULT_PREFIX)
    main(parser.parse_args().prefix)
