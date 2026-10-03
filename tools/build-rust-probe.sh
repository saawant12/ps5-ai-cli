#!/usr/bin/env bash
set -euo pipefail
cd /work
mkdir -p build
export RUSTC_BOOTSTRAP=1
export RUST_LIBC_UNSTABLE_FREEBSD_VERSION=11
export CARGO_TARGET_DIR=/work/build/rust-target
export CARGO_HOME=/work/.cache/cargo
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=clang
export HOST_CC=clang
export HOST_CXX=clang++
export CC_x86_64_ps5_freebsd=/opt/ps5-payload-sdk/bin/prospero-clang
export AR_x86_64_ps5_freebsd=/opt/ps5-payload-sdk/bin/prospero-ar
python3 tools/prepare-rust-std.py
python3 tools/prepare-mio.py
cargo build --release --manifest-path probes/rust-runtime/Cargo.toml \
  --config 'patch.crates-io.mio.path="/work/vendor/mio-1.2.0"' \
  --target /work/targets/x86_64-ps5-freebsd.json -Z json-target-spec \
  -Z build-std=std,panic_abort -Z build-std-features=panic-unwind
mapfile -t syscall_flags < /work/platform/syscalls.link
/opt/ps5-payload-sdk/bin/prospero-clang -O2 -Wall -Wextra -Werror \
  probes/rust-entry.c platform/freebsd11.c platform/syscalls.S "${syscall_flags[@]}" \
  build/rust-target/x86_64-ps5-freebsd/release/libps5_rust_runtime_probe.a \
  -Wl,--gc-sections -Wl,--wrap=fcntl -Wl,--wrap=sysctl -lpthread -ldl -lunwind -o build/ps5-rust-runtime-probe.elf
llvm-readelf -h -l -d build/ps5-rust-runtime-probe.elf > build/rust-probe-elf.txt
