"""Check signal ownership and exit propagation in the native command proxy."""
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('cc'), 'a C compiler is required')
class ShellExecTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / 'shell-driver'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror',
                        str(root / 'tools/shell/driver.c'), str(root / 'platform/shell-exec.c'),
                        '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_caught_child_signal_is_reset_and_exit_status_preserved(self):
        result = subprocess.run([str(self.binary), 'success'], capture_output=True, timeout=5)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (37, b'child ran\n', b''))

    def test_ignored_child_signal_does_not_auto_reap_owned_child(self):
        result = subprocess.run([str(self.binary), 'ignored'], capture_output=True, timeout=5)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (37, b'child ran\n', b''))

    def test_failed_launch_restores_handler_mask_flags_and_errno(self):
        subprocess.run([str(self.binary), 'failure'], check=True, timeout=5)

    def test_child_termination_signal_reaches_calling_shell(self):
        result = subprocess.run([str(self.binary), 'signal'], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, -signal.SIGTERM)


if __name__ == '__main__':
    unittest.main()
