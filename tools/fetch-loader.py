#!/usr/bin/env python3
"""Fetch and verify the pinned loader files without overwriting local changes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--offline", action="store_true", help="Verify existing files only")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
pin = json.loads((root / "sources.lock.json").read_text())["loader"]
destination = root / "vendor/shsrv"
destination.mkdir(parents=True, exist_ok=True)
patched = pin.get("patched_files", {})
patch = pin.get("patch")
if patch and hashlib.sha256((root / patch["path"]).read_bytes()).hexdigest() != patch["sha256"]:
    raise SystemExit("Loader patch checksum mismatch")
for name, expected in pin["files"].items():
    if Path(name).name != name:
        raise SystemExit("Invalid loader filename")
    path = destination / name
    if path.exists():
        if hashlib.sha256(path.read_bytes()).hexdigest() not in {expected, patched.get(name)}:
            raise SystemExit(f"Loader source differs from the pin; refusing to overwrite {name}")
        continue
    if args.offline:
        raise SystemExit(f"Missing loader file {name}; run tools/fetch-loader.py on the host")
    url = f"https://raw.githubusercontent.com/ps5-payload-dev/shsrv/{pin['commit']}/{name}"
    with urllib.request.urlopen(url, timeout=30) as response:
        data = response.read(1024 * 1024 + 1)
    if hashlib.sha256(data).hexdigest() != expected:
        raise SystemExit(f"Loader checksum mismatch: {name}")
    with path.open("xb") as output:
        output.write(data)
if patch:
    actual = {name: hashlib.sha256((destination / name).read_bytes()).hexdigest() for name in patched}
    if actual != patched:
        if any(actual[name] != pin["files"][name] for name in patched):
            raise SystemExit("Loader patch is partially applied; refusing to overwrite files")
        if args.offline:
            raise SystemExit("Apply the loader patch with tools/fetch-loader.py on the host first")
        command = ["git", "apply", "--directory=vendor/shsrv"]
        subprocess.run([*command, "--check", patch["path"]], cwd=root, check=True)
        subprocess.run([*command, patch["path"]], cwd=root, check=True)
    for name, expected in patched.items():
        if hashlib.sha256((destination / name).read_bytes()).hexdigest() != expected:
            raise SystemExit(f"Patched loader checksum mismatch: {name}")
print(f"Verified {len(pin['files'])} loader files at {pin['commit']}")
