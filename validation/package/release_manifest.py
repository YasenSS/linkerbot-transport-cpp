"""Validate the entire native release matrix before writing any release metadata."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import tarfile
import zipfile
from package_validation import verify_files, verify_metadata


def expected_targets(target_file):
    data = json.loads(Path(target_file).read_text())
    if data.get("schema_version") != 1:
        raise ValueError("Unsupported package target schema")
    linux = data["linux"]
    windows = data["windows"]
    expected_linux = {(ubuntu, arch) for ubuntu in ("20.04", "22.04", "24.04", "26.04")
                      for arch in ("x86_64", "aarch64")}
    expected_windows = {(arch, config) for arch in ("x64", "ARM64") for config in ("Debug", "Release")}
    if len(linux) != 8 or {(row["ubuntu"], row["arch"]) for row in linux} != expected_linux:
        raise ValueError("Incomplete or duplicate native Ubuntu package target matrix")
    if len(windows) != 4 or {(row["arch"], row["configuration"]) for row in windows} != expected_windows:
        raise ValueError("Incomplete or duplicate native Windows package target matrix")
    identities = set()
    for row in linux:
        if row["distribution"] != "ubuntu" + row["ubuntu"]:
            raise ValueError("Ubuntu distribution target mismatch")
        identities.add(("Linux", row["distribution"], row["arch"], "Release"))
    for row in windows:
        if row["runtime"] != {"Debug": "MDd", "Release": "MD"}[row["configuration"]] or row["toolset"] != "143":
            raise ValueError("Windows target CRT/toolset mismatch")
        identities.add(("Windows", "Windows", row["arch"], row["configuration"]))
    return identities


def read_package(archive):
    """Read regular files without extracting paths or following archive links."""
    files = {}
    seen = set()
    roots = set()

    def accept(name, is_directory, reader):
        parts = name.rstrip("/").split("/")
        if (not parts or any(part in ("", ".", "..") for part in parts)
                or "\\" in name or "\0" in name or ":" in parts[0]):
            raise ValueError("Unsafe SDK archive path: " + name)
        if name.rstrip("/") in seen:
            raise ValueError("Duplicate SDK archive path: " + name)
        seen.add(name.rstrip("/"))
        roots.add(parts[0])
        if is_directory:
            return
        if len(parts) < 2:
            raise ValueError("Expected one top-level SDK directory")
        relative = PurePosixPath(*parts[1:]).as_posix()
        files[relative] = reader()

    if archive.name.endswith(".tar.gz"):
        with tarfile.open(archive) as package:
            for member in package.getmembers():
                if not member.isdir() and not member.isfile():
                    raise ValueError("SDK archive contains a link or special file")
                accept(member.name, member.isdir(), lambda member=member: package.extractfile(member).read())
    elif archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as package:
            for member in package.infolist():
                mode = stat.S_IFMT(member.external_attr >> 16)
                if mode not in (0, stat.S_IFREG, stat.S_IFDIR):
                    raise ValueError("SDK ZIP contains a link or special file")
                accept(member.filename, member.is_dir(), lambda member=member: package.read(member))
    else:
        raise ValueError("Unsupported SDK archive format")
    if len(roots) != 1:
        raise ValueError("Expected one top-level SDK directory")
    info = json.loads(files["share/linkerbot-comm/build-info.json"])
    return info, files


def build_manifest(root, sha, version, target_file, validation_run):
    """Validate exact bytes and all twelve identities, returning a manifest."""
    root = Path(root)
    expected = expected_targets(target_file)
    archives = sorted(list(root.glob("*.tar.gz")) + list(root.glob("*.zip")))
    if len(archives) != len(expected):
        raise ValueError("Expected exactly twelve native SDK archives")
    required_names = {archive.name for archive in archives}
    required_names.update(archive.name + ".sha256" for archive in archives)
    if any(path.is_symlink() or not path.is_file() for path in root.iterdir()):
        raise ValueError("Release staging directory must contain only regular files")
    if {path.name for path in root.iterdir()} != required_names:
        raise ValueError("Unexpected or missing release staging asset")
    assets = []
    actual = set()
    for archive in archives:
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        sidecar = Path(str(archive) + ".sha256")
        fields = sidecar.read_text().split()
        if len(fields) != 2 or fields[0].lower() != digest or fields[1] not in (archive.name, "*" + archive.name):
            raise ValueError("SDK checksum mismatch: " + archive.name)
        info, files = read_package(archive)
        system = "Windows" if archive.suffix == ".zip" else "Linux"
        identity = (system, info.get("distribution"), info.get("architecture"), info.get("configuration"))
        if identity not in expected or identity in actual:
            raise ValueError("Unexpected or duplicate native SDK identity: %r" % (identity,))
        verify_metadata(info, sha=sha, version=version, system=system, arch=identity[2],
                        distribution=identity[1], configuration=identity[3], require_clean=True)
        if identity[1] == "ubuntu20.04" and not info["compiler"].startswith("GNU 9."):
            raise ValueError("Focal compatibility baseline requires GCC 9")
        compiler_version = info["compiler"].split()[1]
        if system == "Linux":
            expected_name = "linkerbot-comm-%s-Linux-%s-%s-GNU%s-libstdcxx-abi1-Release.tar.gz" % (
                version, identity[2], identity[1], compiler_version)
        else:
            expected_name = "linkerbot-comm-%s-Windows-%s-msvc%s-%s-%s-%s.zip" % (
                version, identity[2], info["toolset"], compiler_version, identity[3], info["runtime"])
        if archive.name != expected_name:
            raise ValueError("SDK filename disagrees with verified metadata: " + archive.name)
        count = verify_files(files, info)
        actual.add(identity)
        assets.append({"file": archive.name, "sha256": digest, "bytes": archive.stat().st_size,
                       "verified_objects": count, "build": info})
    if actual != expected:
        raise ValueError("Missing native SDK identities: %r" % (expected - actual,))
    if not re.fullmatch(r"https://github\.com/[^/]+/[^/]+/actions/runs/[0-9]+", validation_run):
        raise ValueError("Expected a concrete GitHub Actions validation run URL")
    return {"schema_version": 1, "version": version, "source_sha": sha, "assets": assets,
            "validation_run": validation_run,
            "scope": "Exact-source quality gates; Ubuntu 20.04/22.04/24.04/26.04 native x86_64/aarch64 SDKs; Windows native x64/ARM64 Debug/Release regression and relocated consumption; Focal cross-distribution and ROS1 Noetic validation; no hardware acceptance"}


def write_manifest(root, manifest):
    root = Path(root)
    (root / "release-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    files = sorted(path for path in root.iterdir() if path.is_file())
    (root / "SHA256SUMS.txt").write_text("".join(
        hashlib.sha256(path.read_bytes()).hexdigest() + "  " + path.name + "\n" for path in files))
