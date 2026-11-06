"""Merge compilation databases and preserve analyzer include/target configuration."""

import argparse
import json
from pathlib import Path
import shlex


def target_defines(path):
    """Preserve compiler target selectors needed by multiarch system headers."""
    names = {"__linux__", "__aarch64__", "__arm__", "__x86_64__", "__i386__",
             "__LP64__", "__ILP32__", "__riscv", "__riscv_xlen",
             "__AARCH64EL__", "__AARCH64EB__", "__ARM_EABI__", "__ARM_PCS_VFP"}
    defines = []
    for line in Path(path).read_text().splitlines():
        parts = line.split(maxsplit=2)
        if len(parts) == 3 and parts[0] == "#define" and parts[1] in names:
            defines.append(f"-D{parts[1]}={parts[2]}")
    if not defines:
        raise ValueError("No compiler target selectors found")
    return defines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--target-macros", help="Compiler -dM output for this build target")
    parser.add_argument("databases", nargs="+")
    args = parser.parse_args()
    defines = target_defines(args.target_macros) if args.target_macros else []

    entries = []
    for database in args.databases:
        for entry in json.loads(Path(database).read_text()):
            command = entry.get("arguments") or shlex.split(entry["command"])
            # Cppcheck 2.13 does not resolve all -isystem paths from a database.
            # Keep the same directories, making them explicit -I arguments.
            command = ["-I" if token == "-isystem" else token for token in command]
            command = command[:1] + defines + command[1:]
            entry.pop("command", None)
            entry["arguments"] = command
            entries.append(entry)
    if not entries:
        raise RuntimeError("No compilation entries were supplied")
    Path(args.output).write_text(json.dumps(entries, indent=2) + "\n")
    print(f"Prepared {len(entries)} compilation entries")


if __name__ == "__main__":
    main()
