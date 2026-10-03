#!/usr/bin/env python3
"""Prepare a local capability credential; embed only its SHA-256 in the ELF."""
import hashlib
import os
from pathlib import Path
import secrets

root = Path(__file__).resolve().parents[1]
secret = root / ".cache/session-token"
secret.parent.mkdir(parents=True, exist_ok=True)
if not secret.exists():
    fd = os.open(secret, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as stream:
        stream.write(secrets.token_hex(32))
token = secret.read_text().strip()
if len(token) != 64 or any(c not in "0123456789abcdef" for c in token):
    raise SystemExit("Unexpected session credential format")
digest = hashlib.sha256(token.encode()).hexdigest()
header = root / "build/session-auth.h"
header.parent.mkdir(parents=True, exist_ok=True)
header.write_text(f'#define PS5_SESSION_TOKEN_SHA256 "{digest}"\n')
print("Session credential prepared; only its hash is embedded in the ELF.")
