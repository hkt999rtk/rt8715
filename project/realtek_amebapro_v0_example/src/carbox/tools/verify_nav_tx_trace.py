#!/usr/bin/env python3
"""Verify diagnostic call paths in the actual linked vendor-NCM firmware."""
import argparse
import re
import subprocess
from pathlib import Path

PREFIX = '/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
p = argparse.ArgumentParser()
p.add_argument('elf', type=Path)
a = p.parse_args()
dis = subprocess.check_output([PREFIX+'objdump', '-d', str(a.elf)], text=True)
nm = subprocess.check_output([PREFIX+'nm', '-n', '-S', str(a.elf)], text=True)

def body(name):
    m = re.search(r'^[0-9a-f]+ <'+re.escape(name)+r'>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)', dis, re.M | re.S)
    assert m, name
    return m[1]

def reaches(code, target):
    if '<'+target+'>' in code:
        return True
    veneer = '__'+target+'_veneer'
    if '<'+veneer+'>' not in code:
        return False
    sym = re.search(r'^([0-9a-f]+) [0-9a-f]+ [Tt] '+re.escape(target)+r'$', nm, re.M)
    assert sym, target
    return re.search(r'\.word\s+0x%08x' % (int(sym[1], 16) | 1), body(veneer)) is not None

apply = '_Z18CConcealment_ApplyP16CConcealmentInfoP22CAacDecoderChannelInfoP28CAacDecoderStaticChannelInfoPK16SamplingRateInfoihij'
state = '_Z21CConcealment_GetStateP16CConcealmentInfo'
for caller, targets in {
    'carbox_music_output_sendto_c': ['navtx_begin', 'lwip_sendto', 'navtx_end'],
    'lwip_sendto': ['navtx_prepared', 'netconn_send'],
    'tcpip_send_msg_wait_sem': ['navtx_core_wait', 'navtx_core_acquired', 'navtx_core_release'],
    'ncm_tx_async_enqueue': ['navtx_vendor_send', 'navtx_vendor_free'],
    'ncm_tx_async_worker': ['navtx_vendor_receive', 'navtx_vendor_build', 'navtx_vendor_send', 'navtx_vendor_free'],
    'ncm_tx_usb_worker': ['navtx_vendor_usb'],
    'navtx_vendor_send': ['xQueueGenericSend', 'navtx_ncm'],
    'navtx_vendor_receive': ['xQueueReceive'],
    'navtx_vendor_build': ['carbox_ncm_tx_build_batch'],
    'navtx_vendor_usb': ['carbox_ncm_tx_send_prebuilt'],
    'navtx_vendor_free': ['pbuf_free'],
    'carbox_nav_convert': ['navtx_decode_context'],
    '__wrap_'+apply: [apply, state, 'navtx_fdk_state'],
}.items():
    code = body(caller)
    for target in targets:
        assert reaches(code, target), (caller, target)

# A wrapper present but never called would provide no runtime evidence.
assert re.search(r'\bbl\s+[0-9a-f]+ <__wrap_'+apply+r'>', dis)
assert body('__wrap_'+apply).count('<'+state+'>') == 2
for name in ('navtx_vendor_input_queue', 'navtx_vendor_ready_queue'):
    assert re.search(r'^[0-9a-f]+ [0-9a-f]+ [BD] '+name+r'$', nm, re.M), name

for name in ('records', 'contexts', 'metrics', 'fdk_events', 'report_copy'):
    m = re.search(r'^([0-9a-f]+) [0-9a-f]+ b '+name+r'$', nm, re.M)
    assert m and 0x70000000 <= int(m[1], 16) < 0x72000000, name
for name in re.findall(r'^[0-9a-f]+ [0-9a-f]+ T (navtx_\w+)$', nm, re.M):
    if name == 'navtx_report':
        continue
    for forbidden in ('printf', 'carbox_rxhd_log', 'malloc', 'calloc'):
        assert '<'+forbidden+'>' not in body(name), (name, forbidden)
print('PASS linked navigation trace: send/core/vendor NCM/native forwarding, FDK state hook, LPDDR storage, no direct hot-path logging/allocation')
