#!/usr/bin/env python3
"""Create a checksum-verified, locally patched Mio checkout for the PS5 target."""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tomllib
import urllib.request

root = Path(__file__).resolve().parents[1]
lock = tomllib.loads((root / "vendor/codex/codex-rs/Cargo.lock").read_text())
package = next(p for p in lock["package"] if p["name"] == "mio")
version = package["version"]
if version != "1.2.0":
    raise SystemExit(f"Review the Mio patch for new version {version}")
checkout = root / "vendor" / f"mio-{version}"
if not checkout.exists():
    cached = list((root / ".cache/cargo/registry/cache").glob(f"*/mio-{version}.crate"))
    archive = cached[0] if cached else root / ".cache" / f"mio-{version}.crate"
    if not archive.exists():
        archive.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(f"https://static.crates.io/crates/mio/mio-{version}.crate", timeout=30) as response:
            archive.write_bytes(response.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != package["checksum"]:
        raise SystemExit("Mio archive checksum mismatch")
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            path = Path(member.name)
            if (path.is_absolute() or ".." in path.parts or
                    path.parts[0] != f"mio-{version}" or not (member.isfile() or member.isdir())):
                raise SystemExit("Unexpected path or file type in Mio archive")
        tar.extractall(root / "vendor", filter="data")
patch = root / "patches/mio/0001-ps5-share-kqueue.patch"
source = checkout / "src/sys/unix/selector/kqueue.rs"
marker = checkout / ".ps5-patch.json"
fingerprint = {"patch": hashlib.sha256(patch.read_bytes()).hexdigest(),
               "source": hashlib.sha256(source.read_bytes()).hexdigest()}
if marker.exists() and json.loads(marker.read_text()) == fingerprint:
    print(f"PS5 Mio verified: {checkout}")
    raise SystemExit(0)
applied = subprocess.run(["git", "apply", "--reverse", "--check", str(patch)],
                         cwd=checkout, capture_output=True).returncode == 0
if not applied:
    subprocess.run(["git", "apply", "--check", str(patch)], cwd=checkout, check=True)
    subprocess.run(["git", "apply", str(patch)], cwd=checkout, check=True)
fingerprint["source"] = hashlib.sha256(source.read_bytes()).hexdigest()
marker.write_text(json.dumps(fingerprint) + "\n")
print(f"PS5 Mio ready: {checkout}")
