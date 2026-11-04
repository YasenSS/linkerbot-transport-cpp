#!/usr/bin/env python3
"""Validate all twelve native SDK archives and write the 26-asset release index."""
import argparse
from pathlib import Path
import sys
sys.dont_write_bytecode = True
from release_manifest import build_manifest, write_manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--sha", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--targets", type=Path, default=Path("ci/package-targets.json"))
    parser.add_argument("--validation-run", required=True)
    args = parser.parse_args()
    manifest = build_manifest(args.directory, args.sha, args.version, args.targets, args.validation_run)
    write_manifest(args.directory, manifest)
    print("Verified %d archives; wrote release-manifest.json and SHA256SUMS.txt" % len(manifest["assets"]))


if __name__ == "__main__":
    main()
