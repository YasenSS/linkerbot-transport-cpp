#!/usr/bin/env python3
"""Require every built PE executable/DLL to match the native test architecture."""
import argparse
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from package_validation import pe_machine


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--arch', required=True, choices=('x64', 'ARM64'))
    args = parser.parse_args()
    expected = {'x64': 0x8664, 'ARM64': 0xAA64}[args.arch]
    paths = sorted(p for p in args.root.rglob('*') if p.suffix.lower() in ('.exe', '.dll'))
    if not paths:
        raise ValueError('No executable/DLL images found')
    for path in paths:
        machine = pe_machine(path)
        if machine != expected:
            raise ValueError('%s: machine %#x != %#x' % (path, machine, expected))
    print(json.dumps({'architecture': args.arch, 'verified_pe_images': len(paths)}))


if __name__ == '__main__':
    main()
