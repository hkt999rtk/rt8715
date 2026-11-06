import sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from prepare_mp import prepare, PrepareError
GOOD=b'[REC_MP_STATUS] phase=MP ready=1 slot=2 fw1_valid=0 cpu_hz=300000000 mode=1 driver=1/0 promisc=0 ota_busy=0 mp_mode=1 fw_state=0x00010081 test_state=1\r\n'
OTA=b'[REC_MP_STATUS] phase=OTA ready=0 slot=2 fw1_valid=0 cpu_hz=300000000 mode=3 driver=1/1 promisc=1 ota_busy=0\r\n'
class Serial:
 def __init__(self,states): self.states=iter(states);self.lines=[];self.commands=[]
 def reset_input_buffer(self):self.lines=[]
 def write(self,data):self.commands.append(data);self.lines=list(next(self.states));return len(data)
 def flush(self):pass
 def readline(self):
  if not self.lines: raise PrepareError('no more simulated input')
  return self.lines.pop(0)
class Tests(unittest.TestCase):
 def test_ota_switch_then_fresh_status(self):
  s=Serial([[b'#\r\n',OTA],[b'#\r\n',b'[REC_MP] READY; baud=1500000\r\n'],[GOOD]])
  prepare(s,1,lambda _:None);self.assertEqual(s.commands,[b'MP=STATUS\r',b'MP=4\r',b'MP=STATUS\r'])
 def test_already_mp_no_reboot(self):
  s=Serial([[GOOD]]);prepare(s,1,lambda _:None);self.assertEqual(s.commands,[b'MP=STATUS\r'])
 def test_rejected_stops(self):
  s=Serial([[OTA],[b'[REC_MP] OTA busy; MP=4 rejected\r\n']])
  with self.assertRaises(PrepareError):prepare(s,1,lambda _:None)
  self.assertEqual(len(s.commands),2)
 def test_ready_then_wrong_state_stops(self):
  s=Serial([[OTA],[b'[REC_MP] READY; baud=1500000\r\n'],[OTA]])
  with self.assertRaises(PrepareError):prepare(s,1,lambda _:None)
 def test_inconsistent_ready_rejected(self):
  s=Serial([[GOOD.replace(b'promisc=0',b'promisc=1')]])
  with self.assertRaises(PrepareError):prepare(s,1,lambda _:None)
 def test_missing_mp_flags(self):
  s=Serial([[GOOD.replace(b'fw_state=0x00010081',b'fw_state=0x00000080')]])
  with self.assertRaises(PrepareError):prepare(s,1,lambda _:None)
 def test_mp_init_failed(self):
  s=Serial([[OTA],[b'[REC_MP] MP START FAILED ret=-1004; commands blocked\r\n']])
  with self.assertRaises(PrepareError):prepare(s,1,lambda _:None)
if __name__=='__main__':unittest.main()
