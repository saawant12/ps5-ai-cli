"""Exercise the actual shortcut installer against disposable application trees."""
import os
from pathlib import Path
import shutil
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

    def test_upgrades_only_exact_owned_legacy_icon(self):
        self.install()
        icon = self.apps / 'PAIC00001/sce_sys/icon0.png'
        icon.write_bytes((ROOT/'launcher/legacy/icon0-v1.png').read_bytes())
        (self.state/'ps5-ai-cli-launcher-icon-v2').unlink()
        manifest = self.apps / 'PAIC00001/sce_sys/param.json'
        before = manifest.read_bytes(), manifest.stat().st_mtime_ns
        self.assertIn('registrations=1', self.install())
        self.assertEqual(icon.read_bytes(), (ROOT/'launcher/sce_sys/icon0.png').read_bytes())
        self.assertEqual(before, (manifest.read_bytes(), manifest.stat().st_mtime_ns))
        after = self.snapshot()
        self.assertIn('registrations=0', self.install())
        self.assertEqual(after, self.snapshot())

    def test_icon_upgrade_registration_failure_retries_without_rewriting(self):
        self.install()
        icon = self.apps / 'PAIC00001/sce_sys/icon0.png'
        icon.write_bytes((ROOT/'launcher/legacy/icon0-v1.png').read_bytes())
        (self.state/'ps5-ai-cli-launcher-icon-v2').unlink()
        self.install(success=False, fail_registration=True)
        before = icon.read_bytes(), icon.stat().st_mtime_ns
        self.assertIn('registrations=1', self.install())
        self.assertEqual(before, (icon.read_bytes(), icon.stat().st_mtime_ns))

    def test_missing_owned_icon_is_restored(self):
        self.install()
        icon = self.apps / 'PAIC00001/sce_sys/icon0.png'
        icon.unlink()
        self.assertIn('registrations=1', self.install())
        self.assertEqual(icon.read_bytes(), (ROOT/'launcher/sce_sys/icon0.png').read_bytes())

    def test_deleted_shortcut_reinstalls_without_changing_saved_data(self):
        self.install()
        (self.state/'keep').write_bytes(b'saved workspace placeholder')
        before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.state.iterdir()}
        shutil.rmtree(self.apps/'PAIC00001')
        self.assertIn('registrations=1', self.install())
        self.assertEqual(before, {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.state.iterdir()})
        self.assertEqual((self.apps/'PAIC00001/sce_sys/icon0.png').read_bytes(),
                         (ROOT/'launcher/sce_sys/icon0.png').read_bytes())
        self.assertIn('registrations=0', self.install())

    def test_modified_or_symlink_icon_is_preserved(self):
        self.install()
        icon = self.apps / 'PAIC00001/sce_sys/icon0.png'
        icon.write_bytes(b'user replacement')
        before = self.snapshot()
        self.install(success=False)
        self.assertEqual(before, self.snapshot())
        icon.unlink()
        outside = self.root/'foreign-icon'; outside.write_bytes(b'preserve')
        icon.symlink_to(outside)
        self.install(success=False)
        self.assertTrue(icon.is_symlink())
        self.assertEqual(outside.read_bytes(), b'preserve')

if __name__ == '__main__':
    unittest.main()
