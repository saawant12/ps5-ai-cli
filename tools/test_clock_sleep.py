"""Check the PS5 sleep adapter's error and interruption contract."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(shutil.which('cc'), 'a C compiler is required')
class ClockSleepTests(unittest.TestCase):
    def test_relative_sleep_and_interruption(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / 'sleep-driver'
            # macOS lacks clock_nanosleep; test using the target's flag value.
            defines = ['-DTIMER_ABSTIME=1'] if sys.platform == 'darwin' else []
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror',
                            *defines,
                            '-Dnanosleep=ps5_test_nanosleep',
                            str(root / 'tools/sleep/driver.c'),
                            str(root / 'platform/clock-sleep.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)
