"""Exercise runtime installation against both observed Payload Manager layouts."""
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('clang'), 'run in the SDK image')
class RuntimeImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[1]
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / 'runtime-driver'
        subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-DPS5_AI_STATE="state"',
                        '-DPS5_AI_PAYLOAD_ROOT="payloads"', str(cls.root / 'tools/runtime/driver.c'),
                        str(cls.root / 'app/runtime-image.c'), '-o', str(cls.binary)], check=True)
        cls.build_id = re.search(r'"([0-9a-f]{32})"', (cls.root / 'build/terminal-build-id.h').read_text())[1].encode()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.case = tempfile.TemporaryDirectory()
        self.directory = Path(self.case.name)
        (self.directory / 'state').mkdir()
        (self.directory / 'payloads').mkdir()

    def tearDown(self):
        self.case.cleanup()

    def image(self, build_id=None):
        # A minimal ELF section table carrying the runtime's ownership note.
        header = struct.pack('<16sHHIQQQIHHHHHH', b'\x7fELF\x02\x01\x01', 3, 62, 1,
                             0, 0, 64, 0, 64, 56, 0, 64, 1, 0)
        section = struct.pack('<IIQQQQIIQQ', 0, 7, 0, 0, 128, 56, 0, 0, 4, 0)
        note = struct.pack('<III12s32s', 9, 32, 0x50533541, b'PS5AICLI', build_id or self.build_id)
        return header + section + note

    def install(self, success=True):
        result = subprocess.run([str(self.binary)], cwd=self.directory, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0 if success else 1, result.stderr.decode())

    def test_both_manager_directory_layouts_and_idempotence(self):
        for folder, filename in (('ps5-ai-cli', 'ps5-ai-cli-0123456789ab.elf'),
                                 ('ps5-ai-cli-0123456789ab', 'ps5-ai-cli-0123456789ab.elf'),
                                 ('ps5-ai-cli', 'ps5-ai-cli.elf')):
            with self.subTest(folder=folder, filename=filename):
                source = self.directory / 'payloads' / folder
                source.mkdir()
                (source / filename).write_bytes(self.image())
                self.install()
                runtime = self.directory / 'state/runtime/codex.elf'
                before = runtime.stat().st_mtime_ns
                self.install()
                self.assertEqual((runtime.read_bytes(), runtime.stat().st_mtime_ns), (self.image(), before))
                runtime.unlink()
                shutil.rmtree(source)

    def test_unknown_runtime_is_preserved(self):
        runtime = self.directory / 'state/runtime'
        runtime.mkdir()
        (runtime / 'codex.elf').write_bytes(b'not owned')
        self.install(success=False)
        self.assertEqual((runtime / 'codex.elf').read_bytes(), b'not owned')

    def test_other_build_and_symlink_are_not_selected(self):
        folder = self.directory / 'payloads/ps5-ai-cli'
        folder.mkdir()
        (folder / 'ps5-ai-cli-other.elf').write_bytes(self.image(b'x' * 32))
        outside = self.directory / 'external.elf'
        outside.write_bytes(self.image())
        (folder / 'ps5-ai-cli-link.elf').symlink_to(outside)
        self.install(success=False)
        self.assertFalse((self.directory / 'state/runtime/codex.elf').exists())

    def test_owned_previous_build_is_updated(self):
        runtime = self.directory / 'state/runtime'
        runtime.mkdir()
        (runtime / 'codex.elf').write_bytes(self.image(b'x' * 32))
        folder = self.directory / 'payloads/ps5-ai-cli'
        folder.mkdir()
        (folder / 'ps5-ai-cli-new.elf').write_bytes(self.image())
        self.install()
        self.assertEqual((runtime / 'codex.elf').read_bytes(), self.image())
