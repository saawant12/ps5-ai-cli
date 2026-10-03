#!/usr/bin/env bash
set -euo pipefail
cd /work
mode="${1:-check}"
if [[ "$mode" != check && "$mode" != build ]]; then
  echo 'Usage: tools/check-codex.sh [check|build]' >&2
  exit 2
fi
export RUSTUP_TOOLCHAIN=1.95.0
export RUSTC_BOOTSTRAP=1
export RUST_LIBC_UNSTABLE_FREEBSD_VERSION=11
export CARGO_HOME=/work/.cache/cargo
export CARGO_TARGET_DIR=/work/build/codex-target
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=clang
export HOST_CC=clang HOST_CXX=clang++
export CC_x86_64_ps5_freebsd=/opt/ps5-payload-sdk/bin/prospero-clang
export CXX_x86_64_ps5_freebsd=/opt/ps5-payload-sdk/bin/prospero-clang++
export AR_x86_64_ps5_freebsd=/opt/ps5-payload-sdk/bin/prospero-ar
# cc-rs infers an invalid LLVM triple from the custom Rust environment.
# Its explicit CFLAGS follow that inferred triple; keep the native SDK target.
export CFLAGS_x86_64_ps5_freebsd='--target=x86_64-sie-ps5 -mno-red-zone -femulated-tls -include /work/platform/compat.h'
export CXXFLAGS_x86_64_ps5_freebsd="$CFLAGS_x86_64_ps5_freebsd"
export X86_64_PS5_FREEBSD_OPENSSL_DIR=/opt/ps5-payload-sdk/target/user/homebrew
export X86_64_PS5_FREEBSD_OPENSSL_STATIC=1
python3 tools/prepare-rust-std.py
python3 tools/prepare-mio.py
if [[ "$mode" == build ]]; then
  mkdir -p /work/build
  /opt/ps5-payload-sdk/bin/prospero-clang -O2 -Wall -Wextra -Werror -c \
    /work/platform/freebsd11.c -o /work/build/ps5-compat.o
  /opt/ps5-payload-sdk/bin/prospero-clang -c /work/platform/syscalls.S -o /work/build/ps5-syscalls.o
  /opt/ps5-payload-sdk/bin/prospero-clang -O2 -Wall -Wextra -Werror -c \
    /work/probes/codex-version-entry.c -o /work/build/codex-version-entry.o
  export CARGO_TARGET_X86_64_PS5_FREEBSD_LINKER=/work/tools/ps5-rust-linker.sh
  export CARGO_PROFILE_DEV_DEBUG=0
  export CARGO_PROFILE_DEV_PANIC=abort
fi
mkdir -p /work/locks/codex
if [ ! -f /work/locks/codex/Cargo.lock ]; then
  cp /work/vendor/codex/codex-rs/Cargo.lock /work/locks/codex/Cargo.lock
fi
cd vendor/codex/codex-rs
cargo "$mode" -Z lockfile-path --config 'resolver.lockfile-path="/work/locks/codex/Cargo.lock"' \
  --config 'patch.crates-io.mio.path="/work/vendor/mio-1.2.0"' \
  -j "${CARGO_BUILD_JOBS:-1}" \
  -p codex-cli --bin codex \
  --target /work/targets/x86_64-ps5-freebsd.json -Z json-target-spec \
  -Z build-std=std,panic_abort -Z build-std-features=panic-unwind
if [[ "$mode" == build ]]; then
  cp /work/build/codex-target/x86_64-ps5-freebsd/debug/codex /work/build/codex-version-probe.elf
  llvm-strip --strip-all /work/build/codex-version-probe.elf
  llvm-readelf -h -l -d /work/build/codex-version-probe.elf > /work/build/codex-elf.txt
fi
