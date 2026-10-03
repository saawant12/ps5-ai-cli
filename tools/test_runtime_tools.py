"""Behavior checks for the PS5 stdio and in-memory script adaptations."""
import os
import sys
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('clang') and shutil.which('make'),
                     'run with clang and make in the Linux validation container')
class RuntimeToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        source = root / 'vendor/sbase'
        if not source.exists():
            raise unittest.SkipTest('fetch the pinned shell sources first')
        cls.temporary = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temporary.name)
        paths = subprocess.check_output(['git', 'ls-files', '-z'], cwd=source).split(b'\0')
        for entry in paths:
            if not entry:
                continue
            relative = Path(os.fsdecode(entry))
            target = cls.directory / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / relative, target)
        adapter = cls.directory / 'stdio-adapter.o'
        subprocess.run(['clang', '-O2', '-Dgetline=ps5_test_getline', '-c',
                        str(root / 'platform/tool-stdio.c'), '-o', str(adapter)], check=True)
        build = subprocess.run(['make', '-j2', 'CC=clang',
                                'CFLAGS=-O2 -Dgetline=ps5_test_getline',
                                f'LDFLAGS={adapter}', 'grep', 'sed'], cwd=cls.directory,
                               capture_output=True)
        if build.returncode:
            raise RuntimeError(build.stderr.decode(errors='replace'))

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_tool(self, name, args, data, expected, status=0):
        result = subprocess.run([str(self.directory / name), *args], input=data,
                                capture_output=True, cwd=self.directory, timeout=5)
        self.assertEqual((result.returncode, result.stdout, result.stderr),
                         (status, expected, b''))

    def test_grep_multiple_patterns_and_blank_pattern(self):
        self.run_tool('grep', ['-e', '^alpha$\n^gamma$'], b'alpha\nbeta\ngamma\n', b'alpha\ngamma\n')
        self.run_tool('grep', ['-e', ''], b'alpha\nbeta\n', b'alpha\nbeta\n')
        self.run_tool('grep', ['-e', '^absent$'], b'alpha\n', b'', status=1)

    def test_grep_long_line_and_eof_without_newline(self):
        line = b'a' * 8192 + b'needle'
        self.run_tool('grep', ['needle'], line, line + b'\n')
        self.run_tool('grep', ['needle'], b'', b'', status=1)

    def test_grep_pattern_file_matches_command_pattern(self):
        (self.directory / 'patterns').write_bytes(b'^alpha$\n^gamma$')
        self.run_tool('grep', ['-f', 'patterns'], b'alpha\nbeta\ngamma', b'alpha\ngamma\n')

    def test_sed_multiline_script_and_empty_script(self):
        data = b'alpha\nbeta\n'
        self.run_tool('sed', ['s/alpha/first/\ns/beta/second/'], data, b'first\nsecond\n')
        self.run_tool('sed', [''], data, data)
        self.run_tool('sed', ['s/beta/gamma/'], b'beta', b'gamma\n')

    def test_sed_continuation_and_script_file(self):
        script = 'a\\\nfirst\\\nsecond\n'
        self.run_tool('sed', [script], b'alpha\n', b'alpha\nfirst\nsecond\n')
        (self.directory / 'script.sed').write_text(script)
        self.run_tool('sed', ['-f', 'script.sed'], b'alpha\n', b'alpha\nfirst\nsecond\n')

    def test_stdio_long_binary_line(self):
        # An embedded NUL must not truncate getline's consumed byte count.
        # Count mode avoids grep's upstream string-oriented matching/output.
        self.run_tool('grep', ['-c', ''], b'a\0b\n' + b'x' * 16384 + b'\n', b'2\n')


if __name__ == '__main__':
    unittest.main()
