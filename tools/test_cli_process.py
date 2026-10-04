"""Exercise the native CLI supervisor against real, isolated host child processes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('cc'), 'a C compiler is required')
class CliProcessTests(unittest.TestCase):
    def test_restart_hung_child_reaps_owned_pid_and_preserves_unrelated_process(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory)/'cli-driver'
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-pthread',
                            str(root/'tools/cli/driver.c'), str(root/'app/cli-process.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=12)
