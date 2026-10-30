#!/usr/bin/env python3
"""Execute vendor Thumb code, not a C model of its ownership policy.

Host-only dependencies: unicorn, pyelftools (may be installed with pip --target).
Unicorn API reference: https://www.unicorn-engine.org/docs/tutorial.html
Locks, skb allocation, libc and netif delivery are deterministic stubs. Actual
amsdu_to_msdu, rtw_alloc_recvframe, _rtw_alloc_recvframe, recvframe_pull,
rtw_recv_indicatepkt and rtw_free_recvframe execute from the linked objects.
"""
import argparse
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R8,
                               UC_ARM_REG_R11, UC_ARM_REG_LR, UC_ARM_REG_SP,
                               UC_ARM_REG_PC)

HERE = Path(__file__).resolve().parent
Q, COUNT, BUDDY, TYPE = 0xc44, 0xc70, 0x3378, 0x338a
DATA, STACK, STUB = 0x4000000, 0x5000000, 0x300000
STOP = STUB + 0xfff0


def run(*args):
    return subprocess.check_output([str(a) for a in args], stderr=subprocess.STDOUT)


def link_fixture(directory, prefix, vendor, fixed):
    directory.mkdir()
    objects = []
    for name in ('rtw_recv.o', 'freertos_recv.o'):
        p = directory / name
        p.write_bytes(run(prefix+'ar', 'p', vendor, name))
        objects.append(p)
    for script in ('patch_amsdu_alloc_failure.py', 'patch_recvframe_pool.py'):
        run(sys.executable, HERE/script, objects[0])
    if fixed:
        run(sys.executable, HERE/'patch_amsdu_child_adapter.py', objects[0])
        helper = directory/'wlan_amsdu_child_init.o'
        run(prefix+'gcc', '-march=armv8-m.main+dsp', '-mthumb', '-c',
            HERE/'wlan_amsdu_child_init.S', '-o', helper)
        objects.append(helper)
    defined, undefined = set(), set()
    for obj in objects:
        for line in run(prefix+'nm', '-g', '-P', obj).decode().splitlines():
            fields = line.split()
            if len(fields) > 1:
                (undefined if fields[1] == 'U' else defined).add(fields[0])
    stubs = {name: STUB + 16*i for i, name in enumerate(sorted(undefined-defined))}
    assert max(stubs.values()) < STOP
    script = directory/'fixture.ld'
    script.write_text('ENTRY(amsdu_to_msdu)\nSECTIONS { . = 0x100000; '
                      '.text : { *(.text*) *(.wlan.text*) } '
                      '.rodata : { *(.rodata*) *(.wlan.rodata*) } '
                      '.data : { *(.data*) *(.wlan.data*) } '
                      '.bss : { *(.bss*) *(.wlan.bss*) *(COMMON) } }\n' +
                      '\n'.join(f'{name} = {addr|1};' for name, addr in stubs.items()))
    elf = directory/'fixture.elf'
    run(prefix+'ld', '--gc-sections', '-T', script, '-o', elf, *objects)
    return elf.read_bytes(), stubs


class Machine:
    def __init__(self, fixture, swapped=False, mode='ok'):
        binary, self.stubs = fixture
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        for address, size in ((0x100000, 0x100000), (STUB, 0x10000),
                              (DATA, 0x400000), (STACK, 0x10000)):
            self.uc.mem_map(address, size)
        elf = ELFFile(io.BytesIO(binary))
        for section in elf.iter_sections():
            if section['sh_flags'] & 2 and section['sh_size']:
                self.uc.mem_write(section['sh_addr'], section.data())
        self.symbols = {s.name: s['st_value'] & ~1
                        for s in elf.get_section_by_name('.symtab').iter_symbols()}
        self.stub_names = {v: k for k, v in self.stubs.items()}
        for address in self.stub_names:
            self.uc.mem_write(address, b'\x70\x47')
        self.uc.mem_write(STOP, b'\x00\xbe')
        self.a, self.b = (DATA+0x10000, DATA) if swapped else (DATA, DATA+0x10000)
        self.mode = mode
        self.live_skb, self.indicated, self.freed = {}, [], []
        self.skb_calls = self.alloc_calls = self.child_inits = 0
        self.native_free_calls = []
        self.frames = {}
        self.fail_alloc_at = 0
        self.fail_skb_at = 0
        self.uc.hook_add(UC_HOOK_CODE, self.hook, begin=STUB, end=STOP)
        self.uc.hook_add(UC_HOOK_CODE, self.observe_free,
                         begin=self.symbols['rtw_free_recvframe'],
                         end=self.symbols['rtw_free_recvframe'])
        self.uc.hook_add(UC_HOOK_CODE, self.observe_alloc,
                         begin=self.symbols['rtw_alloc_recvframe'],
                         end=self.symbols['rtw_alloc_recvframe'])
        self.uc.hook_add(UC_HOOK_CODE, self.observe_child,
                         begin=self.symbols['amsdu_to_msdu']+0x76,
                         end=self.symbols['amsdu_to_msdu']+0x76)
        for i, adapter in enumerate((self.a, self.b)):
            self.write(adapter+TYPE, bytes([i]))
            self.w32(adapter+BUDDY, self.b if i == 0 else self.a)
            self.w32(adapter+0x3350, adapter)  # opaque netif cookie
            self.init_list(adapter+Q)
            self.frames[adapter] = {DATA+0x20000+i*0x2000+n*192 for n in range(32)}
            for frame in sorted(self.frames[adapter]):
                self.w32(frame+16, adapter)
                self.init_list(frame)
                self.insert(frame, adapter+Q)
            self.w32(adapter+COUNT, 32)

    def read(self, addr, size):
        return bytes(self.uc.mem_read(addr, size))

    def write(self, addr, data):
        self.uc.mem_write(addr, data)

    def u32(self, addr):
        return struct.unpack('<I', self.read(addr, 4))[0]

    def w32(self, addr, value):
        self.write(addr, struct.pack('<I', value & 0xffffffff))

    def reg(self, reg):
        return self.uc.reg_read(reg)

    def init_list(self, ptr):
        self.w32(ptr, ptr)
        self.w32(ptr+4, ptr)

    def delete(self, ptr):
        nxt, prev = self.u32(ptr), self.u32(ptr+4)
        self.w32(prev, nxt)
        self.w32(nxt+4, prev)
        self.init_list(ptr)

    def insert(self, ptr, head):
        prev = self.u32(head+4)
        self.w32(ptr, head)
        self.w32(ptr+4, prev)
        self.w32(prev, ptr)
        self.w32(head+4, ptr)

    def queue(self, adapter):
        head = adapter+Q
        ptr, prev, result = self.u32(head), head, []
        while ptr != head:
            assert ptr not in result and len(result) < 65, 'list cycle/corruption'
            assert self.u32(ptr+4) == prev, 'broken back link'
            result.append(ptr)
            prev, ptr = ptr, self.u32(ptr)
        assert self.u32(head+4) == prev
        return set(result)

    def skb(self, size):
        self.skb_calls += 1
        if self.skb_calls == self.fail_skb_at:
            return 0
        ptr = next(DATA+0x100000+i*0x2000 for i in range(8)
                   if DATA+0x100000+i*0x2000 not in self.live_skb)
        self.live_skb[ptr] = size
        self.write(ptr, bytes(64))
        self.w32(ptr+16, ptr+64)
        return ptr

    def release_skb(self, ptr):
        assert ptr in self.live_skb, f'skb double/invalid free {ptr:#x}'
        del self.live_skb[ptr]
        self.freed.append(ptr)

    def observe_free(self, uc, addr, size, _):
        self.native_free_calls.append(self.reg(UC_ARM_REG_R0))

    def observe_alloc(self, uc, addr, size, _):
        self.alloc_calls += 1
        if self.alloc_calls == self.fail_alloc_at:
            uc.reg_write(UC_ARM_REG_R0, 0)
            uc.reg_write(UC_ARM_REG_PC, self.reg(UC_ARM_REG_LR))

    def observe_child(self, uc, addr, size, _):
        self.child_inits += 1

    def hook(self, uc, addr, size, _):
        if addr == STOP:
            uc.emu_stop()
            return
        name = self.stub_names.get(addr)
        if name is None:
            raise AssertionError(f'jump into unexpected stub offset {addr:#x}')
        a, b, c = [self.reg(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)]
        value = 0
        if name in ('rtw_enter_critical_bh', 'rtw_exit_critical_bh'):
            pass
        elif name == 'rtw_queue_empty':
            value = int(self.u32(a) == a)
        elif name == 'rtw_init_listhead':
            self.init_list(a)
        elif name == 'rtw_list_delete':
            self.delete(a)
        elif name == 'rtw_list_insert_tail':
            self.insert(a, b)
        elif name == 'dev_alloc_skb':
            value = self.skb(a)
        elif name == 'rtw_memcpy':
            self.write(a, self.read(b, c))
            value = a
        elif name == 'rtw_memcmp':
            value = int(self.read(a, c) == self.read(b, c))
        elif name == '_htons':
            value = ((a & 255) << 8) | ((a >> 8) & 255)
        elif name == 'skb_set_tail_pointer':
            pass
        elif name == 'rltk_netif_rx':
            self.indicated.append(self.read(self.u32(a+16), self.u32(a+32)))
            self.release_skb(a)  # netif accepts/consumes skb ownership
        elif name == 'kfree_skb_chk_key':
            self.release_skb(a)
        elif name == 'rtw_get_stainfo' and self.mode == 'indicate_reject':
            value = 0  # native EAPOL reject path then frees the child
        else:
            raise AssertionError(f'unexpected external call {name}({a:#x},{b:#x},{c:#x})')
        uc.reg_write(UC_ARM_REG_R0, value)
        uc.reg_write(UC_ARM_REG_PC, self.reg(UC_ARM_REG_LR))

    def call(self, name, *args):
        self.uc.reg_write(UC_ARM_REG_SP, STACK+0xff00)
        self.uc.reg_write(UC_ARM_REG_LR, STOP|1)
        saved = list(range(UC_ARM_REG_R4, UC_ARM_REG_R11+1))
        for i, r in enumerate(saved):
            self.uc.reg_write(r, 0xdead0000+i)
        for r, arg in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args):
            self.uc.reg_write(r, arg)
        self.uc.emu_start(self.symbols[name]|1, STOP, count=100000)
        assert self.reg(UC_ARM_REG_PC) == STOP, 'instruction limit or invalid return'
        assert self.reg(UC_ARM_REG_SP) == STACK+0xff00, 'stack not restored'
        assert [self.reg(r) for r in saved] == [0xdead0000+i for i in range(len(saved))]
        return self.reg(UC_ARM_REG_R0)

    def exercise(self, buddy=True, mode='ok'):
        self.mode = mode
        self.native_free_calls.clear()
        self.indicated.clear()
        self.freed.clear()
        processor = self.b if buddy else self.a
        parent = self.call('rtw_alloc_recvframe', self.a+Q)
        assert parent
        self.w32(parent+16, processor)  # native tasklet's existing handoff
        protocol = b'\x88\x8e' if mode == 'indicate_reject' else b'\x08\x00'
        subdata = b'\xaa\xaa\x03\0\0\0'+protocol+bytes(range(16))
        if mode == 'short_subframe':
            subdata = b'\0'*7
        if mode == 'oversized_subframe':
            declared = 500
        else:
            declared = len(subdata)
        sub = bytes.fromhex('020304050607101112131415')+struct.pack('>H', declared)+subdata
        sub += b'\0'*((-len(sub)) & 3)
        payload = sub*2
        skb = self.skb(len(payload))
        self.write(skb+64, payload)
        self.w32(parent+8, skb)
        self.write(parent+0x21, b'\0')
        self.write(parent+0x30, b'\0')
        self.w32(parent+0xa0, len(payload))
        self.w32(parent+0xa8, skb+64)
        self.w32(parent+0xac, skb+64+len(payload))
        self.w32(parent+0xb0, skb+64+len(payload))
        self.w32(processor+0x32c4, int(mode == 'stopped'))
        self.w32(processor+0x32c8, int(mode == 'removed'))
        self.write(processor+0x1865, bytes([int(mode == 'indicate_reject')]))
        self.fail_alloc_at = self.alloc_calls + {'alloc_first': 1, 'alloc_second': 2}.get(mode, -99999)
        self.fail_skb_at = self.skb_calls + {'skb_first': 1, 'skb_second': 2}.get(mode, -99999)
        rc = self.call('amsdu_to_msdu', processor, parent)
        if rc != 1:
            # Existing caller recv_indicatepkts_in_order owns parent on failure.
            assert parent not in self.native_free_calls
            self.call('rtw_free_recvframe', parent, processor+Q)
        assert len(self.native_free_calls) == len(set(self.native_free_calls)), 'frame double free'
        assert self.native_free_calls.count(parent) == 1
        assert not self.live_skb, 'skb leak'
        if mode == 'ok':
            assert rc == 1 and len(self.indicated) == 2
            for packet in self.indicated:
                assert packet[:14] == sub[:12]+protocol
                assert packet[14:] == bytes(range(16))
        return rc

    def assert_balanced(self):
        for adapter in (self.a, self.b):
            actual = self.queue(adapter)
            assert actual == self.frames[adapter], (
                f'wrong pool: A={len(self.queue(self.a))}, B={len(self.queue(self.b))}, '
                f'A_count={self.u32(self.a+COUNT)}, B_count={self.u32(self.b+COUNT)}')
            assert self.u32(adapter+COUNT) == 32


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prefix', required=True)
    p.add_argument('--vendor-archive', type=Path, required=True)
    p.add_argument('--unfixed', action='store_true', help='RED: run recovery assertion against unfixed object')
    p.add_argument('--iterations', type=int, default=10000)
    args = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='amsdu-native-test-') as td:
        fixture = link_fixture(Path(td)/'fixture', args.prefix, args.vendor_archive, not args.unfixed)
        m = Machine(fixture)
        m.exercise()
        m.assert_balanced()
        print('PASS: buddy A-MSDU indication restores native queues and counts')
        for swapped in (False, True):
            for buddy in (False, True):
                for mode in ('ok', 'alloc_first', 'alloc_second', 'skb_first', 'skb_second',
                             'indicate_reject', 'short_subframe', 'oversized_subframe', 'stopped', 'removed'):
                    m = Machine(fixture, swapped, mode)
                    m.exercise(buddy, mode)
                    m.assert_balanced()
        print('PASS: 40 role/success/failure combinations, skb and frame lifetime, register/stack ABI')
        m = Machine(fixture)
        held = [m.call('rtw_alloc_recvframe', m.a+Q) for _ in range(31)]
        assert len(set(held)) == 31 and all(held)
        assert m.exercise(mode='pool_full') == 0  # real empty queue, no injection
        assert len(m.queue(m.a)) == m.u32(m.a+COUNT) == 1
        for frame in held:
            m.call('rtw_free_recvframe', frame, m.a+Q)
        m.assert_balanced()
        for processor in (m.a, m.b):
            frame = m.call('rtw_alloc_recvframe', m.a+Q)
            m.w32(frame+16, processor)
            skb = m.skb(30)
            packet = bytes.fromhex('0203040506071011121314150800')+bytes(range(16))
            m.write(skb+64, packet)
            m.w32(frame+8, skb)
            m.w32(frame+0xa8, skb+64)
            m.w32(frame+0xa0, len(packet))
            assert m.call('rtw_recv_indicatepkt', processor, frame) == 1
            assert m.indicated[-1] == packet and not m.live_skb
            m.assert_balanced()
        print('PASS: real pool exhaustion/recovery and ordinary primary/buddy indication')
        m = Machine(fixture)
        for _ in range(args.iterations):
            m.exercise()
            m.assert_balanced()
        print(f'PASS: {args.iterations} repeated buddy A-MSDUs, {m.child_inits} children; both pools intact')
        if not args.unfixed:
            test_patch_and_verifier(Path(td), args.prefix, args.vendor_archive)


def test_patch_and_verifier(td, prefix, vendor):
    import patch_amsdu_child_adapter as patcher
    base = td/'base.o'
    base.write_bytes(run(prefix+'ar', 'p', vendor, 'rtw_recv.o'))
    for script in ('patch_amsdu_alloc_failure.py', 'patch_recvframe_pool.py'):
        run(sys.executable, HERE/script, base)
    original = base.read_bytes()
    fixed = patcher.patch(original)
    assert patcher.patch(fixed) == fixed
    patcher.check(fixed)
    original_elf, fixed_elf = ELFFile(io.BytesIO(original)), ELFFile(io.BytesIO(fixed))
    for section in original_elf.iter_sections():
        if section.name in ('.symtab', '.strtab', '.rel.wlan.text.amsdu_to_msdu'):
            continue
        assert section.data() == fixed_elf.get_section_by_name(section.name).data(), section.name
    syms_before = original_elf.get_section_by_name('.symtab')
    syms_after = fixed_elf.get_section_by_name('.symtab')
    assert syms_after.data()[:-16] == syms_before.data()
    changes = []
    rb = original_elf.get_section_by_name('.rel.wlan.text.amsdu_to_msdu')
    ra = fixed_elf.get_section_by_name(rb.name)
    for old, new in zip(rb.iter_relocations(), ra.iter_relocations()):
        assert old['r_offset'] == new['r_offset']
        assert old['r_info_type'] == new['r_info_type']
        if old['r_info_sym'] != new['r_info_sym']:
            changes.append(old['r_offset'])
            assert syms_after.get_symbol(new['r_info_sym']).name == 'wlan_amsdu_child_init'
    assert changes == [0x76]
    base.write_bytes(fixed)
    for script in ('patch_amsdu_alloc_failure.py', 'patch_recvframe_pool.py', 'patch_amsdu_child_adapter.py'):
        run(sys.executable, HERE/script, '--check', base)
    assert base.read_bytes() == fixed, '--check must be read-only'
    candidates = [original[:-1], b'', fixed[:-1], original+b'changed']
    for offset in (0, 32, rb['sh_offset'], rb['sh_offset']+4,
                   original_elf.get_section_by_name(patcher.FUNCTION)['sh_offset']+0xc):
        bad = bytearray(original)
        bad[offset] ^= 1
        candidates.append(bytes(bad))
    for bad in candidates:
        try:
            patcher.patch(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('accepted unknown/damaged object')
    print('PASS: exact one-relocation delta, preserved symbols/sections, idempotence, read-only check, 9 bad inputs')
    directory = td/'fixture'
    archive = td/'fixed.a'
    run(prefix+'ar', 'rcs', archive, directory/'rtw_recv.o', directory/'freertos_recv.o',
        directory/'wlan_amsdu_child_init.o')
    command = [sys.executable, HERE/'verify_amsdu_child_adapter.py', '--prefix', prefix,
               '--archive', archive, '--elf', directory/'fixture.elf']
    print(run(*command).decode().strip())
    bad_elf = td/'bad.elf'
    elf = ELFFile(io.BytesIO(fixture_bytes := (directory/'fixture.elf').read_bytes()))
    sym = elf.get_section_by_name('.symtab').get_symbol_by_name('wlan_amsdu_child_init')[0]
    addr = sym['st_value'] & ~1
    damaged = bytearray(fixture_bytes)
    for sec in elf.iter_sections():
        if sec['sh_flags'] & 2 and sec['sh_addr'] <= addr < sec['sh_addr']+sec['sh_size']:
            damaged[sec['sh_offset']+addr-sec['sh_addr']+2] ^= 1
            break
    bad_elf.write_bytes(damaged)
    result = subprocess.run([*map(str, command[:-1]), str(bad_elf)], capture_output=True)
    assert result.returncode != 0, 'accepted corrupted final helper'
    # Same final ELF with an unfixed archive must also fail archive verification.
    base.write_bytes(original)
    broken_dir = td/'broken'
    broken_dir.mkdir()
    (broken_dir/'rtw_recv.o').write_bytes(original)
    run(prefix+'ar', 'rcs', archive, broken_dir/'rtw_recv.o')
    result = subprocess.run(list(map(str, command)), capture_output=True)
    assert result.returncode != 0, 'accepted archive missing the fix'
    print('PASS: verifier rejects bad final helper and unfixed archive')


if __name__ == '__main__':
    main()
