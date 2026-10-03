"""Verify bundled runtime installation and helper entry isolation on Linux."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

INSTALL_DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
int ps5_install_runtime_tools(void);
int ps5_configure_runtime_tools(void);
int main(void) {
    if (ps5_configure_runtime_tools() || ps5_install_runtime_tools()) {
        perror("runtime tools"); return 1;
    }
    printf("%s\n%s\n", getenv("SHELL"), getenv("PATH"));
    return 0;
}
'''

ENTRY_DRIVER = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gateway.h"
static int saved_argc;
static char **saved_argv;
static char saved_cwd[4096];
int __wrap_main(int, char **);
int __real_main(int argc, char **argv) {
    char cwd[4096];
    if (argc != saved_argc || argv != saved_argv || !getcwd(cwd, sizeof(cwd)) ||
        strcmp(cwd, saved_cwd) || strcmp(getenv("SHELL"), "preserve-child-shell")) abort();
    return 37;
}
int ps5_set_executable_path(const char *path) {
    if (strcmp(path, "/data/ps5-ai-cli/runtime/codex.elf")) abort();
    return getenv("FAIL_EXECUTABLE_PATH") ? -1 : 0;
}
int ps5_install_runtime_image(void) { abort(); }
int ps5_install_runtime_tools(void) { abort(); }
int ps5_configure_runtime_tools(void) { abort(); }
int ps5_install_trust_store(void) { abort(); }
int ps5_terminal_prepare(void) { abort(); }
int ps5_terminal_attach(unsigned a, unsigned b) { (void)a; (void)b; abort(); }
void ps5_terminal_resize(unsigned a, unsigned b) { (void)a; (void)b; abort(); }
void ps5_terminal_detach(void) { abort(); }
int ps5_terminal_wait(void) { abort(); }
int ps5_ui_start(const struct ui_config *config) { (void)config; abort(); }
int ps5_ai_launcher_ensure(int fd) { (void)fd; abort(); }
const char *ps5_ai_launcher_registration_method(void) { abort(); }
const char *ps5_ai_launcher_last_error(void) { abort(); }
uint32_t arc4random_uniform(uint32_t bound) { (void)bound; abort(); }
struct notification;
int sceKernelSendNotificationRequest(int a, struct notification *b, size_t c, int d) {
    (void)a; (void)b; (void)c; (void)d; abort();
}
int main(int argc, char **argv) {
    saved_argc = argc - 1; saved_argv = argv + 1;
    if (!getcwd(saved_cwd, sizeof(saved_cwd)) || setenv("SHELL", "preserve-child-shell", 1)) return 1;
    return __wrap_main(saved_argc, saved_argv);
}
'''


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('clang'), 'Linux clang required')
class RuntimeBundleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[1]
        if not (cls.root / 'build/runtime-tools.h').exists():
            raise unittest.SkipTest('build and embed the native runtime first')
        cls.temporary = tempfile.TemporaryDirectory()
        build = Path(cls.temporary.name)
        (build / 'install.c').write_text(INSTALL_DRIVER)
        (build / 'entry.c').write_text(ENTRY_DRIVER)
        cls.installer, cls.entry = build / 'install', build / 'entry'
        subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-DPS5_AI_STATE="state"',
                        str(build / 'install.c'), str(cls.root / 'app/runtime-tools.c'),
                        str(cls.root / 'platform/sdk-elf.c'), '-o', str(cls.installer)], check=True)
        subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-I'+str(cls.root / 'app'),
                        str(build / 'entry.c'), str(cls.root / 'app/entry.c'),
                        '-o', str(cls.entry)], check=True)
        identity = re.search(r'PS5_RUNTIME_ID "([0-9a-f]+)"',
                             (cls.root / 'build/runtime-tools.h').read_text())[1]
        cls.folder = 'tools-' + identity

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.temporary_case = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary_case.name)
        (self.directory / 'state').mkdir()
        self.runtime = self.directory / 'state/runtime' / self.folder

    def tearDown(self):
        self.temporary_case.cleanup()

    def install(self, success=True):
        result = subprocess.run([str(self.installer)], cwd=self.directory, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0 if success else 1, result.stderr.decode())
        if success:
            self.assertEqual(result.stdout.decode().splitlines(),
                             [f'state/runtime/{self.folder}/sh', f'state/runtime/{self.folder}'])

    def test_install_all_assets_and_repeat_without_rewriting(self):
        self.install()
        names = ['sh', 'sbase-box', *(self.root / 'platform/runtime-tools.txt').read_text().splitlines()]
        for name in names:
            expected = self.root / ('build/shell/sh.elf' if name == 'sh' else 'build/shell/sbase-box.elf')
            self.assertEqual((self.runtime / name).read_bytes(), expected.read_bytes())
            self.assertTrue((self.runtime / name).stat().st_mode & 0o100)
        before = {name: (self.runtime / name).stat().st_mtime_ns for name in names}
        self.install()
        self.assertEqual(before, {name: (self.runtime / name).stat().st_mtime_ns for name in names})

    def test_owned_partial_install_completes_and_keeps_other_files(self):
        self.install()
        (self.runtime / 'cat').unlink()
        (self.runtime / 'notes').write_bytes(b'preserve')
        self.install()
        self.assertTrue((self.runtime / 'cat').is_file())
        self.assertEqual((self.runtime / 'notes').read_bytes(), b'preserve')

    def test_foreign_folder_and_modified_tool_are_preserved(self):
        self.runtime.mkdir(parents=True)
        (self.runtime / 'sh').write_bytes(b'foreign')
        self.install(success=False)
        self.assertEqual(list(self.runtime.iterdir()), [self.runtime / 'sh'])
        (self.runtime / 'sh').unlink()
        self.runtime.rmdir()
        self.install()
        (self.runtime / 'cat').write_bytes(b'foreign')
        self.install(success=False)
        self.assertEqual((self.runtime / 'cat').read_bytes(), b'foreign')

    def test_symlink_runtime_is_rejected(self):
        outside = self.directory / 'outside'
        outside.mkdir()
        (self.directory / 'state/runtime').symlink_to(outside)
        self.install(success=False)
        self.assertEqual(list(outside.iterdir()), [])

    def test_child_entries_preserve_context_without_gateway(self):
        entries = [
            ['/data/ps5-ai-cli/runtime/codex.elf', '--version'],
            ['apply_patch', 'patch text'], ['applypatch', 'patch text'],
            ['codex-execve-wrapper', 'sh', '-c', 'true'],
            ['codex', '--codex-run-as-apply-patch', 'patch text'],
            ['codex', '--codex-run-as-fs-helper'],
            ['codex', '--codex-run-as-arg0-exec-helper'],
        ]
        for args in entries:
            with self.subTest(args=args):
                result = subprocess.run([str(self.entry), *args], cwd=self.directory, timeout=5)
                self.assertEqual(result.returncode, 37)
        self.assertEqual(list((self.directory / 'state').iterdir()), [])


if __name__ == '__main__':
    unittest.main()
