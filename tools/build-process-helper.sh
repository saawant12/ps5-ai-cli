#!/usr/bin/env bash
set -euo pipefail
cd /work
mkdir -p build
/opt/ps5-payload-sdk/bin/prospero-clang -O2 -Wall -Wextra -Werror \
  probes/process-helper.c -o build/process-helper.elf
python3 - <<'PY'
from pathlib import Path
data = Path('build/process-helper.elf').read_bytes()
rows = [','.join(f'0x{x:02x}' for x in data[i:i+16]) for i in range(0, len(data), 16)]
Path('build/process-helper.h').write_text('static const unsigned char ps5_process_helper[] = {\n' + ',\n'.join(rows) + '\n};\n')
PY
