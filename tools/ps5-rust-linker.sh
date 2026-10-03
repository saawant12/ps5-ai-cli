#!/usr/bin/env bash
set -euo pipefail
if [[ "${PS5_RELINK:-0}" != 1 ]]; then
  python3 /work/tools/capture-link.py "$@"
fi
# FreeBSD splits these symbols into separate libraries. The PS5 SDK places
# the implemented functions in libc and native modules. Missing functions
# still cause a link error; no unresolved-symbol suppression is used.
args=()
for arg in "$@"; do
  case "$arg" in
    -lm|-lrt|-lutil|-lexecinfo|-lkvm|-lmemstat|-lprocstat|-ldevstat) args+=(-lc) ;;
    -lgcc_s) args+=(-lunwind) ;;
    *) args+=("$arg") ;;
  esac
done
mapfile -t syscall_flags < /work/platform/syscalls.link
# rustc passes -nodefaultlibs, so name the native SDK imports explicitly.
exec /opt/ps5-payload-sdk/bin/prospero-clang "${args[@]}" /work/build/ps5-compat.o \
  /work/build/ps5-syscalls.o "${syscall_flags[@]}" \
  /work/build/codex-version-entry.o -Wl,--wrap=main -Wl,--wrap=fcntl -Wl,--wrap=sysctl \
  -Wl,--error-limit=0 \
  -ldl -lunwind -lc -lkernel_web -lSceLibcInternal -lSceNet
