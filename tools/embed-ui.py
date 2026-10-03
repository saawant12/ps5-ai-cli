#!/usr/bin/env python3
"""Embed the offline web interface into the native ELF."""
from pathlib import Path
import hashlib
import json
root = Path(__file__).resolve().parents[1]
assets = [('index.html', 'text/html; charset=utf-8'), ('app.css', 'text/css; charset=utf-8'),
          ('app.js', 'text/javascript; charset=utf-8'), ('icon.svg', 'image/svg+xml'),
          ('fonts/JetBrainsMono-Regular.woff2', 'font/woff2'), ('fonts/JetBrainsMono-Bold.woff2', 'font/woff2')]
for filename, expected in json.loads((root / 'sources.lock.json').read_text())['terminal_font']['files'].items():
    if hashlib.sha256((root / filename).read_bytes()).hexdigest() != expected:
        raise SystemExit(f'Terminal font checksum mismatch: {filename}')
modules = [('xterm.js', '@xterm/xterm/lib/xterm.js', 'text/javascript; charset=utf-8'),
           ('xterm.css', '@xterm/xterm/css/xterm.css', 'text/css; charset=utf-8'),
           ('addon-fit.js', '@xterm/addon-fit/lib/addon-fit.js', 'text/javascript; charset=utf-8')]
inputs = [(name, root / 'web' / name, kind) for name, kind in assets]
inputs += [(name, root / 'web/node_modules' / module, kind) for name, module, kind in modules]
lines = ['/* Generated from web/; do not edit. */', '#include <stddef.h>']
entries = []
for i, (name, path, kind) in enumerate(inputs):
    data = path.read_bytes()
    if len(data) > 1024 * 1024:
        raise SystemExit('UI asset exceeds 1 MiB')
    lines.append(f'static const unsigned char ui_asset_{i}[] = {{' + ','.join(map(str, data)) + '};')
    entries.append('{' + json.dumps('/' + name) + ',' + json.dumps(kind) + f',ui_asset_{i},sizeof(ui_asset_{i})' + '}')
lines.append('static const struct { const char *path, *type; const unsigned char *data; size_t size; } ui_assets[] = {' + ','.join(entries) + '};')
(root / 'build').mkdir(exist_ok=True)
(root / 'build/ui-assets.h').write_text('\n'.join(lines) + '\n')
print(f'Embedded {len(inputs)} offline terminal assets.')
