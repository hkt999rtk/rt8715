#!/usr/bin/env python3
"""Fail closed on changes to the vendor ABI used by recvframe diagnostics.

Pin the actual linked archive members, then verify native call sites resolve
into wrappers in the final ELF. This is read-only; it never patches firmware.
"""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess

MEMBERS = {
    # One child-init relocation redirected; all native instruction offsets unchanged.
    'rtw_recv.o': '452b1a0782e9ac7f0bb6cb7d57db72f375e2af15f711b640174e1e269cea0f8d',
    'freertos_recv.o': 'da79b7d94933eefcbc971a54ffca925898e8098594d7ed0d4791d955aec9de8e',
    'rtl8195b_recv.o': '1c5cb416660c3e500714833c43b8c0f308f941c83e543446decf5a5e361f742a',
}
# Instruction offsets, four bytes before the LR values used by the hooks.
CALLS = [
    ('_rtw_init_recv_priv', 0x62, 'rtw_list_insert_tail'),
    ('_rtw_free_recv_priv', 0x26, 'rtw_vmfree'),
    ('_rtw_alloc_recvframe', 0x04, 'rtw_queue_empty'),
    ('_rtw_alloc_recvframe', 0x10, 'rtw_list_delete'),
    ('rtw_free_recvframe', 0x34, 'rtw_list_delete'),
    ('rtw_free_recvframe', 0x42, 'rtw_list_insert_tail'),
    ('enqueue_reorder_recvframe', 0x2a, 'rtw_list_delete'),
    ('enqueue_reorder_recvframe', 0x32, 'rtw_list_insert_tail'),
    ('recv_indicatepkts_in_order', 0x8c, 'rtw_list_delete'),
    ('recv_indicatepkts_in_order', 0xb6, 'rtw_recv_indicatepkt'),
    ('recvframe_chk_defrag', 0x5c, 'rtw_list_insert_tail'),
    ('recvframe_chk_defrag', 0xdc, 'rtw_list_insert_tail'),
    ('_rtw_enqueue_recvframe', 0x08, 'rtw_list_delete'),
    ('_rtw_enqueue_recvframe', 0x10, 'rtw_list_insert_tail'),
    ('amsdu_to_msdu', 0x1fa, 'rtw_recv_indicatepkt'),
    ('process_recv_indicatepkts', 0x50, 'rtw_recv_indicatepkt'),
    ('recv_indicatepkt_reorder', 0xa8, 'rtw_recv_indicatepkt'),
    ('recv_indicatepkt_reorder', 0xec, 'rtw_recv_indicatepkt'),
    ('recv_indicatepkt_reorder', 0x13c, 'rtw_recv_indicatepkt'),
    ('recvframe_defrag', 0x0e, 'rtw_list_delete'),
    ('rtl8195b_recv_tasklet', 0x2d0, 'rtw_recv_entry'),
    ('rtl8195b_recv_tasklet', 0x348, 'rtw_recv_entry'),
]


def run(*args):
    return subprocess.check_output(args, text=True)


def verify_archive(path, prefix):
    names = run(prefix+'ar', 't', str(path)).splitlines()
    for member, expected in MEMBERS.items():
        if names.count(member) != 1:
            raise ValueError('missing or duplicate native member: '+member)
        data = subprocess.check_output([prefix+'ar', 'p', str(path), member])
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(member+' changed: review offsets, structure layout and ownership before updating guard')
    print('recvframe trace: native archive ABI verified (32 frames, stride 192)')


def verify_elf(path, prefix):
    elf = path.read_bytes()
    if elf[:6] != b'\x7fELF\x01\x01':
        raise ValueError('expected little endian ELF32')
    phoff = struct.unpack_from('<I',elf,28)[0]
    phsize, phnum = struct.unpack_from('<HH',elf,42)
    if phsize != 32: raise ValueError('unexpected program header size')
    segments = [struct.unpack_from('<8I',elf,phoff+i*phsize) for i in range(phnum)]
    def read_code(addr, size):
        for typ, off, vaddr, _, filesz, _, _, _ in segments:
            if typ == 1 and vaddr <= addr and addr+size <= vaddr+filesz:
                return elf[off+addr-vaddr:off+addr-vaddr+size]
        raise ValueError(f'ELF code address unmapped: {addr:#x}')
    symbols = {}
    sizes = {}
    for line in run(prefix+'nm', '-S', '-n', str(path)).splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[2] in ('T','t','W','w'):
            symbols[fields[3]] = int(fields[0],16) & ~1
            sizes[fields[3]] = int(fields[1],16)
    veneers = {addr for name,addr in symbols.items() if name.endswith('_veneer')}
    def resolve(addr):
        for _ in range(3):
            if addr not in veneers: return addr
            code = read_code(addr,16)
            # Realtek ld's long-call stub preserves arguments and LR:
            # push r0; ldr r0,[pc,#8]; mov ip,r0; pop r0; bx ip; nop; literal.
            if code[:12] != bytes.fromhex('01b40248844601bc604700bf'):
                raise ValueError(f'unreviewed veneer at {addr:#x}')
            target = struct.unpack_from('<I',code,12)[0]
            if target & 1 == 0: raise ValueError('non-Thumb veneer target')
            addr = target & ~1
        raise ValueError('veneer chain too deep')
    def disasm(start, size):
        return run(prefix+'objdump', '-d', '--start-address='+hex(start),
                   '--stop-address='+hex(start+size), str(path))
    def targets(text):
        return [(int(m[1],16),resolve(int(m[2],16))) for m in re.finditer(
            r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{4}\s+)+(?:(?:bl|blx|b\.w)\s+)([0-9a-f]+)\s+<', text,re.M)]
    for function, offset, callee in CALLS:
        pc = symbols[function]+offset
        if (pc,symbols['__wrap_'+callee]) not in targets(disasm(pc,4)):
            raise ValueError(f'{function}+{offset:#x} does not call __wrap_{callee}')
    # Wrappers must forward to the native implementations, not themselves.
    for callee in {c for _,_,c in CALLS}:
        wrapper = '__wrap_'+callee
        forward = 'carbox_recvframe_insert' if callee == 'rtw_list_insert_tail' else wrapper
        actual = [target for _,target in targets(disasm(symbols[forward],sizes[forward]))]
        if symbols[callee] not in actual or symbols[wrapper] in actual:
            raise ValueError('bad native forwarding in '+wrapper)
    # The shim reads a caller LR only from this exact native stack layout.
    # Other list callers must not read beyond their own stack frames.
    native = symbols['rtw_free_recvframe']
    if read_code(native+4,4) != bytes.fromhex('2de9f341'):
        raise ValueError('native free no longer saves r0,r1,r4-r8,lr; SP+28 invalid')
    shim = symbols['__wrap_rtw_list_insert_tail']
    code = read_code(shim,20)
    # mov r2,lr; ldr r3,[pc,#12]; cmp r2,r3; ite eq;
    # ldreq r3,[sp,#28]; movne r3,#0; b.w helper; Thumb return-site literal.
    if code[:12] != bytes.fromhex('7246034b9a420cbf079b0023'):
        raise ValueError('unreviewed recvframe caller shim: conditional stack read changed')
    if struct.unpack_from('<I',code,16)[0] != native+0x47:
        raise ValueError('caller shim native return-site literal lost Thumb bit or offset')
    if (shim+12,symbols['carbox_recvframe_insert']) not in targets(disasm(shim+12,4)):
        raise ValueError('caller shim does not tail-branch to observer')
    reporter = 'wlan_recvframe_diag_task'
    actual = [target for _,target in targets(disasm(symbols[reporter],sizes[reporter]))]
    for fn in ('printf_lock_init','carbox_rxhd_report','carbox_recvframe_trace_report'):
        if symbols[fn] not in actual:
            raise ValueError('common reporter missing '+fn)
    print(f'recvframe trace: {len(CALLS)} final ELF hook sites, saved-LR ABI, forwarding and reporter verified')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix',required=True)
    parser.add_argument('--archive',type=Path)
    parser.add_argument('--elf',type=Path)
    args=parser.parse_args()
    if not args.archive and not args.elf: parser.error('need --archive or --elf')
    if args.archive: verify_archive(args.archive,args.prefix)
    if args.elf: verify_elf(args.elf,args.prefix)


if __name__=='__main__':
    main()
