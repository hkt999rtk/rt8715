#!/usr/bin/env python3
"""Diagnostic symbol redirects only; require the reviewed customer object hash."""
import argparse,hashlib,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('object',type=Path);p.add_argument('--objcopy',required=True);a=p.parse_args()
expected='678cd07378f07afd6aeb4f0bde31c2d8ac550d8726012ed88f040f9b42f2a964'
if hashlib.sha256(a.object.read_bytes()).hexdigest()!=expected:
 raise SystemExit('Unreviewed ncm_tx_async.o: refuse diagnostic ABI hooks')
args=[a.objcopy]
# objcopy applies symbol renames before globalize matching. Match the exported
# names so these queue handles are linkable from the diagnostic adapter.
for name in ('navtx_vendor_input_queue','navtx_vendor_ready_queue'):args+=['--globalize-symbol',name]
for src,dst in {
 'g_ncm_tx_async_queue':'navtx_vendor_input_queue','g_ncm_tx_ready_queue':'navtx_vendor_ready_queue',
 'xQueueGenericSend':'navtx_vendor_send','xQueueReceive':'navtx_vendor_receive',
 'carbox_ncm_tx_build_batch':'navtx_vendor_build','carbox_ncm_tx_send_prebuilt':'navtx_vendor_usb',
 'pbuf_free':'navtx_vendor_free',
}.items():args+=['--redefine-sym',src+'='+dst]
subprocess.run(args+[str(a.object)],check=True)
print('NAV_TX vendor hook: hash checked, batch16/item20 ABI, scoped symbol redirects')
