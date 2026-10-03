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
entry=(/work/build/codex-version-entry.o)
extra=()
native=/work/build
if [[ "${PS5_TERMINAL:-0}" == 1 ]]; then
  native="${PS5_NATIVE_DIR:-/work/build}"
  entry=("$native/terminal-entry.o")
  for name in gateway http websocket native-terminal runtime-image runtime-tools trust-store; do
    extra+=("$native/terminal-$name.o")
  done
  extra+=("$native/launcher-install.o" "$native/launcher-platform_ps5.o"
    -Wl,--wrap=isatty -Wl,--wrap=tcgetattr -Wl,--wrap=tcsetattr -Wl,--wrap=tcflush -Wl,--wrap=ioctl
    -L/opt/ps5-payload-sdk/target/user/homebrew/lib -lcrypto
    -Wl,--push-state,--no-as-needed -lSceIpmi -Wl,--pop-state -lSceAppInstUtil)
fi
for name in sdk-elf sdk-spawn elfldr pt; do
  extra+=("$native/runtime-$name.o")
done
# rustc passes -nodefaultlibs, so name the native SDK imports explicitly.
exec /opt/ps5-payload-sdk/bin/prospero-clang "${args[@]}" "$native/ps5-compat.o" \
  "$native/ps5-syscalls.o" "${syscall_flags[@]}" \
  "${entry[@]}" "${extra[@]}" -Wl,--wrap=main -Wl,--wrap=fcntl -Wl,--wrap=sysctl \
  -Wl,--error-limit=0 \
  -ldl -lunwind -lc -lkernel_web -lSceLibcInternal -lSceNet
