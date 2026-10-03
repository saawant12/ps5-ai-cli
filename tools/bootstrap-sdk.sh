#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# OpenSSL/curl cross-build options adapted from Orbit's build-deps.sh.
set -euo pipefail
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
stage=$(mktemp -d /tmp/ps5-ai-cli-sdk.XXXXXXXX)
trap 'rm -rf "$stage"' EXIT
python3 - "$1" "$stage" <<'PY'
import hashlib
import json
from pathlib import Path
import subprocess
import sys

pins = json.loads(Path(sys.argv[1]).read_text())
stage = Path(sys.argv[2])
for name, filename in [('sdk', 'sdk.zip'), ('openssl', 'openssl.tar.gz'), ('curl', 'curl.tar.xz')]:
    pin = pins[name]
    path = stage / filename
    subprocess.run(['curl', '--silent', '--show-error', '--fail', '--location', '--retry', '3',
                    '--connect-timeout', '30', '--max-time', '600',
                    '--proto', '=https', '--tlsv1.2', pin['url'], '-o', str(path)], check=True)
    if hashlib.sha256(path.read_bytes()).hexdigest() != pin['sha256']:
        raise SystemExit(f'{name}: SHA-256 mismatch')
PY
unzip -q "$stage/sdk.zip" -d "$(dirname "$PS5_PAYLOAD_SDK")"
test -x "$PS5_PAYLOAD_SDK/bin/prospero-clang"
tar xf "$stage/openssl.tar.gz" -C "$stage"
tar xf "$stage/curl.tar.xz" -C "$stage"
source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
export CPPFLAGS="-I$PS5_SYSROOT/user/homebrew/include"
export LDFLAGS="-L$PS5_SYSROOT/user/homebrew/lib"
cd "$stage"/openssl-*
./Configure BSD-x86_64 no-tests no-apps no-shared --prefix="$PREFIX"
make -j "${SDK_BUILD_JOBS:-2}" build_sw
make install_sw DESTDIR="$PS5_SYSROOT"
cd "$stage"/curl-*
./configure --prefix="$PREFIX" --host=x86_64-pc-freebsd \
  --enable-static --disable-shared --with-openssl="$PS5_SYSROOT/user/homebrew" \
  --without-zlib --without-brotli --without-zstd --without-libpsl --without-libidn2 \
  --without-libssh2 --disable-ldap --disable-ldaps --disable-docs --disable-manual \
  --disable-ftp --disable-file --disable-dict --disable-gopher --disable-imap \
  --disable-mqtt --disable-pop3 --disable-rtsp --disable-smb --disable-smtp \
  --disable-telnet --disable-tftp
make -j "${SDK_BUILD_JOBS:-2}"
make install DESTDIR="$PS5_SYSROOT"
