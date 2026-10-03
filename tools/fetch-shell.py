#!/usr/bin/env python3
"""Fetch pinned shell/tool sources and apply the PS5 shell patch."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--offline', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
pins = json.loads((root / 'sources.lock.json').read_text())
for name in ('dash', 'sbase'):
    pin, destination = pins[name], root / 'vendor' / name
    if not destination.exists():
        if args.offline:
            raise SystemExit(f'Missing {name}; run tools/fetch-shell.py first')
        subprocess.run(['git', 'init', str(destination)], check=True)
        subprocess.run(['git', 'fetch', '--depth', '1', pin['repository'], pin['commit']],
                       cwd=destination, check=True)
        subprocess.run(['git', 'checkout', '--detach', 'FETCH_HEAD'], cwd=destination, check=True)
    actual = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=destination, text=True).strip()
    if actual != pin['commit']:
        raise SystemExit(f'Unexpected {name} revision; refusing to overwrite it')
    for patch in sorted((root / 'patches' / name).glob('*.patch')):
        applied = subprocess.run(['git', 'apply', '--reverse', '--check', str(patch)],
                                 cwd=destination, capture_output=True).returncode == 0
        if not applied:
            subprocess.run(['git', 'apply', '--check', str(patch)], cwd=destination, check=True)
            subprocess.run(['git', 'apply', str(patch)], cwd=destination, check=True)
    print(f'{name}: {actual}')
