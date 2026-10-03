"""Validate a real cross-built artifact and reject corrupted/incompatible ELFs."""
from pathlib import Path
import struct
import unittest
from payload import inspect_elf

ROOT = Path(__file__).resolve().parents[1]


class PayloadValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.elf = (ROOT / "build/ps5-rust-runtime-probe.elf").read_bytes()

    def test_real_sdk_artifact(self):
        info = inspect_elf(self.elf)
        self.assertEqual(set(info["needed"]), {
            "libkernel_web.sprx", "libSceLibcInternal.sprx", "libSceNet.sprx"
        })
        self.assertFalse(info["execution_verified"])

    def test_truncated_segment(self):
        with self.assertRaisesRegex(ValueError, "segment extent"):
            inspect_elf(self.elf[:20000])

    def test_invalid_entrypoint(self):
        data = bytearray(self.elf)
        struct.pack_into("<Q", data, 24, 0xffffffffffffffff)
        with self.assertRaisesRegex(ValueError, "Entrypoint"):
            inspect_elf(data)

    def test_missing_section_headers(self):
        data = bytearray(self.elf)
        struct.pack_into("<H", data, 60, 0)
        with self.assertRaisesRegex(ValueError, "section-header"):
            inspect_elf(data)

    def test_interpreter_and_native_tls_rejected(self):
        for kind, message in ((3, "PT_INTERP"), (7, "PT_TLS")):
            with self.subTest(kind=kind):
                data = bytearray(self.elf)
                phoff = struct.unpack_from("<Q", data, 32)[0]
                struct.pack_into("<I", data, phoff, kind)
                with self.assertRaisesRegex(ValueError, message):
                    inspect_elf(data)

    def test_linux_dependency_rejected(self):
        original = b"libSceNet.sprx"
        data = self.elf.replace(original, b"libc.so.6".ljust(len(original), b"\0"))
        with self.assertRaisesRegex(ValueError, "Non-PS5"):
            inspect_elf(data)

    def test_system_kernel_variant(self):
        artifact = ROOT / "build/ps5-loader-runtime-probe.elf"
        if not artifact.exists():
            self.skipTest("Build the loader probe to validate its SDK kernel variant")
        info = inspect_elf(artifact.read_bytes())
        self.assertIn("libkernel_sys.sprx", info["needed"])
        self.assertFalse(info["execution_verified"])

    def test_unknown_kernel_dependency_rejected(self):
        data = self.elf.replace(b"libkernel_web.sprx", b"libkernel_bad.sprx")
        with self.assertRaisesRegex(ValueError, "Missing supported SDK"):
            inspect_elf(data)


if __name__ == "__main__":
    unittest.main()
