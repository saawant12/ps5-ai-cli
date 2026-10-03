"""Deletion candidates must be owned, superseded and unambiguous."""
import unittest
from deployments import superseded

class DeploymentCleanupTests(unittest.TestCase):
    def test_keeps_unrecorded_other_projects_other_consoles_and_current(self):
        base = "http://ps5:8084"
        old = "/data/pldmgr/payloads/probe/probe-aaaaaaaaaaaa.elf"
        current = "/data/pldmgr/payloads/probe/probe-bbbbbbbbbbbb.elf"
        unrecorded = "/data/pldmgr/payloads/probe/probe-cccccccccccc.elf"
        entries = [{"manager": base, "stem": "probe", "sha256": "a" * 64, "path": old},
                   {"manager": base, "stem": "probe", "sha256": "b" * 64, "path": current},
                   {"manager": "http://other:8084", "stem": "probe", "sha256": "c" * 64, "path": unrecorded}]
        self.assertEqual(superseded(entries, base, "probe", current, [old, current, unrecorded]), [old])
        self.assertEqual(superseded(entries, base, "other", current, [old]), [])

    def test_rejects_ambiguous_names_and_mismatched_hashes(self):
        path = "/a/probe-aaaaaaaaaaaa.elf"
        entries = [{"manager": "http://ps5", "stem": "probe", "sha256": "a" * 64, "path": path}]
        self.assertEqual(superseded(entries, "http://ps5", "probe", "/current", [path, "/b/" + path.split('/')[-1]]), [])
        entries[0]["sha256"] = "b" * 64
        self.assertEqual(superseded(entries, "http://ps5", "probe", "/current", [path]), [])
