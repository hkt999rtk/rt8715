#!/usr/bin/env python3
"""Resolve the actual linked navigation policy calls in a test firmware."""
import argparse,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('elf',type=Path);a=p.parse_args()
prefix='/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
dis=subprocess.check_output([prefix+'objdump','-d',str(a.elf)],text=True)
def body(name):
 m=re.search(r'^([0-9a-f]+) <'+re.escape(name)+r'>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)',dis,re.M|re.S)
 assert m,name
 return m[2]
for caller,target in [('carbox_nav_hook_1','carbox_nav_condition_wait'),
 ('carbox_nav_hook_17','carbox_nav_condition_signal'),('carbox_nav_hook_18','carbox_nav_condition_signal'),
 ('RTPJitterBufferFree','carbox_nav_condition_signal'),
 ('__wrap_RTPJitterBufferInit','RTPJitterBufferInit'),
 ('__wrap_RTPJitterBufferInit','carbox_nav_diag_open'),
 ('__wrap_RTPJitterBufferFree','RTPJitterBufferFree'),
 ('__wrap_RTPJitterBufferFree','carbox_nav_diag_close'),
 ('carbox_nav_condition_wait','pthread_cond_wait'),
 ('carbox_nav_condition_signal','pthread_cond_signal')]:
 assert '<'+target+'>' in body(caller),(caller,target)
 print('PASS',caller,'->',target)
assert 'carbox_jitter_read' not in dis
print('PASS: production policy and diagnostics linked; legacy adaptive reader absent')

if 'carbox_nav_convert_hook' in dis:
 for caller,target in [('carbox_nav_hook_4','carbox_nav_convert_hook'),
  ('carbox_nav_hook_11','carbox_nav_future_hook'),('carbox_nav_hook_13','carbox_nav_gap_hook'),
  ('carbox_nav_hook_21','carbox_nav_startup_purge'),
  ('RTPJitterBufferFree','carbox_nav_condition_destroy_mutex'),
  ('carbox_nav_condition_destroy_mutex','carbox_nav_continuity_before_mutex_destroy'),
  ('carbox_nav_condition_destroy_mutex','pthread_mutex_destroy'),
  ('__wrap_RTPJitterBufferInit','carbox_nav_continuity_open'),
  ('__wrap_RTPJitterBufferFree','carbox_nav_continuity_close'),
  ('carbox_nav_convert','__wrap_aacDecoder_DecodeFrame'),
  ('carbox_nav_convert','AudioConverterFillComplexBuffer')]:
  assert '<'+target+'>' in body(caller),(caller,target)
  print('PASS continuity',caller,'->',target)
