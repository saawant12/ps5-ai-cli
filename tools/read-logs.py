#!/usr/bin/env python3
"""Launch the installed, exact log-reader build once, then retrieve project logs."""
import argparse
import hashlib
import json
from pathlib import Path
import time
import urllib.error
import urllib.parse
import urllib.request

from payload import manager_request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--manager", required=True)
parser.add_argument("--launch", action="store_true", help="Deliberately launch the installed collector once")
parser.add_argument("--output", type=Path, default=Path("build/ps5-latest-log.txt"))
args = parser.parse_args()
parsed = urllib.parse.urlsplit(args.manager)
if (parsed.scheme != "http" or not parsed.hostname or parsed.username or parsed.password
        or parsed.path not in ("", "/") or parsed.query or parsed.fragment):
    parser.error("Expected a plain Payload Manager HTTP base URL")
base = args.manager.rstrip("/")
if args.launch:
    artifact = Path(__file__).resolve().parents[1] / "build/ps5-read-probe-logs.elf"
    digest = hashlib.sha256(artifact.read_bytes()).hexdigest()[:12]
    name = f"{artifact.stem}-{digest}.elf"
    inventory = json.loads(manager_request(base, "/list_payloads"))["payloads"]
    paths = [path for path in inventory if Path(path).name == name]
    if len(paths) != 1:
        raise SystemExit("Upload the current log-reader ELF before using --launch")
    # No automatic launch retry, even if the request times out.
    result = manager_request(base, "/loadpayload:" + urllib.parse.quote(paths[0], safe="/"))
    if result.strip() != b"OK":
        raise SystemExit(f"Unexpected launch response: {result[:200]!r}")
    print("Log collector launch accepted", flush=True)
deadline = time.monotonic() + 12
while True:
    try:
        with urllib.request.urlopen(f"http://{parsed.hostname}:19061/logs", timeout=3) as response:
            data = response.read(16 * (65536 + 256) + 1024)
        break
    except urllib.error.URLError:
        if time.monotonic() >= deadline:
            raise
        time.sleep(0.25)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_bytes(data)
print(data.decode(errors="replace"))
