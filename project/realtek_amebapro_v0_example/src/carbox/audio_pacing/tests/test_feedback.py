from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as td:
    exe=Path(td)/'test'
    subprocess.run(['cc','-std=c99','-O1','-Wall','-Wextra','-Werror',
                    '-fsanitize=undefined','-I'+str(root),
                    str(root/'tests/test_feedback.c'),str(root/'AudioFeedback.c'),
                    '-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
