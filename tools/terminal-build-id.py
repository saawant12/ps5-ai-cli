#!/usr/bin/env python3
"""Generate a deterministic marker used to identify the installed payload."""
import hashlib
import os
from pathlib import Path
root = Path(__file__).resolve().parents[1]
paths = [p for directory in ('app','launcher','platform','patches','targets') for p in (root/directory).rglob('*') if p.is_file()]
paths += [root/p for p in ('sources.lock.json','web/package-lock.json','locks/codex/Cargo.lock',
                           'build/ui-assets.h','build/ca-bundle.h','tools/check-codex.sh',
                           'tools/ps5-rust-linker.sh','tools/build-terminal.sh')]
if os.environ.get('PS5_DEV_PAIRING') == '1': paths.append(root/'build/terminal-pair.h')
hash = hashlib.sha256()
hash.update(os.environ.get('PS5_BUILD_PROFILE', 'debug').encode()+b'\0')
hash.update(os.environ.get('PS5_BUILD_ORIGIN', 'cargo').encode()+b'\0')
for p in sorted(paths):
    hash.update(str(p.relative_to(root)).encode()+b'\0'+p.read_bytes()+b'\0')
(root/'build/terminal-build-id.h').write_text('#define PS5_AI_BUILD_ID "'+hash.hexdigest()[:32]+'"\n')
