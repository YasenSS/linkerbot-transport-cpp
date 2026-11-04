#!/usr/bin/env python3
"""Verify every installed CMake file against an independently authenticated ZIP."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import zipfile


def digest(stream):
    value = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        value.update(block)
    return value.hexdigest()


def reject_link(path):
    attributes = path.lstat()
    if stat.S_ISLNK(attributes.st_mode) or getattr(attributes, "st_file_attributes", 0) & 0x400:
        raise ValueError("Links/reparse points are not permitted in the CMake cache: " + str(path))


def verify(archive, directory, expected_sha256):
    if not re.fullmatch(r"[0-9a-fA-F]{64}", expected_sha256):
        raise ValueError("Expected an independently obtained SHA256")
    reject_link(archive)
    reject_link(directory)
    with archive.open("rb") as stream:
        if digest(stream) != expected_sha256.lower():
            raise ValueError("CMake archive SHA256 mismatch")
    actual_files = set()
    for installed in directory.rglob("*"):
        reject_link(installed)
        if installed.is_file():
            actual_files.add(installed.relative_to(directory))
        elif not installed.is_dir():
            raise ValueError("Unexpected special file in CMake installation")
    expected_files = set()
    names = set()
    with zipfile.ZipFile(archive) as source:
        for entry in source.infolist():
            name = PurePosixPath(entry.filename)
            if (name.is_absolute() or "\\" in entry.filename or ".." in name.parts
                    or not name.parts or name.parts[0] != directory.name
                    or any(":" in part for part in name.parts)):
                raise ValueError("Unexpected archive path: " + entry.filename)
            normalized = str(name).casefold().rstrip("/")
            if normalized in names:
                raise ValueError("Duplicate archive path: " + entry.filename)
            names.add(normalized)
            if stat.S_ISLNK(entry.external_attr >> 16):
                raise ValueError("Archive contains a symbolic link")
            if entry.is_dir():
                continue
            relative = Path(*name.parts[1:])
            installed = directory / relative
            reject_link(installed)
            if not installed.is_file() or installed.stat().st_size != entry.file_size:
                raise ValueError("Missing or size-mismatched CMake file: " + str(relative))
            with source.open(entry) as original, installed.open("rb") as extracted:
                if digest(original) != digest(extracted):
                    raise ValueError("Installed CMake file SHA256 mismatch: " + str(relative))
            expected_files.add(relative)
    if not expected_files:
        raise ValueError("CMake archive contains no files")
    if actual_files != expected_files:
        raise ValueError("Unexpected files in cached CMake installation")
    return {"archive": archive.name, "sha256": expected_sha256.lower(),
            "verified_files": len(expected_files)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--sha256", required=True)
    args = parser.parse_args()
    print(json.dumps(verify(args.archive, args.directory, args.sha256)))


if __name__ == "__main__":
    main()
