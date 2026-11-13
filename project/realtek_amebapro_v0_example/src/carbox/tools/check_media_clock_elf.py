#!/usr/bin/env python3
"""Reject a shadow firmware whose observer was discarded or bypassed by linking."""
import re
import subprocess
import sys
asm = subprocess.check_output([sys.argv[1], '-d', sys.argv[2]], text=True)
functions = dict(re.findall(r'^\w+ <([^>]+)>:\n(.*?)(?=^\w+ <|\Z)', asm, re.M | re.S))
checks = {
    '_TimingReceiveResponse': ('__wrap_AirTunesClock_Adjust',),
    '__wrap_AirTunesClock_Adjust': ('AirTunesClock_Adjust', 'AirTunesClock_GetSynchronizedTimeNearUpTicks'),
    '__wrap_AirTunesClock_Finalize': ('AirTunesClock_Finalize',),
    'pcprof_task': ('carbox_media_clock_evidence_tick', 'carbox_media_clock_evidence_report'),
}
for caller, targets in checks.items():
    body = functions.get(caller, '')
    for target in targets:
        if '<' + target + '>' not in body:
            raise SystemExit('media clock ELF: missing call ' + caller + ' -> ' + target)
# At least one native lifecycle call must use the observer; a definition alone is insufficient.
if not any('<__wrap_AirTunesClock_Finalize>' in body for name, body in functions.items()
           if not name.startswith('__wrap_')):
    raise SystemExit('media clock ELF: native finalization does not reach observer')
if '<AirTunesClock_Adjust>' in functions['_TimingReceiveResponse']:
    raise SystemExit('media clock ELF: native receiver bypasses observer')
print('media clock ELF: receiver -> observer -> native, finalization, periodic reporter verified')

if len(sys.argv)>3 and sys.argv[3]=='1':
    checks={
      'AirPlayScreen_SendScreenConfigure':('carbox_screen_time_config',),
      'AirPlayScreen_SendScreenNormalFrame':('carbox_screen_time_video','carbox_screen_encrypt_observe'),
      'AirPlayTimer_Start':('carbox_screen_time_start',),
      'AirPlayTimer_SetTimeRTCPData':('carbox_screen_time_receive','carbox_screen_time_transmit'),
      'AirPlayTimer_Thread':('carbox_screen_timing_sendto',),
      'carbox_screen_time_config':('AirPlayClock_VideoTimestamp',),
      'carbox_screen_time_video':('AirPlayClock_VideoTimestamp',),
      'carbox_screen_time_start':('AirPlayClock_StartTime',),
      'carbox_screen_time_receive':('AirPlayClock_GetTime',),
      'carbox_screen_time_transmit':('AirPlayClock_GetTime',),
      'carbox_screen_timing_sendto':('lwip_sendto',),
      '__wrap_AirPlayScreen_SendVideo':('carbox_screen_flow_frame',),
      'carbox_move_screen_frame':('carbox_screen_flow_frame',),
      'carbox_move_screen_free':('carbox_screen_flow_frame',),
      'carbox_move_lag_report':('carbox_screen_flow_report',),
    }
    for caller,targets in checks.items():
      for target in targets:
        if '<'+target+'>' not in functions.get(caller,''):
          raise SystemExit('screen flow ELF: missing '+caller+' -> '+target)
    print('screen flow ELF: ingress/forward/free, video/timing and native forwarding verified')
