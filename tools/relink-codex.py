#!/usr/bin/env python3
"""Relink the last captured Rust objects after C-only ABI adapter changes.

Run a full Cargo build again after changing Rust sources, manifests or target
options. This helper deliberately does not report Cargo compilation success.
"""
import json
import argparse
import os
from pathlib import Path
import shutil
import subprocess

os.chdir("/work")
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--service", action="store_true", help="Link the authenticated app-server entry point")
options = parser.parse_args()
cc = "/opt/ps5-payload-sdk/bin/prospero-clang"
for source, output in (("platform/freebsd11.c", "build/ps5-compat.o"),
                       ("platform/syscalls.S", "build/ps5-syscalls.o"),
                       ("probes/codex-version-entry.c", "build/codex-version-entry.o")):
    defines = ["-DPS5_APP_SERVER"] if options.service and source.endswith("codex-version-entry.c") else []
    subprocess.run([cc, "-O2", "-Wall", "-Wextra", "-Werror", *defines, "-c", source, "-o", output], check=True)
args = json.loads(Path("build/codex-link.json").read_text())
subprocess.run(["/work/tools/ps5-rust-linker.sh", *args], check=True,
               env={**os.environ, "PS5_RELINK": "1"})
output = Path(args[args.index("-o") + 1])
artifact = Path("build/codex-service.elf" if options.service else "build/codex-version-probe.elf")
shutil.copy2(output, artifact)
subprocess.run(["llvm-strip", "--strip-all", str(artifact)], check=True)
with Path("build/codex-elf.txt").open("w") as report:
    subprocess.run(["llvm-readelf", "-h", "-l", "-d", str(artifact)], stdout=report, check=True)
print(f"Relinked {artifact}: {artifact.stat().st_size} bytes")
