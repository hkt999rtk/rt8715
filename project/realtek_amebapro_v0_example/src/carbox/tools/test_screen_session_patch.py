#!/usr/bin/env python3
import pathlib,subprocess,tempfile,unittest,sys,os
from test_patch_airplay_music_output import elf_object
TOOLS=pathlib.Path(__file__).resolve().parent
BUILD=TOOLS.parents[2]/'GCC-RELEASE'
P='/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-'
class ArchiveTest(unittest.TestCase):
 def test_hooks_preserve_every_instruction_and_relocation_count(self):
  script=TOOLS/'patch_screen_session.py'
  self.assertTrue(script.exists(),'missing guarded lifecycle archive integration')
  with tempfile.TemporaryDirectory() as td:
   output=pathlib.Path(td)/'fixed.a'
   source=BUILD/'carplay_app/lib_Accessory2_resource450.a'
   subprocess.run([sys.executable,str(script),P+'ar',P+'objcopy',str(source),str(output)],check=True)
   for member in ['AirPlayScreen.o','AirPlayClient.o','AirPlayResponse.o']:
    get=lambda a:elf_object(subprocess.check_output([P+'ar','p',str(a),member]))
    a,ar,asy=get(source);b,br,bsy=get(output)
    for name in a:
     if name.startswith('.text.'):self.assertEqual(a[name],b[name],name)
    self.assertEqual({k:len(v) for k,v in ar.items()},{k:len(v) for k,v in br.items()})
    if member=='AirPlayScreen.o':
     self.assertEqual(bsy['carbox_screen_sending'][2],1)
     self.assertEqual(bsy['carbox_screen_sending'][3]>>4,1)
     self.assertEqual(bsy['carbox_screen_mutex'][3]>>4,1)
     self.assertEqual(bsy['carbox_screen_mutex'][2],96)
     self.assertEqual(bsy['AirPlayScreen_SendScreenData'][5],0)
     self.assertTrue(any(x[2]=='carbox_screen_write' for x in br['.rel.text.AirPlayScreen_SendScreenNormalFrame']))
     self.assertTrue(any(x[2]=='AirPlayScreen_SendScreenData' for x in br['.rel.text.AirPlayScreen_SendScreenConfigure']))
    if member=='AirPlayResponse.o':
     self.assertIn((20,10,'carbox_info_hid_lookup'),br['.rel.text.AirPlayResponse_GetInfoInfoHIDDevices'])
    if member=='AirPlayClient.o':
     self.assertEqual(bsy['carbox_vendor_info_complete'][3]>>4,1)
     self.assertIn((152,2,'carbox_info_complete'),br['.rel.text.AirPlayClient_SendMessageSync'])
   sentinel=pathlib.Path(td)/'sentinel.a';sentinel.write_bytes(b'keep')
   r=subprocess.run([sys.executable,str(script),P+'ar',P+'objcopy',str(output),str(sentinel)],capture_output=True)
   self.assertNotEqual(r.returncode,0);self.assertEqual(sentinel.read_bytes(),b'keep')
 def test_evidence_only_keeps_vendor_lifecycle_and_io(self):
  with tempfile.TemporaryDirectory() as td:
   output=pathlib.Path(td)/'evidence.a'
   source=BUILD/'carplay_app/lib_Accessory2_resource450.a'
   subprocess.run([sys.executable,str(TOOLS/'patch_screen_session.py'),P+'ar',P+'objcopy',str(source),str(output),'--evidence-only'],check=True)
   a,ar,asy=elf_object(subprocess.check_output([P+'ar','p',str(source),'AirPlayScreen.o']))
   b,br,bsy=elf_object(subprocess.check_output([P+'ar','p',str(output),'AirPlayScreen.o']))
   for name in a:
    if name.startswith('.text.'):self.assertEqual(a[name],b[name],name)
   expected={name:[(offset,kind,'carbox_screen_encrypt_observe' if name=='.rel.text.AirPlayScreen_SendScreenNormalFrame' and target=='AirPlayScreen_EncryptData' else target) for offset,kind,target in rels] for name,rels in ar.items()}
   self.assertEqual(expected,br)
   for name in ['AirPlayScreen_StartVideo','AirPlayScreen_StopVideo','AirPlayScreen_SendVideo','AirPlayScreen_SendScreenData','bScreenSend','sockScreen','screenFrames','mutexScreen','pthread_create','pthread_exit','lwip_write','lwip_poll']:
    self.assertEqual(asy[name][1:],bsy[name][1:],name)
   self.assertFalse(any(name.startswith('carbox_vendor_screen_') or name in ['carbox_screen_create','carbox_screen_write','carbox_screen_detach'] for name in bsy))
if __name__=='__main__':unittest.main()
