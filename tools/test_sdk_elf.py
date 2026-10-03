"""Reject malformed loader inputs before any native process is created."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


def fixture():
    data = bytearray(0x5000)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HHIQQQIHHHHHH', data, 16,
                     3, 62, 1, 0, 64, 0x300, 0, 64, 56, 2, 64, 2, 0)
    struct.pack_into('<IIQQQQQQ', data, 64, 1, 7, 0x4000, 0, 0, 0x1000, 0x2000, 0x4000)
    struct.pack_into('<IIQQQQQQ', data, 120, 2, 6, 0x4100, 0x100, 0, 64, 64, 8)
    for index, pair in enumerate([(5, 0x200), (10, 64), (1, 0), (0, 0)]):
        struct.pack_into('<QQ', data, 0x4100 + index * 16, *pair)
    dependency = b'libkernel_web.sprx\0'
    data[0x4200:0x4200 + len(dependency)] = dependency
    struct.pack_into('<IIQQQQIIQQ', data, 0x340, 0, 4, 0, 0, 0x4300, 24, 0, 0, 8, 24)
    struct.pack_into('<QQq', data, 0x4300, 0x800, 8, 0x100)
    return data


@unittest.skipUnless(shutil.which('cc'), 'a C compiler is required')
class SDKELFTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        root = Path(__file__).resolve().parents[1]
        library = Path(cls.directory.name) / 'sdk-elf.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(root / 'platform/sdk-elf.c'), '-o', str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.library.ps5_sdk_elf_valid.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.library.ps5_sdk_elf_valid.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def valid(self, data):
        buffer = ctypes.create_string_buffer(bytes(data))
        return self.library.ps5_sdk_elf_valid(buffer, len(data))

    def test_accept_payload_and_reject_all_truncations(self):
        data = fixture()
        self.assertEqual(self.valid(data), 1)
        for length in range(len(data)):
            if self.valid(data[:length]):
                self.fail(f'accepted truncated input of {length} bytes')

    def test_reject_invalid_extents_and_unsupported_formats(self):
        cases = [
            (16, '<H', 2), (18, '<H', 183), (32, '<Q', 2**64 - 1),
            (40, '<Q', 2**64 - 1), (54, '<H', 55), (56, '<H', 0),
            (64, '<I', 3), (64, '<I', 7), (80, '<Q', 0x4000),
            (104, '<Q', 2**64 - 1), (112, '<Q', 4096),
            (0x4108, '<Q', 0x2000), (0x4118, '<Q', 2**64 - 1),
            (0x4128, '<Q', 64), (0x4130, '<Q', 1),
            (0x344, '<I', 9), (0x358, '<Q', 2**64 - 1),
            (0x360, '<Q', 25), (0x378, '<Q', 16),
            (0x4300, '<Q', 0x1ffc), (0x4300, '<Q', 2**64 - 1),
            (0x4308, '<Q', 1),
        ]
        for offset, format_, value in cases:
            with self.subTest(offset=offset, value=value):
                data = fixture()
                struct.pack_into(format_, data, offset, value)
                self.assertEqual(self.valid(data), 0)
        for dependency in (b'libc.so.7\0', b'lib../../evil.sprx\0', b'libother.sprx\0'):
            data = fixture()
            data[0x4200:0x4200 + len(dependency)] = dependency
            self.assertEqual(self.valid(data), 0)

    def test_generated_shell_when_available(self):
        root = Path(__file__).resolve().parents[1]
        images = [root / 'build/shell/sh.elf', root / 'build/shell/sbase-box.elf']
        present = [path for path in images if path.exists()]
        if not present:
            self.skipTest('native runtime has not been built')
        for path in present:
            with self.subTest(image=path.name):
                self.assertEqual(self.valid(path.read_bytes()), 1)
