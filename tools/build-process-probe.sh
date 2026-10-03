#!/usr/bin/env bash
set -euo pipefail
cd /work
mkdir -p build
cc=/opt/ps5-payload-sdk/bin/prospero-clang
bash tools/build-process-helper.sh
mapfile -t syscall_flags < platform/syscalls.link
"$cc" -O2 -Wall -Wextra -Werror probes/process-runtime.c \
  platform/freebsd11.c platform/syscalls.S "${syscall_flags[@]}" \
  -Wl,--wrap=fcntl -Wl,--wrap=sysctl -lpthread -ldl -lunwind \
  -o build/ps5-process-runtime-probe.elf
llvm-readelf -h -l -d build/ps5-process-runtime-probe.elf > build/process-probe-elf.txt
python3 tools/payload.py build/ps5-process-runtime-probe.elf
