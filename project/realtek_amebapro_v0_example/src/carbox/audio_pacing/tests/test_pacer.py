from pathlib import Path
import subprocess,tempfile
p=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as td:
 exe=Path(td)/'pacer'
 subprocess.run(['cc','-std=c99','-O2','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I'+str(p.parent),str(p/'test_pacer.c'),str(p.parent/'AudioSamplePacer.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
