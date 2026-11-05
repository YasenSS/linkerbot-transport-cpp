#!/usr/bin/env python3
"""Regression checks for the exact twelve-archive native release contract."""
import copy
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tarfile
import tempfile
import unittest
import zipfile
sys.dont_write_bytecode = True
from release_manifest import build_manifest, write_manifest

SHA = "a" * 40
VERSION = "0.3.1"
RUN_URL = "https://github.com/test/repo/actions/runs/123"
TARGETS = Path(__file__).resolve().parents[2] / "ci/package-targets.json"


def static_library(machine, system):
    if system == "Linux":
        body = b"\x7fELF" + bytes([2, 1, 1]) + b"\0" * 9 + struct.pack("<HH", 1, machine)
    else:
        body = struct.pack("<HHIIIHH", machine, 1, 0, 0, 0, 0, 0)
    header = (f'{"object.o/":<16}{0:<12}{0:<6}{0:<6}{"100644":<8}{len(body):<10}`\n').encode()
    return b"!<arch>\n" + header + body + (b"\n" if len(body) % 2 else b"")


class ReleaseManifest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.root = self.directory / "assets"
        self.root.mkdir()
        self.target_file = self.directory / "targets.json"
        self.targets = json.loads(TARGETS.read_text())
        self.target_file.write_text(json.dumps(self.targets))
        self.rows = []
        for target in self.targets["linux"]:
            info = self.base()
            info.update(system="Linux", architecture=target["arch"], configuration="Release",
                        distribution=target["distribution"], standard_library="libstdcxx",
                        libstdcxx_dual_abi="1", compiler="GNU 9.4.0" if target["ubuntu"] == "20.04" else "GNU 13.3.0")
            name = "linkerbot-comm-%s-Linux-%s-%s-GNU%s-libstdcxx-abi1-Release.tar.gz" % (
                VERSION, target["arch"], target["distribution"], info["compiler"].split()[1])
            self.rows.append({"name": name, "info": info})
        for target in self.targets["windows"]:
            info = self.base()
            info.update(system="Windows", architecture=target["arch"], configuration=target["configuration"],
                        distribution="Windows", standard_library="msvc", libstdcxx_dual_abi="unknown",
                        compiler="MSVC 19.44.35219.0", runtime=target["runtime"], toolset="143")
            name = "linkerbot-comm-%s-Windows-%s-msvc143-19.44.35219.0-%s-%s.zip" % (
                VERSION, target["arch"], target["configuration"], target["runtime"])
            self.rows.append({"name": name, "info": info})
        for row in self.rows:
            self.write(row)

    @staticmethod
    def base():
        return {"schema_version": 1, "package": "linkerbot-comm", "version": VERSION,
                "comm_sha": SHA, "source_dirty": False, "instrumented": False,
                "pic": True, "cxx_standard": 17, "build_image": "test@sha256:fixture"}

    def write(self, row, machine=None, mixed=False, extra=None):
        info = row["info"]
        if machine is None:
            machine = {"x86_64": 62, "aarch64": 183, "x64": 0x8664, "ARM64": 0xAA64}[info["architecture"]]
        library = static_library(machine, info["system"])
        if mixed:
            library += static_library(0xAA64 if machine == 0x8664 else 0x8664, "Windows")[8:]
        library_name = "lib/liblinkerbot-comm.a" if info["system"] == "Linux" else "lib/linkerbot-comm.lib"
        files = {library_name: library, "share/linkerbot-comm/build-info.json": json.dumps(info).encode(),
                 "include/linkerbot/transport/session.hpp": b"header", "share/linkerbot-comm/LICENSE": b"MIT",
                 "share/linkerbot-comm/validation/installed-consumer/CMakeLists.txt": b"consumer"}
        if info['package'] == 'linkerbot-transport':
            files[library_name.replace('-comm', '-can')] = library
            for name, content in list(files.items()):
                if name.startswith('share/linkerbot-comm/'):
                    files[name.replace('share/linkerbot-comm/', 'share/linkerbot-transport/')] = content
            for package in ('linkerbot-transport', 'linkerbot-comm'):
                files['lib/cmake/' + package + '/' + package + '-config.cmake'] = b'# fixture'
        files.update(extra or {})
        archive = self.root / row["name"]
        if info["system"] == "Linux":
            with tarfile.open(archive, "w:gz") as package:
                for name, content in files.items():
                    member = tarfile.TarInfo("SDK/" + name)
                    member.size = len(content)
                    package.addfile(member, io.BytesIO(content))
        else:
            with zipfile.ZipFile(archive, "w") as package:
                for name, content in files.items():
                    package.writestr("SDK/" + name, content)
        Path(str(archive) + ".sha256").write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + "  " + archive.name + "\n")

    def manifest(self):
        return build_manifest(self.root, SHA, VERSION, self.target_file, RUN_URL)

    def test_canonical_matrix_preserves_legacy_library_identity(self):
        for path in self.root.iterdir():
            path.unlink()
        for row in self.rows:
            row['name'] = row['name'].replace('linkerbot-comm-', 'linkerbot-transport-')
            row['info'].update(package='linkerbot-transport', transport_sha=SHA,
                               components=['can'], compatibility_libraries=['comm'])
            self.write(row)
        manifest = self.manifest()
        self.assertEqual(len(manifest['assets']), 12)
        self.assertTrue(all(asset['verified_objects'] == 2 for asset in manifest['assets']))
        self.assertTrue(all(asset['file'].startswith('linkerbot-transport-') for asset in manifest['assets']))

    def test_complete_matrix_and_26_assets(self):
        manifest = self.manifest()
        self.assertEqual(len(manifest["assets"]), 12)
        self.assertTrue(all(asset["verified_objects"] == 1 for asset in manifest["assets"]))
        write_manifest(self.root, manifest)
        self.assertEqual(len(list(self.root.iterdir())), 26)
        sums = (self.root / "SHA256SUMS.txt").read_text().splitlines()
        self.assertEqual(len(sums), 25)
        self.assertTrue(any(line.endswith("  release-manifest.json") for line in sums))
        self.assertFalse(any(line.endswith("  SHA256SUMS.txt") for line in sums))

    def test_missing_archive(self):
        (self.root / self.rows[0]["name"]).unlink()
        with self.assertRaisesRegex(ValueError, "exactly twelve"):
            self.manifest()

    def test_duplicate_identity(self):
        old = self.root / self.rows[1]["name"]
        old.unlink()
        Path(str(old) + ".sha256").unlink()
        duplicate = copy.deepcopy(self.rows[0])
        old_version = duplicate["info"]["compiler"].split()[1]
        new_version = old_version.split(".")[0] + ".99.0"
        duplicate["info"]["compiler"] = "GNU " + new_version
        duplicate["name"] = duplicate["name"].replace("-GNU" + old_version + "-", "-GNU" + new_version + "-")
        self.write(duplicate)
        with self.assertRaisesRegex(ValueError, "duplicate native SDK identity"):
            self.manifest()

    def test_filename_wrong_compiler(self):
        row = self.rows[0]
        old = self.root / row["name"]
        old.unlink()
        Path(str(old) + ".sha256").unlink()
        row["name"] = row["name"].replace("-GNU" + row["info"]["compiler"].split()[1] + "-", "-GNU9.99.0-")
        self.write(row)
        with self.assertRaisesRegex(ValueError, "filename disagrees"):
            self.manifest()

    def test_filename_wrong_toolset(self):
        row = self.rows[8]
        old = self.root / row["name"]
        old.unlink()
        Path(str(old) + ".sha256").unlink()
        row["name"] = row["name"].replace("-msvc143-", "-msvc144-")
        self.write(row)
        with self.assertRaisesRegex(ValueError, "filename disagrees"):
            self.manifest()

    def test_wrong_distribution(self):
        self.rows[0]["info"]["distribution"] = "ubuntu18.04"
        self.write(self.rows[0])
        with self.assertRaisesRegex(ValueError, "native SDK identity"):
            self.manifest()

    def test_wrong_elf_object_architecture(self):
        self.write(self.rows[0], machine=183 if self.rows[0]["info"]["architecture"] == "x86_64" else 62)
        with self.assertRaisesRegex(ValueError, "object architecture mismatch"):
            self.manifest()

    def test_wrong_libstdcxx_abi(self):
        self.rows[0]["info"]["libstdcxx_dual_abi"] = "0"
        self.write(self.rows[0])
        with self.assertRaisesRegex(ValueError, "libstdcxx_dual_abi"):
            self.manifest()

    def test_wrong_windows_crt(self):
        row = next(row for row in self.rows if row["info"]["system"] == "Windows" and row["info"]["configuration"] == "Debug")
        row["info"]["runtime"] = "MD"
        self.write(row)
        with self.assertRaisesRegex(ValueError, "runtime"):
            self.manifest()

    def test_wrong_coff_object_architecture(self):
        self.write(self.rows[8], machine=0xAA64 if self.rows[8]["info"]["architecture"] == "x64" else 0x8664)
        with self.assertRaisesRegex(ValueError, "object architecture mismatch"):
            self.manifest()

    def test_mixed_coff_objects(self):
        self.write(self.rows[8], mixed=True)
        with self.assertRaisesRegex(ValueError, "object architecture mismatch"):
            self.manifest()

    def test_checksum_mismatch(self):
        archive = self.root / self.rows[0]["name"]
        Path(str(archive) + ".sha256").write_text("0" * 64 + "  " + archive.name + "\n")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.manifest()

    def test_checksum_wrong_filename(self):
        archive = self.root / self.rows[0]["name"]
        Path(str(archive) + ".sha256").write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + "  other.tar.gz\n")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.manifest()

    def test_wrong_source(self):
        self.rows[0]["info"]["comm_sha"] = "b" * 40
        self.write(self.rows[0])
        with self.assertRaisesRegex(ValueError, "comm_sha"):
            self.manifest()

    def test_dirty_source(self):
        self.rows[0]["info"]["source_dirty"] = True
        self.write(self.rows[0])
        with self.assertRaisesRegex(ValueError, "source_dirty"):
            self.manifest()

    def test_missing_pic(self):
        self.rows[0]["info"]["pic"] = False
        self.write(self.rows[0])
        with self.assertRaisesRegex(ValueError, "pic"):
            self.manifest()

    def test_vendor_leak(self):
        self.write(self.rows[0], extra={"lib/vendor-data": b"MZ\0\0"})
        with self.assertRaisesRegex(ValueError, "Unexpected runtime binary"):
            self.manifest()

    def test_incomplete_central_matrix(self):
        self.targets["linux"].pop()
        self.target_file.write_text(json.dumps(self.targets))
        with self.assertRaisesRegex(ValueError, "target matrix"):
            self.manifest()

    def test_extra_staging_file(self):
        (self.root / "old-release-manifest.json").write_text("{}")
        with self.assertRaisesRegex(ValueError, "staging asset"):
            self.manifest()


if __name__ == "__main__":
    unittest.main()
