#!/usr/bin/env python3
"""Seal tag validation or reuse an explicitly named successful sealed tag run."""
import argparse
import os
from pathlib import Path
import sys
sys.dont_write_bytecode = True
from release_provenance import GitHub, make_seal, reuse_validation


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("seal", "reuse"))
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--tag-object", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--targets", type=Path, default=Path("ci/package-targets.json"))
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    repository = os.environ["GITHUB_REPOSITORY"]
    sha, tag = os.environ["GITHUB_SHA"], os.environ["GITHUB_REF_NAME"]
    if os.environ.get("GITHUB_REF_TYPE") != "tag":
        raise ValueError("Release provenance requires an immutable version tag ref")
    client = GitHub(repository, os.environ["GH_TOKEN"])
    if args.mode == "seal":
        if (os.environ.get("GITHUB_EVENT_NAME") != "push" or args.run_id != os.environ["GITHUB_RUN_ID"]
                or args.output is None):
            raise ValueError("Only this exact tag-push workflow can create its validation seal")
        seal = make_seal(client, args.run_id, int(os.environ["GITHUB_RUN_ATTEMPT"]), repository, sha, tag,
                         args.tag_object, args.version, args.targets, args.assets, args.output)
    else:
        if os.environ.get("GITHUB_EVENT_NAME") != "workflow_dispatch" or args.output is not None:
            raise ValueError("Artifact promotion is only available to explicit manual draft runs")
        seal = reuse_validation(client, args.run_id, os.environ["GITHUB_RUN_ID"], repository, sha, tag,
                                args.tag_object, args.version, args.targets, args.assets)
    print("%s: validated source run %s attempt %s, twelve SDK artifacts" % (
        args.mode, seal["run_id"], seal["run_attempt"]))


if __name__ == "__main__":
    main()
