"""Exercise the raw terminal adapter using real descriptors and socket I/O."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('cc'), 'a C compiler is required')
class NativeTerminalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / 'terminal-driver'
        symbols = ('isatty', 'tcgetattr', 'tcsetattr', 'tcflush', 'ioctl')
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-pthread',
                        *(f'-D__real_{name}={name}' for name in symbols),
                        str(root / 'tools/terminal/driver.c'), str(root / 'app/native-terminal.c'),
                        '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_terminal_input_output_resize_and_reconnect(self):
        subprocess.run([str(self.binary)], check=True, timeout=5)

    def test_missing_standard_descriptors_do_not_overwrite_gateway(self):
        subprocess.run([str(self.binary), 'closed-stdio'], check=True, timeout=5)
