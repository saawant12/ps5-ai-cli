#!/usr/bin/env python3
"""Preserve rustc's final CLI link inputs for fast C ABI adapter iterations."""
import hashlib
import json
from pathlib import Path
import shutil
import sys

destination = Path("/work/build/link-objects")
destination.mkdir(parents=True, exist_ok=True)
args = sys.argv[1:]
for index, arg in enumerate(args):
    path = Path(arg)
    if arg.endswith(".o") and path.is_file():
        name = hashlib.sha256(arg.encode()).hexdigest()[:16] + "-" + path.name
        preserved = destination / name
        shutil.copy2(path, preserved)
        args[index] = str(preserved)
Path("/work/build/codex-link.json").write_text(json.dumps(args, indent=2) + "\n")
