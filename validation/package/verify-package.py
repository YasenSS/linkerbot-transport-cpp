#!/usr/bin/env python3
"""Verify an extracted native SDK; consumers must separately link and run it."""
import argparse
import json
from pathlib import Path
import sys
sys.dont_write_bytecode = True
from package_validation import archive_machines, verify_prefix


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--sha", required=True)
    parser.add_argument("--arch", required=True, choices=("x86_64", "aarch64", "x64", "ARM64"))
    parser.add_argument("--system", default="Linux", choices=("Linux", "Windows"))
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--distribution")
    parser.add_argument("--version")
    parser.add_argument("--require-clean", action="store_true")
    args = vars(parser.parse_args())
    root = args.pop("prefix")
    print(json.dumps(verify_prefix(root, **args), indent=2))


if __name__ == "__main__":
    main()
