#!/usr/bin/env python3
"""Embed this application's own home-screen shortcut metadata and icon."""
import json
import re
from pathlib import Path
root = Path(__file__).resolve().parents[1]
config = (root / 'app/config.h').read_text()
port = int(re.search(r'#define PS5_AI_PORT (\d+)', config)[1])
title = re.search(r'#define PS5_AI_TITLE "([A-Z0-9]{9})"', config)[1]
manifest = {'applicationCategoryType': 65536, 'titleId': title,
            'localizedParameters': {'defaultLanguage': 'en-US', 'en-US': {'titleName': 'PS5 AI CLI'}},
            'deeplinkUri': f'http://127.0.0.1:{port}/?console=1'}
lines = ['/* Generated; do not edit. */']
for name, data in [('launcher_manifest', (json.dumps(manifest, indent=2)+'\n').encode()),
                   ('launcher_icon', (root/'launcher/sce_sys/icon0.png').read_bytes())]:
    lines.append(f'static const unsigned char {name}[] = {{'+','.join(map(str, data))+'};')
(root/'build').mkdir(exist_ok=True)
(root/'build/launcher.h').write_text('\n'.join(lines)+'\n')
