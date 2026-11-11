#!/usr/bin/env python3
"""Run the production C against deterministic RTOS/socket/CF dependencies."""
import pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class ScreenSessionTest(unittest.TestCase):
 def test_stop_write_and_hid_nal_evidence(self):
  files=[ROOT/'screen_session.c',ROOT/'screen_setup_evidence.c']
  for f in files: self.assertTrue(f.exists(),f'{f.name}: stop/cleanup and evidence implementation missing')
  with tempfile.TemporaryDirectory() as td:
   d=pathlib.Path(td); (d/'lwip').mkdir()
   (d/'FreeRTOS.h').write_text('#include "stubs.h"\n')
   for n in ['task.h','semphr.h','hal_timer.h','diag.h']: (d/n).write_text('#include "stubs.h"\n')
   for n in ['sockets.h','errno.h']: (d/'lwip'/n).write_text('#include "stubs.h"\n')
   (d/'stubs.h').write_text((ROOT/'tools/screen_session_test_stubs.h').read_text())
   cmd=['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-DCONFIG_SCREEN_SESSION_FIX=1','-DCONFIG_MOVE_LAG_PROFILE=1','-I'+str(d),'-I'+str(ROOT),*map(str,files),str(ROOT/'tools/screen_session_test.c'),'-o',str(d/'test')]
   subprocess.run(cmd,check=True); subprocess.run([str(d/'test')],check=True)
if __name__=='__main__': unittest.main()
