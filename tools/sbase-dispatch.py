#!/usr/bin/env python3
"""Generate a selected sbase dispatcher; runtime aliases are regular files."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
names = (root / 'platform/runtime-tools.txt').read_text().splitlines()
if len(set(names)) != len(names) or any(not name.isalnum() for name in names):
    raise SystemExit('Invalid runtime tool list')
declarations = '\n'.join(f'int sbase_{name}_main(int, char **);' for name in names)
entries = '\n'.join(f'    {{"{name}", sbase_{name}_main}},' for name in names)
code = '''/* Generated from the pinned, selected sbase applets. */
#include <stdio.h>
#include <string.h>
DECLARATIONS
int main(int argc, char **argv) {
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "sbase-box") && argc > 1) { argc--; argv++; name = argv[0]; }
    static const struct { const char *name; int (*main)(int, char **); } tools[] = {
ENTRIES
    };
    for (unsigned i = 0; i < sizeof(tools) / sizeof(*tools); i++)
        if (!strcmp(name, tools[i].name)) return tools[i].main(argc, argv);
    fprintf(stderr, "Unsupported bundled tool: %s\\n", name);
    return 127;
}
'''
(root / 'build/shell/sbase-box.c').write_text(
    code.replace('DECLARATIONS', declarations).replace('ENTRIES', entries))
