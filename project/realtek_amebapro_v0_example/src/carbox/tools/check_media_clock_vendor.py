#!/usr/bin/env python3
"""Read-only ABI/version gate: never patches customer archives."""
from pathlib import Path
import hashlib,json,subprocess,sys
manifest=json.loads((Path(__file__).resolve().parents[1]/'media_clock/vendor_v19.json').read_text())
for archive,members in manifest.items():
    for name,expected in members.items():
        raw=subprocess.check_output([sys.argv[1],'p',str(Path(sys.argv[2])/archive),name])
        if hashlib.sha256(raw).hexdigest()!=expected:
            raise SystemExit('media clock evidence: unsupported '+archive+'('+name+'); review ABI before enabling')
print('media clock: v19 clock/audio/feedback object identities verified; shadow only')
