#!/usr/bin/env python3
"""Verify an extracted Linux SDK; consumers must separately link and run it."""
import argparse
import json
from pathlib import Path
import re
import struct


def archive_machines(path):
    """Read every ELF member, excluding archive symbol/name tables."""
    data = path.read_bytes()
    if not data.startswith(b"!<arch>\n"):
        raise ValueError("Expected an ordinary static archive (thin archives are not portable)")
    offset = 8
    machines = []
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:60] != b"`\n":
            raise ValueError("Invalid archive header")
        name = header[:16].decode("ascii").strip()
        size = int(header[48:58])
        body = data[offset + 60:offset + 60 + size]
        if len(body) != size:
            raise ValueError("Truncated archive member")
        offset += 60 + size + size % 2
        if name in ("/", "//", "/SYM64/") or name.startswith("__.SYMDEF"):
            continue
        if name.startswith("#1/"):
            body = body[int(name[3:]):]
        if len(body) < 20 or body[:4] != b"\x7fELF" or body[4] != 2 or body[5] not in (1, 2):
            raise ValueError("Every library object must be an ELF64 object")
        endian = "<" if body[5] == 1 else ">"
        object_type, machine = struct.unpack(endian + "HH", body[16:20])
        if object_type != 1:
            raise ValueError("Archive contains a non-relocatable ELF object")
        machines.append(machine)
    if not machines:
        raise ValueError("Archive contains no objects")
    return machines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--sha", required=True)
    parser.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--version")
    parser.add_argument("--require-clean", action="store_true")
    args = parser.parse_args()
    root = args.prefix.resolve()
    info = json.loads((root / "share/linkerbot-comm/build-info.json").read_text())
    expected = {"package": "linkerbot-comm", "comm_sha": args.sha,
                "system": "Linux", "architecture": args.arch,
                "configuration": args.configuration, "pic": True,
                "instrumented": False, "cxx_standard": 17}
    if args.version:
        expected["version"] = args.version
    for key, value in expected.items():
        if info.get(key) != value:
            raise ValueError("Metadata mismatch for %s: %r != %r" % (key, info.get(key), value))
    if not re.fullmatch(r"[0-9a-f]{40}", args.sha):
        raise ValueError("Expected a full commit SHA")
    if args.require_clean and info.get("source_dirty") is not False:
        raise ValueError("Release source tree was dirty")
    libraries = []
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError("Unexpected symlink: %s" % path)
        if not path.is_file():
            continue
        name = path.name.lower()
        if (re.search(r"\.(so(\..*)?|dll|exe|dylib)$", name)
                or "fixture" in name or "catch2" in name
                or name in ("comm-tests", "pcan-tests", "vendor-loader-tests")):
            raise ValueError("Runtime/test/vendor component leaked into SDK: %s" % path)
        if path.suffix == ".a":
            libraries.append(path)
        else:
            magic = path.read_bytes()[:4]
            if magic == b"\x7fELF" or magic[:2] == b"MZ" or magic in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe"):
                raise ValueError("Unexpected runtime binary: %s" % path)
    if len(libraries) != 1 or libraries[0].name != "liblinkerbot-comm.a":
        raise ValueError("Expected exactly the comm static library")
    machines = archive_machines(libraries[0])
    expected_machine = {"x86_64": 62, "aarch64": 183}[args.arch]
    if any(machine != expected_machine for machine in machines):
        raise ValueError("Archive object architecture mismatch: %r" % machines)
    for required in ("include/linkerbot/transport/session.hpp", "share/linkerbot-comm/LICENSE",
                     "share/linkerbot-comm/validation/installed-consumer/CMakeLists.txt"):
        if not (root / required).is_file():
            raise ValueError("Missing SDK entry: " + required)
    print(json.dumps({"metadata": info, "verified_objects": len(machines)}, indent=2))


if __name__ == "__main__":
    main()
