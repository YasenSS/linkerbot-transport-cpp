#!/usr/bin/env python3
"""Reject cache tampering even when the archived download itself is authentic."""
import hashlib
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "cmake_cache", Path(__file__).with_name("verify-cmake-tree.py"))
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.directory = self.root / "cmake-3.31.8-windows-x86_64"
        self.archive = self.root / "cmake.zip"
        with zipfile.ZipFile(self.archive, "w") as source:
            source.writestr(self.directory.name + "/bin/cmake.exe", b"trusted executable")
            source.writestr(self.directory.name + "/share/cmake/module.cmake", b"trusted module")
        with zipfile.ZipFile(self.archive) as source:
            source.extractall(self.root)
        self.sha = hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def verify(self):
        return cache.verify(self.archive, self.directory, self.sha)

    def test_accept_complete_verified_installation(self):
        self.assertEqual(self.verify()["verified_files"], 2)

    def test_reject_modified_executable_even_when_archive_is_authentic(self):
        (self.directory / "bin/cmake.exe").write_bytes(b"altered executable")
        with self.assertRaisesRegex(ValueError, "file SHA256"):
            self.verify()

    def test_reject_extra_dll(self):
        (self.directory / "bin/extra.dll").write_bytes(b"unexpected loader dependency")
        with self.assertRaisesRegex(ValueError, "Unexpected files"):
            self.verify()

    def test_reject_modified_module(self):
        (self.directory / "share/cmake/module.cmake").write_bytes(b"altered module")
        with self.assertRaises(ValueError):
            self.verify()

    def test_reject_missing_file(self):
        (self.directory / "bin/cmake.exe").unlink()
        with self.assertRaises((ValueError, FileNotFoundError)):
            self.verify()

    def test_reject_archive_hash_mismatch(self):
        self.archive.write_bytes(b"wrong archive")
        with self.assertRaisesRegex(ValueError, "archive SHA256"):
            self.verify()

    def test_reject_wrong_archive_root(self):
        with zipfile.ZipFile(self.archive, "w") as source:
            source.writestr("different-cmake/bin/cmake.exe", b"trusted executable")
        self.sha = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        with self.assertRaisesRegex(ValueError, "Unexpected archive path"):
            self.verify()

    def test_reject_archive_traversal(self):
        with zipfile.ZipFile(self.archive, "w") as source:
            source.writestr(self.directory.name + "/../escape", b"unexpected")
        self.sha = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        with self.assertRaisesRegex(ValueError, "Unexpected archive path"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
