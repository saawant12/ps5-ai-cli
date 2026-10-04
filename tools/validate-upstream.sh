#!/usr/bin/env bash
set -euo pipefail
cd /work
export RUSTUP_TOOLCHAIN=1.95.0 RUSTC_BOOTSTRAP=1
export CARGO_HOME=/work/.cache/cargo
export CARGO_TARGET_DIR=/work/build/host-validation
export CARGO_PROFILE_DEV_DEBUG=0 CARGO_BUILD_JOBS=1
export CC=clang CXX=clang++
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=clang
export CARGO_UNSTABLE_LOCKFILE_PATH=true
export CARGO_RESOLVER_LOCKFILE_PATH=/work/locks/host/Cargo.lock
mkdir -p locks/host
if [[ ! -f locks/host/Cargo.lock ]]; then
  cp locks/codex/Cargo.lock locks/host/Cargo.lock
fi
git config --global --add safe.directory /work/vendor/codex
case "${1:-test}" in
  test)
    cd vendor/codex/codex-rs
    just test -p codex-keyring-store -p codex-rmcp-client --locked
    ;;
  test-remote)
    cd vendor/codex/codex-rs
    cargo build -p codex-cli --bin codex --locked
    just test -p codex-rmcp-client --test streamable_http_remote --locked
    ;;
  test-process)
    cd vendor/codex/codex-rs
    just test -p codex-utils-pty --locked
    ;;
  fix-process)
    cd vendor/codex/codex-rs
    just fix -p codex-utils-pty --locked
    ;;
  test-shell)
    cd vendor/codex/codex-rs
    just test -p codex-shell-command --locked
    ;;
  fix-shell)
    cd vendor/codex/codex-rs
    just fix -p codex-shell-command --locked
    ;;
  test-models)
    cd vendor/codex/codex-rs
    just test -p codex-models-manager --locked
    ;;
  fix-models)
    cd vendor/codex/codex-rs
    just fix -p codex-models-manager --locked
    ;;
  fmt)
    cd vendor/codex/codex-rs
    just fmt
    ;;
  lock)
    cd vendor/codex
    just bazel-lock-update
    ;;
  *) echo 'Usage: tools/validate-upstream.sh [test|test-remote|test-process|fix-process|test-shell|fix-shell|test-models|fix-models|fmt|lock]' >&2; exit 2 ;;
esac
