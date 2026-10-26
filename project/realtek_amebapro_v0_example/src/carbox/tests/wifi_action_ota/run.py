#!/usr/bin/env python3
"""Host regressions for Action OTA. Does not access a radio or physical flash."""
from pathlib import Path
import subprocess
import tempfile
HERE = Path(__file__).resolve().parent
OTA = HERE.parents[1] / 'ota'
INC = HERE.parents[3] / 'inc'
with tempfile.TemporaryDirectory() as tmp:
    for name in ('protocol', 'workflow', 'package'):
        out = str(Path(tmp) / name)
        sources = [str(HERE / f'test_{name}.c')]
        if name != 'package':
            sources.append(str(OTA / 'wifi_action_protocol.c'))
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie',
                        '-I'+str(OTA), '-I'+str(INC), *sources, '-o', out], check=True)
        subprocess.run([out], check=True)
subprocess.run(['python3', str(HERE / 'test_http.py')], check=True)
