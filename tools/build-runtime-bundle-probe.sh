#!/usr/bin/env bash
set -euo pipefail
cd /work
python3 tools/embed-runtime.py
mapfile -t wrappers < platform/syscalls.link
/opt/ps5-payload-sdk/bin/prospero-clang -O2 -Wall -Wextra -Werror \
  probes/runtime-bundle.c app/runtime-tools.c platform/sdk-elf.c \
  platform/freebsd11.c platform/syscalls.S "${wrappers[@]}" \
  -Wl,--wrap=fcntl -Wl,--wrap=sysctl -lpthread -ldl -lunwind -lkernel_sys \
  -o build/ps5-runtime-bundle-probe.elf
python3 tools/payload.py build/ps5-runtime-bundle-probe.elf
