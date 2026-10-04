#!/usr/bin/env python3
"""Package a fresh Cargo production ELF with reviewed source and notice archives."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

from payload import inspect_elf


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify_production(root):
    # Development pairing and captured-object relinks have different build IDs.
    # Recompute from the exact source/assets used by the production build.
    env = {**os.environ, 'PS5_DEV_PAIRING': '0', 'PS5_BUILD_PROFILE': 'release',
           'PS5_BUILD_ORIGIN': 'cargo'}
    subprocess.run(['python3', str(root/'tools/terminal-build-id.py')], env=env, check=True)
    identity = re.search(r'"([0-9a-f]{32})"', (root/'build/terminal-build-id.h').read_text())[1]
    data = (root/'build/ps5-ai-cli.elf').read_bytes()
    info = inspect_elf(data)
    note = struct.pack('<III12s32s', 9, 32, 0x50533541, b'PS5AICLI', identity.encode())
    shoff = struct.unpack_from('<Q', data, 40)[0]
    shsize, shcount = struct.unpack_from('<HH', data, 58)
    matches = 0
    for index in range(shcount):
        section = struct.unpack_from('<IIQQQQIIQQ', data, shoff + index * shsize)
        if section[1] == 7 and section[5] == len(note):
            matches += data[section[4]:section[4]+section[5]] == note
    if matches != 1:
        raise ValueError('ELF does not match the fresh Cargo production build; refusing to package it')
    return identity, info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-root', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True, help='Reviewed corresponding-source .tar.gz')
    parser.add_argument('--notices', type=Path, required=True, help='Reviewed third-party-notices .tar.gz')
    parser.add_argument('--revision', required=True, help='Full published source commit SHA')
    parser.add_argument('--output', type=Path, required=True, help='New, empty output directory')
    args = parser.parse_args()
    root = args.build_root.resolve()
    if not re.fullmatch(r'[0-9a-f]{40}', args.revision):
        parser.error('--revision must be a full Git commit SHA')
    for archive in (args.source, args.notices):
        if not archive.is_file() or not archive.name.endswith('.tar.gz'):
            parser.error('Source and notices must be existing .tar.gz archives')
    version = re.search(r'#define PS5_AI_VERSION "([0-9A-Za-z.-]+)"',
                        (root/'app/config.h').read_text())[1]
    identity, info = verify_production(root)
    args.output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(root/'build/ps5-ai-cli.elf', args.output/'ps5-ai-cli.elf')
    for archive, name in ((args.source, 'source'), (args.notices, 'notices')):
        shutil.copyfile(archive, args.output/f'ps5-ai-cli-v{version}-{name}.tar.gz')
    manifest = {'version': version, 'source_commit': args.revision, 'build_id': identity,
                'profile': 'release', 'pairing': 'random', 'build_origin': 'cargo',
                'tool_trace': False, 'elf': {k: info[k] for k in ('bytes', 'sha256', 'needed')}}
    (args.output/'build-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    files = sorted(p for p in args.output.iterdir() if p.is_file())
    (args.output/'SHA256SUMS').write_text(''.join(f'{digest(p)}  {p.name}\n' for p in files))
    print(f'Packaged v{version}: {info["bytes"]} bytes; build {identity}')


if __name__ == '__main__':
    main()
