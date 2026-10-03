#!/usr/bin/env python3
"""Fetch the pinned upstream source and apply the local, reviewable PS5 patches."""
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
pin = json.loads((root / "sources.lock.json").read_text())["codex"]
repo = root / "vendor/codex"
if not repo.exists():
    repo.parent.mkdir(exist_ok=True)
    subprocess.run(["git", "clone", "--depth", "1", "--branch", pin["tag"],
                    pin["repository"], str(repo)], check=True)
actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
if actual != pin["commit"]:
    raise SystemExit(f"Refusing unexpected Codex revision {actual}; expected {pin['commit']}")
for patch in sorted((root / "patches/codex").glob("*.patch")):
    # Do not reset an existing checkout or overwrite unrelated user edits.
    applied = subprocess.run(["git", "apply", "--reverse", "--check", str(patch)],
                             cwd=repo, capture_output=True).returncode == 0
    if applied:
        print(f"Already applied: {patch.name}")
        continue
    subprocess.run(["git", "apply", "--check", str(patch)], cwd=repo, check=True)
    subprocess.run(["git", "apply", str(patch)], cwd=repo, check=True)
    print(f"Applied: {patch.name}")
