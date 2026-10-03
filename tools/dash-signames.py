#!/usr/bin/env python3
"""Run Dash's host generator using PS5 signal numbers, not host signal numbers."""
from pathlib import Path
import re
import subprocess

root = Path('/work')
target = root / 'build/dash/src'
macros = subprocess.check_output([
    '/work/tools/ps5-autoconf-cc.sh', '-dM', '-E', '-include', 'signal.h', '-'],
    input='', text=True)
signals = [line for line in macros.splitlines()
           if re.match(r'#define (?:SIG[A-Z0-9]+|NSIG) ', line)]
if not any(line.startswith('#define NSIG ') for line in signals):
    raise SystemExit('Missing target NSIG')
(target / 'ps5-signals.h').write_text('\n'.join(signals) + '\n')
source = (root / 'vendor/dash/src/mksignames.c').read_text()
before = '\n#include <signal.h>\n'
if source.count(before) != 1:
    raise SystemExit('Dash signal generator context changed')
(target / 'ps5-mksignames.c').write_text(source.replace(before, '\n#include "ps5-signals.h"\n'))
# FreeBSD exposes realtime signal constants beyond its traditional NSIG range;
# Dash's generator explicitly supports excluding those entries.
subprocess.run(['clang', '-DUNUSABLE_RT_SIGNALS=1', '-Wno-deprecated-non-prototype', str(target / 'ps5-mksignames.c'),
                '-o', str(target / 'mksignames')], check=True)
subprocess.run([str(target / 'mksignames')], cwd=target, check=True)
