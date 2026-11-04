#!/usr/bin/env python3
"""Negative tests for release package validation, using synthetic ELF archives."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('verify-package.py')
SHA = 'a' * 40


def archive(machine=62):
    body = b'\x7fELF' + bytes([2, 1, 1]) + b'\0' * 9 + struct.pack('<HH', 1, machine)
    header = (f'{"test.o/":<16}{0:<12}{0:<6}{0:<6}{"100644":<8}{len(body):<10}`\n').encode()
    return b'!<arch>\n' + header + body


class PackageValidation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.info = {'package': 'linkerbot-comm', 'version': '0.3.0', 'comm_sha': SHA,
                     'system': 'Linux', 'architecture': 'x86_64', 'configuration': 'Release',
                     'pic': True, 'instrumented': False, 'cxx_standard': 17, 'source_dirty': False}
        for relative in ('lib/liblinkerbot-comm.a', 'include/linkerbot/transport/session.hpp',
                         'share/linkerbot-comm/LICENSE',
                         'share/linkerbot-comm/validation/installed-consumer/CMakeLists.txt'):
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(archive() if path.suffix == '.a' else b'fixture')

    def verify(self):
        (self.root / 'share/linkerbot-comm/build-info.json').write_text(json.dumps(self.info))
        return subprocess.run([sys.executable, str(SCRIPT), str(self.root), '--sha', SHA,
                               '--arch', 'x86_64', '--require-clean'], capture_output=True)

    def test_valid_package(self):
        self.assertEqual(self.verify().returncode, 0)

    def test_wrong_archive_architecture(self):
        (self.root / 'lib/liblinkerbot-comm.a').write_bytes(archive(183))
        self.assertNotEqual(self.verify().returncode, 0)

    def test_mixed_archive_architectures(self):
        (self.root / 'lib/liblinkerbot-comm.a').write_bytes(archive() + archive(183)[8:])
        self.assertNotEqual(self.verify().returncode, 0)

    def test_truncated_archive(self):
        (self.root / 'lib/liblinkerbot-comm.a').write_bytes(archive()[:-1])
        self.assertNotEqual(self.verify().returncode, 0)

    def test_vendor_runtime_leak(self):
        (self.root / 'lib/PCANBasic.dll').write_bytes(b'MZ')
        self.assertNotEqual(self.verify().returncode, 0)

    def test_renamed_vendor_binary(self):
        (self.root / "lib/vendor-data").write_bytes(b"MZ\0\0")
        self.assertNotEqual(self.verify().returncode, 0)

    def test_wrong_provenance(self):
        self.info['comm_sha'] = 'b' * 40
        self.assertNotEqual(self.verify().returncode, 0)

    def test_dirty_source(self):
        self.info['source_dirty'] = True
        self.assertNotEqual(self.verify().returncode, 0)

    def test_missing_pic(self):
        self.info['pic'] = False
        self.assertNotEqual(self.verify().returncode, 0)

    def test_instrumented(self):
        self.info['instrumented'] = True
        self.assertNotEqual(self.verify().returncode, 0)


if __name__ == '__main__':
    unittest.main()
