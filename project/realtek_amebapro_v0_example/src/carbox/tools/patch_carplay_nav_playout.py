#!/usr/bin/env python3
"""Member-local condition redirection after optional navigation diagnostics.

Validate native allocated section bytes against a hash-pinned vendor member;
only undefined condition symbols change. No global POSIX or music modification.
"""
import hashlib, os, struct, subprocess, sys, tempfile
from pathlib import Path
from patch_carplay_nav_diag import VENDOR_SHAS

def sections(blob):
    h=struct.unpack_from('<16sHHIIIIIHHHHHH',blob)
    if h[0][:6]!=b'\x7fELF\x01\x01' or h[1:3]!=(1,40):raise ValueError('not ARM ELF')
    ss=[struct.unpack_from('<10I',blob,h[6]+40*i) for i in range(h[12])]
    strings=blob[ss[h[13]][4]:ss[h[13]][4]+ss[h[13]][5]]
    return {strings[s[0]:].split(b'\0')[0].decode():s for s in ss}

def validate(vendor,source):
    if hashlib.sha256(vendor).hexdigest() not in VENDOR_SHAS:raise ValueError('unknown vendor AirPlayUtils.o')
    a,b=sections(vendor),sections(source)
    for name,s in a.items():
        if not s[2]&2:continue
        t=b.get(name)
        if t is None or s[1:4]!=t[1:4] or s[5]!=t[5]:raise ValueError('native layout changed: '+name)
        if s[1]!=8 and vendor[s[4]:s[4]+s[5]]!=source[t[4]:t[4]+t[5]]:
            raise ValueError('native code/data changed: '+name)

def main(ar,objcopy,vendor_archive,source,out):
    converter=subprocess.check_output([ar,'p',vendor_archive,'AudioConverter_FDKAAC_OPUS.o'])
    if hashlib.sha256(converter).hexdigest()!='6709116ad5abc08e713bafabd447949f933ec5eee98095efa2c7283705b20181':
        raise ValueError('AudioConverter layout changed; re-audit decoder handle/format before use')
    native=subprocess.check_output([ar,'p',vendor_archive,'AirPlayUtils.o'])
    obj=subprocess.check_output([ar,'p',source,'AirPlayUtils.o'])
    validate(native,obj)
    out=Path(out);out.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out.parent,prefix='.nav-playout-') as td:
        td=Path(td);a=td/'archive.a';a.write_bytes(Path(source).read_bytes())
        p=td/'AirPlayUtils.o';p.write_bytes(obj)
        subprocess.run([objcopy,'--redefine-sym','pthread_cond_wait=carbox_nav_condition_wait',
            '--redefine-sym','pthread_cond_signal=carbox_nav_condition_signal',
            '--redefine-sym','pthread_mutex_destroy=carbox_nav_condition_destroy_mutex',str(p)],check=True)
        subprocess.run([ar,'r',str(a),str(p)],check=True,stdout=subprocess.DEVNULL)
        subprocess.run([ar,'s',str(a)],check=True);os.replace(a,out)
    print('Navigation playout: member-local retained condition wake; native code/data unchanged')
if __name__=='__main__':
    if len(sys.argv)!=6:raise SystemExit('AR OBJCOPY VENDOR INPUT OUTPUT')
    main(*sys.argv[1:])
