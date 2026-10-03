#!/usr/bin/env bash
set -euo pipefail
cd /work
mkdir -p build
# Supply a PEM file explicitly; this diagnostic does not fetch or disable trust.
test -s build/network-probe-ca.pem || { echo 'Provide build/network-probe-ca.pem' >&2; exit 1; }
python3 - <<'PY'
from pathlib import Path
pem = Path('build/network-probe-ca.pem').read_bytes()
if len(pem) > 1024 * 1024 or b'-----BEGIN CERTIFICATE-----' not in pem:
    raise SystemExit('Expected a CA PEM bundle of at most 1 MiB')
Path('build/network-probe-ca.h').write_text(
    'static const unsigned char network_probe_ca[] = {' + ','.join(map(str, pem)) + '};\n')
PY
mapfile -t syscall_flags < platform/syscalls.link
defines=()
if [[ "${PS5_PROBE_RESOLVE:-0}" == 1 ]]; then
  test -s build/network-probe-dns.h
  defines+=(-DPS5_PROBE_RESOLVE)
fi
/opt/ps5-payload-sdk/bin/prospero-clang -O2 -Wall -Wextra -Werror \
  "${defines[@]}" \
  -I/opt/ps5-payload-sdk/target/user/homebrew/include \
  -L/opt/ps5-payload-sdk/target/user/homebrew/lib \
  probes/network-runtime.c platform/freebsd11.c platform/syscalls.S "${syscall_flags[@]}" \
  -Wl,--wrap=fcntl -Wl,--wrap=sysctl -lcurl -lssl -lcrypto -lpthread -ldl -lunwind \
  -o build/ps5-network-runtime-probe.elf
python3 tools/payload.py build/ps5-network-runtime-probe.elf
