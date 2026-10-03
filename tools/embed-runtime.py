#!/usr/bin/env python3
"""Embed the pinned native shell and tools into the self-installing payload."""
import hashlib
from pathlib import Path

root = Path(__file__).resolve().parents[1]
shell = (root / 'build/shell/sh.elf').read_bytes()
box = (root / 'build/shell/sbase-box.elf').read_bytes()
names = (root / 'platform/runtime-tools.txt').read_text().splitlines()
if not names or len(set(names)) != len(names) or any(not name.isascii() or not name.isalnum() for name in names):
    raise SystemExit('invalid runtime tool names')
identity = hashlib.sha256(shell + box + '\n'.join(names).encode()).hexdigest()[:32]
lines = [f'#define PS5_RUNTIME_ID "{identity}"']
for name, data in [('shell', shell), ('box', box)]:
    lines.append(f'static const unsigned char runtime_{name}[] = {{')
    lines.extend(','.join(str(b) for b in data[i:i+24]) + ',' for i in range(0, len(data), 24))
    lines.append('};')
lines.extend(['struct runtime_asset { const char *name; const unsigned char *data; size_t size; };',
              'static const struct runtime_asset runtime_assets[] = {',
              '{"sh", runtime_shell, sizeof(runtime_shell)},',
              '{"sbase-box", runtime_box, sizeof(runtime_box)},'])
lines.extend(f'{{"{name}", runtime_box, sizeof(runtime_box)}},' for name in names)
lines.append('};')
(root / 'build/runtime-tools.h').write_text('\n'.join(lines) + '\n')
print(f'Embedded shell and {len(names)} tools: {identity}')
