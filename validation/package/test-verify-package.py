#!/usr/bin/env python3
"""Negative tests for release package validation, using synthetic ELF archives."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.dont_write_bytecode = True
from package_validation import archive_machines, pe_machine

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
                     'schema_version': 1, 'system': 'Linux', 'architecture': 'x86_64', 'configuration': 'Release',
                     'compiler': 'GNU 13.3.0', 'distribution': 'ubuntu24.04',
                     'standard_library': 'libstdcxx', 'libstdcxx_dual_abi': '1',
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
                               '--arch', self.info['architecture'], '--system', self.info['system'], '--require-clean'], capture_output=True)

    def add_optional_library(self, component, wrong_machine=False):
        windows = self.info['system'] == 'Windows'
        name = ('linkerbot-' + component + '.lib') if windows else ('liblinkerbot-' + component + '.a')
        data = coff_archive(0xAA64 if wrong_machine else 0x8664) if windows else archive(183 if wrong_machine else 62)
        (self.root / 'lib' / name).write_bytes(data)
        headers = {'serial': 'include/linkerbot/serial/byte_stream.hpp',
                   'modbus': 'include/linkerbot/modbus/rtu.hpp'}
        path = self.root / headers[component]
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'fixture')
        if component == 'modbus':
            (self.root / 'share/linkerbot-comm/validation/installed-consumer/rtu.cpp').write_bytes(b'fixture')
        return self.root / 'lib' / name

    def test_declared_optional_components(self):
        self.info['components'] = ['comm']
        self.assertEqual(self.verify().returncode, 0)
        for component in ('serial', 'modbus'):
            self.add_optional_library(component)
            self.info['components'].append(component)
            result = self.verify()
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['verified_objects'], 3)

    def test_optional_library_architecture_is_checked(self):
        self.info['components'] = ['comm', 'serial', 'modbus']
        self.add_optional_library('serial')
        library = self.add_optional_library('modbus', wrong_machine=True)
        result = self.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'architecture mismatch', result.stderr)
        library.unlink()
        self.assertNotEqual(self.verify().returncode, 0)

    def test_undeclared_and_duplicate_libraries_are_rejected(self):
        library = self.add_optional_library('serial')
        self.assertNotEqual(self.verify().returncode, 0)
        self.info['components'] = ['comm', 'serial']
        self.assertEqual(self.verify().returncode, 0)
        duplicate = self.root / 'other' / library.name
        duplicate.parent.mkdir()
        duplicate.write_bytes(library.read_bytes())
        self.assertNotEqual(self.verify().returncode, 0)

    def test_invalid_component_declarations(self):
        for components in ('comm', None, [], ['serial'], ['comm', 'modbus'],
                           ['comm', 'vendor'], ['comm', 'comm'], ['comm', 1]):
            with self.subTest(components=components):
                self.info['components'] = components
                self.assertNotEqual(self.verify().returncode, 0)

    def test_optional_component_headers_are_required(self):
        self.info['components'] = ['comm', 'serial']
        self.add_optional_library('serial')
        (self.root / 'include/linkerbot/serial/byte_stream.hpp').unlink()
        self.assertNotEqual(self.verify().returncode, 0)

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


def coff_archive(machine=0x8664, bigobj=False, import_object=False):
    if bigobj:
        body = struct.pack('<HHHHI', 0, 0xffff, 2, machine, 0)
        body += bytes.fromhex('c7a1bad1eebaa94baf20faf66aa4dcb8')
        body += struct.pack('<IIIIIII', 0, 0, 0, 0, 1, 0, 0)
    elif import_object:
        body = struct.pack('<HHHHIIHH', 0, 0xffff, 0, machine, 0, 0, 0, 0)
    else:
        body = struct.pack('<HHIIIHH', machine, 1, 0, 0, 0, 0, 0)
    header = (f'{"test.obj/":<16}{0:<12}{0:<6}{0:<6}{"100644":<8}{len(body):<10}`\n').encode()
    return b'!<arch>\n' + header + body + (b'\n' if len(body) % 2 else b'')


class WindowsPackageValidation(PackageValidation):
    def setUp(self):
        super().setUp()
        self.info.update(system='Windows', architecture='x64', distribution='Windows',
                         compiler='MSVC 19.44.35214.0', standard_library='msvc',
                         libstdcxx_dual_abi='unknown', runtime='MD', toolset='143')
        (self.root / 'lib/liblinkerbot-comm.a').unlink()
        (self.root / 'lib/linkerbot-comm.lib').write_bytes(coff_archive())

    def test_arm64_package(self):
        self.info['architecture'] = 'ARM64'
        (self.root / 'lib/linkerbot-comm.lib').write_bytes(coff_archive(0xAA64))
        self.assertEqual(self.verify().returncode, 0)

    def test_wrong_coff_machine(self):
        (self.root / 'lib/linkerbot-comm.lib').write_bytes(coff_archive(0xAA64))
        self.assertNotEqual(self.verify().returncode, 0)

    def test_mixed_coff_objects(self):
        (self.root / 'lib/linkerbot-comm.lib').write_bytes(coff_archive() + coff_archive(0xAA64)[8:])
        self.assertNotEqual(self.verify().returncode, 0)

    def test_bigobj(self):
        (self.root / 'lib/linkerbot-comm.lib').write_bytes(coff_archive(bigobj=True))
        self.assertEqual(self.verify().returncode, 0)

    def test_import_library(self):
        (self.root / 'lib/linkerbot-comm.lib').write_bytes(coff_archive(import_object=True))
        self.assertNotEqual(self.verify().returncode, 0)

    def test_wrong_crt(self):
        self.info['runtime'] = 'MDd'
        self.assertNotEqual(self.verify().returncode, 0)


class ObjectHeaders(unittest.TestCase):
    def test_mixed_bigobj_machine(self):
        values = archive_machines(coff_archive() + coff_archive(0xAA64, bigobj=True)[8:], 'Windows')
        self.assertEqual(values, [0x8664, 0xAA64])

    def test_pe_x64_arm64(self):
        for machine in (0x8664, 0xAA64):
            body = bytearray(88)
            body[:2] = b'MZ'
            struct.pack_into('<I', body, 60, 64)
            body[64:68] = b'PE\0\0'
            struct.pack_into('<H', body, 68, machine)
            self.assertEqual(pe_machine(bytes(body)), machine)

    def test_truncated_pe(self):
        with self.assertRaises(ValueError):
            pe_machine(b'MZ')


if __name__ == '__main__':
    unittest.main()
