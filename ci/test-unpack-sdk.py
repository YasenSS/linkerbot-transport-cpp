#!/usr/bin/env python3
"""Regression: old CPack may exit successfully but produce an empty archive."""
import hashlib
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


class UnpackTests(unittest.TestCase):
    def test_checksummed_empty_archive_is_rejected(self):
        script = Path(__file__).with_name("unpack-sdk.sh").resolve()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            downloads = root / "downloads"
            downloads.mkdir()
            archive = downloads / "empty.tar.gz"
            with tarfile.open(archive, "w:gz"):
                pass
            checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
            (downloads / "empty.tar.gz.sha256").write_text(
                checksum + "  empty.tar.gz\n")
            result = subprocess.run(
                ["bash", str(script), str(downloads), str(root / "extracted")],
                capture_output=True, text=True, check=False)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout, "", "An invalid package must not emit a prefix")
            self.assertIn("empty.tar.gz: OK", result.stderr,
                          "The regression must get past checksum verification")


if __name__ == "__main__":
    unittest.main()
