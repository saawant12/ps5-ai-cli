#!/usr/bin/env bash
set -euo pipefail
cd /work
python3 tools/fetch-loader.py --offline
bash tools/build-process-helper.sh
defines=()
rust_lib=()
artifact=build/ps5-loader-runtime-probe.elf
if [[ "${1:-}" == rust ]]; then
  export RUSTUP_TOOLCHAIN=1.95.0 RUSTC_BOOTSTRAP=1 RUST_LIBC_UNSTABLE_FREEBSD_VERSION=11
  export CARGO_TARGET_DIR=/work/build/rust-target CARGO_HOME=/work/.cache/cargo
  export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=clang HOST_CC=clang HOST_CXX=clang++
  export CC_x86_64_ps5_freebsd=/opt/ps5-payload-sdk/bin/prospero-clang
  export AR_x86_64_ps5_freebsd=/opt/ps5-payload-sdk/bin/prospero-ar
  python3 tools/prepare-rust-std.py
  python3 tools/prepare-mio.py
  cargo build --release --locked --offline -j1 --manifest-path probes/loader-rust/Cargo.toml \
    --config 'patch.crates-io.mio.path="/work/vendor/mio-1.2.0"' \
    --target /work/targets/x86_64-ps5-freebsd.json -Z json-target-spec \
    -Z build-std=std,panic_abort -Z build-std-features=panic-unwind
  defines=(-DPS5_LOADER_RUST)
  rust_lib=(build/rust-target/x86_64-ps5-freebsd/release/libps5_rust_loader_probe.a)
  artifact=build/ps5-rust-loader-runtime-probe.elf
elif [[ -n "${1:-}" ]]; then
  echo 'Usage: tools/build-loader-probe.sh [rust]' >&2
  exit 2
fi
cc=/opt/ps5-payload-sdk/bin/prospero-clang
"$cc" -O2 -Wall -Wextra -Werror "${defines[@]}" -c probes/loader-runtime.c -o build/loader-runtime.o
"$cc" -O2 -Wall -Werror -c vendor/shsrv/elfldr.c -o build/loader-elfldr.o
"$cc" -O2 -Wall -Werror -c vendor/shsrv/pt.c -o build/loader-pt.o
mapfile -t syscall_flags < platform/syscalls.link
"$cc" -O2 -Wall -Wextra -Werror build/loader-runtime.o build/loader-elfldr.o build/loader-pt.o \
  "${rust_lib[@]}" platform/freebsd11.c platform/syscalls.S "${syscall_flags[@]}" \
  -Wl,--wrap=fcntl -Wl,--wrap=sysctl -lpthread -ldl -lunwind -lkernel_sys \
  -o "$artifact"
llvm-readelf -h -l -d "$artifact" > "${artifact%.elf}-headers.txt"
python3 tools/payload.py "$artifact"
