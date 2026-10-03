#!/usr/bin/env bash
set -euo pipefail
cd /work
export PS5_NATIVE_DIR="${PS5_NATIVE_DIR:-/work/build/native-${PS5_BUILD_PROFILE:-debug}}"
mkdir -p "$PS5_NATIVE_DIR"
export PS5_BUILD_ORIGIN=cargo
if [[ "${PS5_COMPILE_ONLY:-0}" != 1 ]]; then export PS5_BUILD_ORIGIN=captured; fi
python3 tools/embed-ui.py
python3 tools/embed-launcher.py
python3 tools/embed-ca.py
PS5_TOOL_TRACE=0 bash tools/build-shell.sh
python3 tools/embed-runtime.py
python3 tools/terminal-build-id.py
cc=/opt/ps5-payload-sdk/bin/prospero-clang
includes=(-I/opt/ps5-payload-sdk/target/user/homebrew/include -Ibuild -Ilauncher)
for name in gateway http websocket native-terminal runtime-image runtime-tools trust-store; do
  "$cc" -O2 -Wall -Wextra -Werror "${includes[@]}" -c "app/$name.c" -o "$PS5_NATIVE_DIR/terminal-$name.o"
done
for name in sdk-elf sdk-spawn; do
  "$cc" -O2 -Wall -Wextra -Werror -c "platform/$name.c" -o "$PS5_NATIVE_DIR/runtime-$name.o"
done
for name in elfldr pt; do
  "$cc" -O2 -Wall -Werror -c "vendor/shsrv/$name.c" -o "$PS5_NATIVE_DIR/runtime-$name.o"
done
for name in install platform_ps5; do
  "$cc" -O2 -Wall -Wextra -Werror "${includes[@]}" -c "launcher/$name.c" -o "$PS5_NATIVE_DIR/launcher-$name.o"
done
defines=()
if [[ "${PS5_DEV_PAIRING:-0}" == 1 ]]; then defines=(-DPS5_DEV_PAIRING); fi
"$cc" -O2 -Wall -Wextra -Werror "${includes[@]}" "${defines[@]}" -c app/entry.c -o "$PS5_NATIVE_DIR/terminal-entry.o"
"$cc" -O2 -Wall -Wextra -Werror -c platform/freebsd11.c -o "$PS5_NATIVE_DIR/ps5-compat.o"
"$cc" -c platform/syscalls.S -o "$PS5_NATIVE_DIR/ps5-syscalls.o"
if [[ "${PS5_COMPILE_ONLY:-0}" == 1 ]]; then exit 0; fi
# The captured Rust objects remain a development shortcut. A fresh Cargo
# terminal build is required before claiming reproducible release packaging.
python3 - <<'PY'
import json, os, subprocess
from pathlib import Path
args = json.loads(Path('build/codex-link.json').read_text())
args[args.index('-o') + 1] = '/work/build/ps5-ai-cli.elf'
subprocess.run(['/work/tools/ps5-rust-linker.sh', *args], check=True,
               env={**os.environ, 'PS5_RELINK': '1', 'PS5_TERMINAL': '1'})
subprocess.run(['llvm-strip', '--strip-all', 'build/ps5-ai-cli.elf'], check=True)
subprocess.run(['python3', 'tools/payload.py', 'build/ps5-ai-cli.elf', '--max-size-mib', '512'], check=True)
PY
