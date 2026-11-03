"""Require a complete, finding-free A+ report from all configured analyzers."""

import argparse
import json
from pathlib import Path


def validate(report):
    if report.get("mode") != "full" or report.get("analysis_complete") is not True:
        raise ValueError("A complete full-mode report is required")
    if report.get("grade") != "A+":
        raise ValueError(f"Expected A+, got {report.get('grade')!r}")
    checks = report.get("checks", [])
    expected = {"clang-format", "cppcheck", "clang-tidy"}
    if len(checks) != len(expected) or {check.get("tool") for check in checks} != expected:
        raise ValueError("Report must contain exactly the three configured analyzers")
    for check in checks:
        tool = check["tool"]
        analyzed, total = check.get("analyzed"), check.get("total")
        if (type(analyzed) is not int or type(total) is not int
                or total <= 0 or analyzed != total):
            raise ValueError(f"{tool}: incomplete analysis ({analyzed}/{total})")
        if check.get("status") != "passed" or check.get("diagnostics") != []:
            raise ValueError(f"{tool}: findings or tool failure remain")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    validate(report)
    print(f"Verified complete A+ report: score={report.get('score')}")


if __name__ == "__main__":
    main()
