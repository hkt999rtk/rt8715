#!/usr/bin/env python3
"""Verify the actual linked navigation pacing hook and native loop return."""
from pathlib import Path
import argparse,re,subprocess
P='/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
a=argparse.ArgumentParser();a.add_argument('elf',type=Path);args=a.parse_args()
dis=subprocess.check_output([P+'objdump','-d',str(args.elf)],text=True)
def body(n):
 m=re.search(r'^([0-9a-f]+) <'+re.escape(n)+r'>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)',dis,re.M|re.S)
 assert m,n
 return int(m[1],16),m[2]
base,code=body('AirPlayAudio_AudioOutputThread')
assert re.search(r'^\s*'+hex(base+0x118)[2:]+r':.*<carbox_nav_output_pace>',code,re.M)
_,hook=body('carbox_nav_output_pace')
assert '<carbox_nav_output_pace_c>' in hook and '<RingBuffer_size>' in hook
assert 'sub.w\tlr, lr, #140' in hook
assert re.search(r'^\s*'+hex(base+0x90)[2:]+r':.*ldr.w\s+fp, \[r4, #52\]',code,re.M)
_,helper=body('carbox_nav_output_pace_c')
for target in ['nav_pace_schedule','RingBuffer_size','vTaskDelay']:
 assert target in helper,target
for name in ['nav_pace_schedule','nav_pace_pop','nav_pace_send_enter','nav_pace_send_exit','carbox_nav_output_pace_c']:
 _,text=body(name)
 for forbidden in ['<printf>','<carbox_rxhd_log>','<malloc>','<calloc>']:
  assert forbidden not in text,(name,forbidden)
print('PASS final pacing ELF: additional relocation, loop stop check, native fallback, controller/wait, no hot-path logging/allocation')
