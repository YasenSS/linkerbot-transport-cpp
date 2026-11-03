"""Negative controls for the quality-report gate; no external dependencies."""

import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "quality_report", Path(__file__).with_name("check-quality-report.py")
)
quality_report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(quality_report)


class ReportGateTests(unittest.TestCase):
    def setUp(self):
        self.report = {
            "mode": "full", "analysis_complete": True, "grade": "A+", "score": 100,
            "checks": [
                {"tool": tool, "status": "passed", "analyzed": 2, "total": 2,
                 "diagnostics": []}
                for tool in ("clang-format", "cppcheck", "clang-tidy")
            ],
        }

    def test_complete_report_passes(self):
        quality_report.validate(self.report)

    def test_incomplete_or_basic_report_fails(self):
        for field, value in (("mode", "basic"), ("analysis_complete", False)):
            with self.subTest(field=field):
                report = copy.deepcopy(self.report)
                report[field] = value
                with self.assertRaises(ValueError):
                    quality_report.validate(report)

    def test_lower_grade_fails(self):
        self.report["grade"] = "A"
        with self.assertRaises(ValueError):
            quality_report.validate(self.report)

    def test_missing_or_duplicate_analyzer_fails(self):
        for checks in (self.report["checks"][:-1], [self.report["checks"][0]] * 3):
            with self.subTest(checks=checks), self.assertRaises(ValueError):
                quality_report.validate({**self.report, "checks": checks})

    def test_incomplete_unit_counts_fail(self):
        for analyzed, total in ((1, 2), (0, 0), (True, 1), (2, "2")):
            with self.subTest(analyzed=analyzed, total=total):
                report = copy.deepcopy(self.report)
                report["checks"][0].update(analyzed=analyzed, total=total)
                with self.assertRaises(ValueError):
                    quality_report.validate(report)

    def test_warning_or_tool_failure_cannot_hide_behind_grade(self):
        for update in ({"diagnostics": [{"severity": "warning"}]},
                       {"status": "tool-error"}):
            with self.subTest(update=update):
                report = copy.deepcopy(self.report)
                report["checks"][0].update(update)
                with self.assertRaises(ValueError):
                    quality_report.validate(report)


if __name__ == "__main__":
    unittest.main()
