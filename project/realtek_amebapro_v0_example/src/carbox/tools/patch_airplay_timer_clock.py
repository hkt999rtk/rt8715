#!/usr/bin/env python3
"""按已审查 ABI 区分 baseline/采时入口；可选只读 config 观测。保留指令。"""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from patch_screen_session import ELF

MEMBER = 'AirPlayTimer.o'
SHA256 = '801285174fd4eb96a39b9df79d93fe5755aa5de0b432c5dcbcb882dbc87bd7f4'
CONFIG_SHA256 = '60fd54487b765e74f24cdf9bfffe63afbfb82efd0b706a87205d0bad617f7160'


def observe_config(ar, objcopy, source, archive, tmp):
    member = 'AirPlayScreen.o'
    members = subprocess.check_output([ar, 't', str(source)]).decode().splitlines()
    if members.count(member) != 1:
        raise SystemExit('timer clock: missing/duplicate screen object')
    p = tmp / member
    p.write_bytes(subprocess.check_output([ar, 'p', str(source), member]))
    before = ELF(p.read_bytes())
    fn = 'AirPlayScreen_SendScreenConfigure'
    if hashlib.sha256(before.code(fn)).hexdigest() != CONFIG_SHA256:
        raise SystemExit('timer clock: unknown Configure ABI')
    hook = 'carbox_airplay_clock_config_send'
    if hook in before.symbols:
        raise SystemExit('timer clock: config already observed')
    subprocess.run([objcopy, '--add-symbol='+hook+'=0,global', str(p)], check=True)
    after = ELF(p.read_bytes())
    after.undefined(hook)
    after.redirect('AirPlayScreen_SendScreenData', hook, fn, 0x102, 10)
    for name, s in before.named.items():
        if name.startswith(('.text', '.rodata', '.data')):
            t = after.named[name]
            if before.b[s[4]:s[4]+s[5]] != after.b[t[4]:t[4]+t[5]]:
                raise SystemExit('timer clock: config instructions/data changed')
    p.write_bytes(after.b)
    subprocess.run([ar, 'r', str(archive), str(p)], check=True)


def main(ar, objcopy, source, output, observe='0'):
    source, output = Path(source), Path(output)
    if source.resolve() == output.resolve():
        raise SystemExit('timer clock: input and output must differ')
    if observe not in ('0', '1'):
        raise SystemExit('timer clock: observe must be 0 or 1')
    members = subprocess.check_output([ar, 't', str(source)]).decode().splitlines()
    if members.count(MEMBER) != 1:
        raise SystemExit('timer clock: missing or duplicate AirPlayTimer.o')
    data = subprocess.check_output([ar, 'p', str(source), MEMBER])
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise SystemExit('timer clock: unknown/already patched AirPlayTimer.o; review required')
    replacements = {'gettimeofday': 'carbox_airplay_timer_gettimeofday'}
    if observe == '1':
        replacements['lwip_sendto'] = 'carbox_airplay_timer_sendto'
    objdump = objcopy.removesuffix('objcopy') + 'objdump'
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix='.timer-clock-') as tmp:
        tmp = Path(tmp)
        obj = tmp / MEMBER
        obj.write_bytes(data)
        before = subprocess.check_output([objdump, '-r', str(obj)]).decode()
        native = ELF(data)
        if len(re.findall(r'\bgettimeofday\b', before)) != 3:
            raise SystemExit('timer clock: expected start + receive + transmit time calls')
        if len(re.findall(r'\blwip_sendto\b', before)) != 1:
            raise SystemExit('timer clock: unexpected timing send path')
        hook = 'carbox_airplay_timer_start_time'
        subprocess.run([objcopy, *['--redefine-sym='+a+'='+b for a,b in replacements.items()],
                        '--add-symbol='+hook+'=0,global', str(obj)], check=True)
        elf = ELF(obj.read_bytes())
        elf.undefined(hook)
        elf.redirect('carbox_airplay_timer_gettimeofday', hook, 'AirPlayTimer_Start', 6, 10)
        obj.write_bytes(elf.b)
        expected = before
        for a,b in replacements.items():
            expected = re.sub(r'\b'+a+r'\b', b, expected)
        start = 'RELOCATION RECORDS FOR [.text.AirPlayTimer_Start]:'
        head, separator, tail = expected.partition(start)
        if not separator:
            raise SystemExit('timer clock: Start relocation section missing')
        expected = head + separator + tail.replace('carbox_airplay_timer_gettimeofday', hook, 1)
        if subprocess.check_output([objdump, '-r', str(obj)]).decode() != expected:
            raise SystemExit('timer clock: unexpected relocation change')
        # 新增导入符号可能改变 objdump 的标签选择；直接比较原始 section。
        for name, s in native.named.items():
            if name.startswith(('.text', '.rodata', '.data')):
                t = elf.named[name]
                if native.b[s[4]:s[4]+s[5]] != elf.b[t[4]:t[4]+t[5]]:
                    raise SystemExit('timer clock: native instructions/data changed')
        archive = tmp/'archive.a'
        archive.write_bytes(source.read_bytes())
        subprocess.run([ar, 'r', str(archive), str(obj)], check=True)
        if observe == '1':
            observe_config(ar, objcopy, source, archive, tmp)
        subprocess.run([ar, 's', str(archive)], check=True)
        os.replace(archive, output)
    print('timer clock: isolated start baseline + monotonic samples; observe='+observe)


if __name__ == '__main__':
    main(*sys.argv[1:])
