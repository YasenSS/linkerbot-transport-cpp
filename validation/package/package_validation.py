"""Shared SDK metadata and static-object validation (standard library only)."""
import json
from pathlib import Path
import re
import struct

ELF_MACHINES = {"x86_64": 62, "aarch64": 183}
COFF_MACHINES = {"x64": 0x8664, "ARM64": 0xAA64}
BIGOBJ_CLASS_ID = bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8")


def _bytes(value):
    return value if isinstance(value, bytes) else Path(value).read_bytes()


def archive_members(value):
    """Yield every actual member of ordinary ar/MSVC archives, not index tables."""
    data = _bytes(value)
    if not data.startswith(b"!<arch>\n"):
        raise ValueError("Expected an ordinary static archive (thin archives are not portable)")
    offset = 8
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:60] != b"`\n":
            raise ValueError("Invalid archive header")
        name = header[:16].decode("ascii").strip()
        try:
            size = int(header[48:58])
        except ValueError as error:
            raise ValueError("Invalid archive member size") from error
        if size < 0:
            raise ValueError("Negative archive member size")
        body = data[offset + 60:offset + 60 + size]
        if len(body) != size or offset + 60 + size + size % 2 > len(data):
            raise ValueError("Truncated archive member")
        offset += 60 + size + size % 2
        if name.startswith("#1/"):
            length = int(name[3:])
            if length < 1 or length > len(body):
                raise ValueError("Invalid extended archive member name")
            name, body = body[:length].decode("utf-8").rstrip("\0"), body[length:]
        if name in ("/", "//", "/SYM64/") or name in ("__.SYMDEF", "__.SYMDEF SORTED", "__.SYMDEF_64", "__.SYMDEF_64 SORTED"):
            continue
        yield name, body


def archive_machines(value, system="Linux"):
    """Read every ELF/COFF object; reject import libraries and mixed formats."""
    machines = []
    for name, body in archive_members(value):
        if system == "Linux":
            if len(body) < 20 or body[:4] != b"\x7fELF" or body[4] != 2 or body[5] not in (1, 2):
                raise ValueError("Every Linux library object must be an ELF64 object: " + name)
            endian = "<" if body[5] == 1 else ">"
            object_type, machine = struct.unpack(endian + "HH", body[16:20])
            if object_type != 1:
                raise ValueError("Archive contains a non-relocatable ELF object")
        elif system == "Windows":
            if len(body) < 20:
                raise ValueError("Truncated COFF object: " + name)
            if body[:4] == b"\0\0\xff\xff":
                # Anonymous short import objects share the signature but have
                # version 0: they must never masquerade as static SDK objects.
                if len(body) < 56 or struct.unpack_from("<H", body, 4)[0] < 2 or body[12:28] != BIGOBJ_CLASS_ID:
                    raise ValueError("Import/unsupported anonymous COFF object in static SDK: " + name)
                machine = struct.unpack_from("<H", body, 6)[0]
                sections = struct.unpack_from("<I", body, 44)[0]
            else:
                machine, sections = struct.unpack_from("<HH", body)
                optional_header_size, characteristics = struct.unpack_from("<HH", body, 16)
                if optional_header_size or characteristics & 0x0002:
                    raise ValueError("Executable image in COFF archive: " + name)
            if not sections:
                raise ValueError("COFF object has no sections: " + name)
        else:
            raise ValueError("Unsupported object system: " + system)
        machines.append(machine)
    if not machines:
        raise ValueError("Archive contains no objects")
    return machines


def pe_machine(value):
    """Return the actual PE machine of a Windows executable or DLL."""
    data = _bytes(value)
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Expected a PE binary")
    offset = struct.unpack_from("<I", data, 60)[0]
    if offset < 64 or offset + 24 > len(data) or data[offset:offset + 4] != b"PE\0\0":
        raise ValueError("Invalid PE header")
    return struct.unpack_from("<H", data, offset + 4)[0]


def verify_metadata(info, sha, arch, configuration, system="Linux", version=None,
                    distribution=None, require_clean=False):
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ValueError("Expected a full commit SHA")
    if system not in ("Linux", "Windows"):
        raise ValueError("Unsupported SDK system")
    supported = ELF_MACHINES if system == "Linux" else COFF_MACHINES
    if arch not in supported:
        raise ValueError("Architecture is not valid for SDK system")
    expected = {"schema_version": 1, "package": "linkerbot-comm", "comm_sha": sha,
                "system": system, "architecture": arch, "configuration": configuration,
                "pic": True, "instrumented": False, "cxx_standard": 17}
    if version is not None:
        expected["version"] = version
    if distribution is not None:
        expected["distribution"] = distribution
    if require_clean:
        expected["source_dirty"] = False
    if system == "Linux":
        expected.update(standard_library="libstdcxx", libstdcxx_dual_abi="1")
        if not re.fullmatch(r"GNU [0-9]+(?:\.[0-9]+)+", info.get("compiler", "")):
            raise ValueError("Native Ubuntu SDK must identify a GNU compiler")
        if not re.fullmatch(r"ubuntu(?:20|22|24|26)\.04", info.get("distribution", "")):
            raise ValueError("Unknown Ubuntu build distribution")
    else:
        if configuration not in ("Debug", "Release"):
            raise ValueError("Unsupported Windows configuration")
        expected.update(standard_library="msvc", toolset="143", distribution="Windows",
                        runtime={"Debug": "MDd", "Release": "MD"}[configuration])
        if not re.fullmatch(r"MSVC [0-9]+(?:\.[0-9]+)+", info.get("compiler", "")):
            raise ValueError("Windows SDK must identify an MSVC compiler")
    for key, value in expected.items():
        actual = info.get(key)
        if type(actual) is not type(value) or actual != value:
            raise ValueError("Metadata mismatch for %s: %r != %r" % (key, actual, value))
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", info.get("version", "")):
        raise ValueError("Invalid SDK version")


def verify_files(files, info):
    """Validate a mapping of package-relative file names to their exact bytes."""
    libraries = []
    for relative, data in files.items():
        name = Path(relative).name.lower()
        if (re.search(r"\.(so(\..*)?|dll|exe|dylib)$", name)
                or "fixture" in name or "catch2" in name
                or name in ("comm-tests", "pcan-tests", "vendor-loader-tests")):
            raise ValueError("Runtime/test/vendor component leaked into SDK: " + relative)
        if name.endswith((".a", ".lib")):
            libraries.append(relative)
        elif (data[:4] == b"\x7fELF" or data[:2] == b"MZ"
              or data[:4] in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe")):
            raise ValueError("Unexpected runtime binary: " + relative)
    expected_name = "liblinkerbot-comm.a" if info["system"] == "Linux" else "linkerbot-comm.lib"
    if len(libraries) != 1 or Path(libraries[0]).name != expected_name:
        raise ValueError("Expected exactly the comm static library")
    machines = archive_machines(files[libraries[0]], info["system"])
    expected_machine = (ELF_MACHINES if info["system"] == "Linux" else COFF_MACHINES)[info["architecture"]]
    if any(machine != expected_machine for machine in machines):
        raise ValueError("Archive object architecture mismatch: %r" % machines)
    for required in ("include/linkerbot/transport/session.hpp", "share/linkerbot-comm/LICENSE",
                     "share/linkerbot-comm/validation/installed-consumer/CMakeLists.txt"):
        if required not in files:
            raise ValueError("Missing SDK entry: " + required)
    return len(machines)


def verify_prefix(root, **expected):
    root = Path(root).resolve()
    files = {}
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError("Unexpected symlink: %s" % path)
        if path.is_file():
            files[path.relative_to(root).as_posix()] = path.read_bytes()
    info = json.loads(files["share/linkerbot-comm/build-info.json"])
    verify_metadata(info, **expected)
    count = verify_files(files, info)
    return {"metadata": info, "verified_objects": count}
