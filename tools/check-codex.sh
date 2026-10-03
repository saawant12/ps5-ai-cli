#!/usr/bin/env bash
set -euo pipefail
cd /work
mode="${1:-check}"
if [[ "$mode" != check && "$mode" != build && "$mode" != terminal && "$mode" != terminal-release ]]; then
  echo 'Usage: tools/check-codex.sh [check|build|terminal|terminal-release]' >&2
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
if [[ "$mode" != check ]]; then
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
cargo_mode="$mode"
cargo_extra=()
rustc_extra=()
profile=debug
if [[ "$mode" == terminal || "$mode" == terminal-release ]]; then
  export PS5_TERMINAL=1
  export PS5_BUILD_PROFILE=debug
  if [[ "$mode" == terminal-release ]]; then export PS5_BUILD_PROFILE=release; fi
  export PS5_NATIVE_DIR="/work/build/native-$PS5_BUILD_PROFILE"
  PS5_COMPILE_ONLY=1 tools/build-terminal.sh
  cargo_mode=rustc
  # External C sources are not Cargo inputs. A deterministic final-crate link
  # argument invalidates only the CLI artifact when those sources change.
  identity=$(sed -n 's/.*"\([0-9a-f]\{8\}\).*/\1/p' build/terminal-build-id.h)
  rustc_extra=(-- -C "link-arg=-Wl,--defsym=ps5_native_build=0x${identity}")
  if [[ "$mode" == terminal-release ]]; then
    profile=release
    cargo_extra=(--release)
    export CARGO_PROFILE_RELEASE_DEBUG=0 CARGO_PROFILE_RELEASE_PANIC=abort
    export CARGO_PROFILE_RELEASE_OPT_LEVEL=1 CARGO_PROFILE_RELEASE_LTO=false CARGO_PROFILE_RELEASE_CODEGEN_UNITS=16
  fi
fi
mkdir -p /work/locks/codex
if [ ! -f /work/locks/codex/Cargo.lock ]; then
  cp /work/vendor/codex/codex-rs/Cargo.lock /work/locks/codex/Cargo.lock
fi
cd vendor/codex/codex-rs
cargo "$cargo_mode" "${cargo_extra[@]}" --locked -Z lockfile-path --config 'resolver.lockfile-path="/work/locks/codex/Cargo.lock"' \
  --config 'patch.crates-io.mio.path="/work/vendor/mio-1.2.0"' \
  -j "${CARGO_BUILD_JOBS:-1}" \
  -p codex-cli --bin codex \
  --target /work/targets/x86_64-ps5-freebsd.json -Z json-target-spec \
  -Z build-std=std,panic_abort -Z build-std-features=panic-unwind "${rustc_extra[@]}"
if [[ "$mode" == build ]]; then
  cp /work/build/codex-target/x86_64-ps5-freebsd/debug/codex /work/build/codex-version-probe.elf
  llvm-strip --strip-all /work/build/codex-version-probe.elf
  llvm-readelf -h -l -d /work/build/codex-version-probe.elf > /work/build/codex-elf.txt
fi

if [[ "$mode" == terminal || "$mode" == terminal-release ]]; then
  cp "/work/build/codex-target/x86_64-ps5-freebsd/$profile/codex" /work/build/ps5-ai-cli.elf
  llvm-strip --strip-all /work/build/ps5-ai-cli.elf
  python3 /work/tools/payload.py /work/build/ps5-ai-cli.elf --max-size-mib 512
fi
