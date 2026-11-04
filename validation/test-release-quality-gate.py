"""Exercise the release workflow's actual quality gate with deterministic API data."""
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import textwrap
import unittest
from unittest.mock import patch


WORKFLOW = Path(__file__).resolve().parents[1] / ".github/workflows/release.yml"
SHA = "1" * 40
REQUIRED = {"linux.yml": 12, "windows-minicanfd.yml": 4, "style.yml": 1,
            "cppreportcard.yml": 1, "focal-session.yml": 2}


class QualityGateTest(unittest.TestCase):
    def setUp(self):
        section = WORKFLOW.read_text().split("  quality-gate:", 1)[1].split("  packages:", 1)[0]
        source = section.split("python3 - <<'PYTHON'\n", 1)[1].split("          PYTHON", 1)[0]
        self.code = compile(textwrap.dedent(source), str(WORKFLOW), "exec")
        self.runs = [{"id": i, "path": ".github/workflows/" + name, "event": "push",
                      "status": "completed", "conclusion": "success",
                      "html_url": "https://example.invalid/runs/" + str(i)}
                     for i, name in enumerate(REQUIRED, 1)]
        self.jobs = {i: [{"conclusion": "success"} for _ in range(count)]
                     for i, count in enumerate(REQUIRED.values(), 1)}
        self.requests = []

    def response(self, request):
        url = request.full_url
        self.requests.append(url)
        base = "https://api.github.com/repos/example/repository/actions/runs"
        if url == base + "?head_sha=" + SHA + "&per_page=100":
            data = {"workflow_runs": self.runs}
        else:
            identities = {base + "/%s/jobs?per_page=100" % i: i for i in self.jobs}
            if url not in identities:
                raise ValueError("Unexpected or unfiltered API request: " + url)
            data = {"jobs": self.jobs[identities[url]]}
        return io.BytesIO(json.dumps(data).encode())

    def execute(self):
        with patch.dict(os.environ, {"GITHUB_API_URL": "https://api.github.com",
                                     "GITHUB_REPOSITORY": "example/repository",
                                     "GITHUB_SHA": SHA, "GH_TOKEN": "test-only"}), \
                patch("urllib.request.urlopen", side_effect=self.response), \
                contextlib.redirect_stdout(io.StringIO()):
            exec(self.code, {})

    def test_all_five_workflows_succeed(self):
        self.execute()
        self.assertTrue(any("/5/jobs?" in url for url in self.requests))

    def test_trusted_manual_focal_run_succeeds(self):
        self.runs[-1]["event"] = "workflow_dispatch"
        self.execute()

    def test_missing_focal_run_rejected(self):
        self.runs.pop()
        with self.assertRaises(AssertionError):
            self.execute()

    def test_failed_or_pending_focal_run_rejected(self):
        for status, conclusion in (("completed", "failure"), ("in_progress", None),
                                   ("queued", None), ("completed", "cancelled")):
            with self.subTest(status=status, conclusion=conclusion):
                self.runs[-1].update(status=status, conclusion=conclusion)
                with self.assertRaises(AssertionError):
                    self.execute()

    def test_failed_or_skipped_focal_job_rejected(self):
        for conclusion in ("failure", "skipped", "cancelled", None):
            with self.subTest(conclusion=conclusion):
                self.jobs[5][0]["conclusion"] = conclusion
                with self.assertRaises(AssertionError):
                    self.execute()

    def test_single_architecture_rejected(self):
        self.jobs[5].pop()
        with self.assertRaises(AssertionError):
            self.execute()

    def test_pr_only_focal_run_rejected(self):
        self.runs[-1]["event"] = "pull_request"
        with self.assertRaises(AssertionError):
            self.execute()

    def test_existing_quality_failure_still_rejected(self):
        self.runs[0]["conclusion"] = "failure"
        with self.assertRaises(AssertionError):
            self.execute()

    def test_latest_failed_run_cannot_fall_back_to_earlier_success(self):
        latest = copy.deepcopy(self.runs[-1])
        latest.update(id=6, conclusion="failure")
        self.runs.append(latest)
        with self.assertRaises(AssertionError):
            self.execute()


if __name__ == "__main__":
    unittest.main()
