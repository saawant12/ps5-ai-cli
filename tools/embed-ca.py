#!/usr/bin/env python3
"""Embed the pinned, public Mozilla trust bundle after verifying its checksum."""
import hashlib
import json
from pathlib import Path
root = Path(__file__).resolve().parents[1]
pin = json.loads((root/'sources.lock.json').read_text())['ca_bundle']
data = (root/'assets/cacert.pem').read_bytes()
if hashlib.sha256(data).hexdigest() != pin['sha256']:
    raise SystemExit('CA bundle differs from sources.lock.json')
if b'PRIVATE KEY' in data or data.count(b'-----BEGIN CERTIFICATE-----') != data.count(b'-----END CERTIFICATE-----'):
    raise SystemExit('Invalid public CA bundle')
(root/'build/ca-bundle.h').write_text('#define PS5_CA_NAME "mozilla-'+pin['sha256'][:16]+'.pem"\n'
    'static const unsigned char ps5_ca_bundle[] = {'+','.join(map(str,data))+'};\n')
