"""Exercise the actual shortcut installer against disposable application trees."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class LauncherTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='ps5-ai-cli-launcher-test-')
        cls.binary = Path(cls.build.name) / 'installer'
        subprocess.run(['python3', 'tools/embed-launcher.py'], cwd=ROOT, check=True)
        subprocess.run([os.environ.get('CC', 'cc'), '-O2', '-Wall', '-Wextra', '-Werror', '-Ibuild', '-Ilauncher',
                        'launcher/install.c', 'tools/launcher/driver.c', '-o', str(cls.binary)], cwd=ROOT, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.state = self.root / 'ps5-ai-cli'
        self.apps = self.root / 'app'
        self.state.mkdir(); self.apps.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def snapshot(self):
        return {str(p.relative_to(self.root)): (p.read_bytes(), p.stat().st_mtime_ns)
                for p in self.root.rglob('*') if p.is_file()}

    def install(self, success=True, fail_registration=False):
        env = dict(os.environ)
        if fail_registration: env['PS5_AI_TEST_FAIL'] = '1'
        result = subprocess.run([str(self.binary), str(self.state), str(self.apps)],
                                capture_output=True, text=True, timeout=5, env=env)
        self.assertEqual(result.returncode, 0 if success else 1, result.stdout + result.stderr)
        return result.stdout

    def test_preserves_other_application_and_repeated_launch(self):
        orbit = self.apps / 'ORBT00001' / 'sce_sys'
        orbit.mkdir(parents=True)
        (orbit / 'param.json').write_bytes(b'original other application manifest')
        (orbit / 'icon0.png').write_bytes(b'original other application icon')
        before = self.snapshot()
        self.assertIn('registrations=1', self.install())
        after = self.snapshot()
        for path, value in before.items(): self.assertEqual(after[path], value)
        self.assertIn('registrations=0', self.install())
        self.assertEqual(after, self.snapshot())

    def test_refuses_unknown_title_collision(self):
        folder = self.apps / 'PAIC00001'; folder.mkdir()
        (folder / 'keep').write_text('existing application')
        before = self.snapshot()
        self.assertIn('registrations=0', self.install(success=False))
        self.assertEqual(before, self.snapshot())

    def test_refuses_symlink_title(self):
        target = self.root / 'untouched'; target.mkdir()
        (self.apps / 'PAIC00001').symlink_to(target, target_is_directory=True)
        self.install(success=False)
        self.assertEqual(list(target.iterdir()), [])
        self.assertEqual(list(self.state.iterdir()), [])

    def test_registration_failure_can_retry_without_rewriting_files(self):
        self.install(success=False, fail_registration=True)
        before = self.snapshot()
        self.install()
        after = self.snapshot()
        for path, value in before.items(): self.assertEqual(after[path], value)

    def test_modified_manifest_is_preserved_and_rejected(self):
        self.install()
        path = self.apps / 'PAIC00001/sce_sys/param.json'
        path.write_text('unexpected replacement')
        before = self.snapshot()
        self.install(success=False)
        self.assertEqual(before, self.snapshot())

if __name__ == '__main__':
    unittest.main()
