"""Merge compilation databases and expose project system-include directories."""

import argparse
import json
from pathlib import Path
import shlex


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("databases", nargs="+")
    args = parser.parse_args()

    entries = []
    for database in args.databases:
        for entry in json.loads(Path(database).read_text()):
            command = entry.get("arguments") or shlex.split(entry["command"])
            # Cppcheck 2.13 does not resolve all -isystem paths from a database.
            # Keep the same directories, making them explicit -I arguments.
            command = ["-I" if token == "-isystem" else token for token in command]
            entry.pop("command", None)
            entry["arguments"] = command
            entries.append(entry)
    if not entries:
        raise RuntimeError("No compilation entries were supplied")
    Path(args.output).write_text(json.dumps(entries, indent=2) + "\n")
    print(f"Prepared {len(entries)} compilation entries")


if __name__ == "__main__":
    main()
