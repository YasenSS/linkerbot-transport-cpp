#!/usr/bin/env python3
"""Mocked API/real ZIP and package-byte tests for explicit sealed-run promotion."""
import copy
import datetime
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import stat
import sys
import unittest
from unittest.mock import patch
import urllib.request
import warnings
import zipfile
sys.dont_write_bytecode = True
import release_provenance as provenance

spec = importlib.util.spec_from_file_location("manifest_fixtures", Path(__file__).with_name("test-release-manifest.py"))
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)
SHA, TAG_OBJECT, TAG, REPOSITORY, RUN_ID = fixtures.SHA, "b" * 40, "v" + fixtures.VERSION, "test/repo", 123


def zip_bytes(files):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as package:
        for name, value in files.items():
            package.writestr(name, value)
    return output.getvalue()


class FakeGitHub(provenance.GitHub):
    def __init__(self, fixture):
        self.fixture = fixture
        self.tag_object = TAG_OBJECT
        self.now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
        self.run = {"id": RUN_ID, "repository": {"id": 17, "full_name": REPOSITORY},
                    "head_repository": {"id": 17, "full_name": REPOSITORY}, "head_sha": SHA,
                    "head_branch": TAG, "event": "push", "path": provenance.WORKFLOW_PATH,
                    "status": "in_progress", "conclusion": None, "run_attempt": 1,
                    "pull_requests": [], "run_started_at": self.time(-300),
                    "referenced_workflows": [{"path": REPOSITORY + "/.github/workflows/" + name + "@" + SHA,
                                               "sha": SHA, "ref": "refs/tags/" + TAG}
                                              for name in ("package.yml", "windows-minicanfd.yml", "compatibility.yml")]}
        self.jobs = [{"id": 1000 + index, "name": name, "run_attempt": 1, "status": "completed",
                      "conclusion": "success", "started_at": self.time(-240), "completed_at": self.time(-120)}
                     for index, name in enumerate(sorted(provenance.required_success_jobs(fixture.target_file)))]
        self.jobs.extend([{"id": 2000, "name": "seal-validation", "run_attempt": 1,
                           "status": "in_progress", "conclusion": None, "started_at": self.time(-60), "completed_at": None},
                          {"id": 2001, "name": "draft", "run_attempt": 1, "status": "completed", "conclusion": "skipped"},
                          {"id": 2002, "name": "compatibility / packages", "run_attempt": 1,
                           "status": "completed", "conclusion": "skipped"}])
        self.artifacts = []
        self.blobs = {}
        self.on_download = None
        for index, row in enumerate(fixture.rows):
            info = row["info"]
            name = ("sdk-linux-%s-%s" % (info["distribution"], info["architecture"]) if info["system"] == "Linux"
                    else "sdk-windows-%s-%s" % (info["architecture"], info["configuration"]))
            files = {row["name"]: (fixture.root / row["name"]).read_bytes(),
                     row["name"] + ".sha256": (fixture.root / (row["name"] + ".sha256")).read_bytes()}
            self.add_artifact(3000 + index, name + "-attempt-1", zip_bytes(files), -180)

    def time(self, delta):
        return (self.now + datetime.timedelta(seconds=delta)).isoformat().replace("+00:00", "Z")

    def add_artifact(self, identity, name, value, created):
        self.blobs[identity] = value
        self.artifacts.append({"id": identity, "name": name, "size_in_bytes": len(value),
                               "digest": "sha256:" + hashlib.sha256(value).hexdigest(), "expired": False,
                               "created_at": self.time(created), "expires_at": self.time(86400),
                               "workflow_run": {"id": RUN_ID, "repository_id": 17, "head_repository_id": 17,
                                                "head_branch": TAG, "head_sha": SHA}})

    def get(self, path):
        if path == "/git/ref/tags/" + TAG:
            return {"object": {"type": "tag", "sha": self.tag_object}}
        if path == "/git/tags/" + self.tag_object:
            return {"object": {"type": "commit", "sha": SHA}}
        if re.fullmatch(r"/actions/runs/123(?:/attempts/[0-9]+)?", path):
            return copy.deepcopy(self.run)
        if re.fullmatch(r"/actions/runs/123/attempts/[0-9]+/jobs\?per_page=100&page=1", path):
            return {"jobs": copy.deepcopy(self.jobs)}
        if path == "/actions/runs/123/artifacts?per_page=100&page=1":
            return {"artifacts": copy.deepcopy(self.artifacts)}
        raise AssertionError("Unexpected API call: " + path)

    def open(self, path):
        identity = int(re.fullmatch(r"/actions/artifacts/([0-9]+)/zip", path).group(1))
        if self.on_download:
            self.on_download(identity)
        return io.BytesIO(self.blobs[identity])


class Promotion(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.ReleaseManifest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.client = FakeGitHub(self.fixture)
        self.output = self.fixture.directory / "seal"
        self.sealed_assets = self.fixture.directory / "validated"
        self.seal = provenance.make_seal(self.client, RUN_ID, 1, REPOSITORY, SHA, TAG, TAG_OBJECT,
                                         fixtures.VERSION, self.fixture.target_file, self.sealed_assets, self.output)
        self.client.run.update(status="completed", conclusion="success")
        seal_job = next(job for job in self.client.jobs if job["name"] == "seal-validation")
        seal_job.update(status="completed", conclusion="success", completed_at=self.client.time(-10))
        self.client.add_artifact(4000, "release-validation-attempt-1", zip_bytes({
            "validation.json": (self.output / "validation.json").read_bytes(),
            "release-manifest.json": (self.output / "release-manifest.json").read_bytes()}), -20)

    def promote(self, run_id=RUN_ID):
        return provenance.reuse_validation(self.client, run_id, 999, REPOSITORY, SHA, TAG, TAG_OBJECT,
                                           fixtures.VERSION, self.fixture.target_file,
                                           self.fixture.directory / "promoted")

    def test_exact_original_26_asset_bytes(self):
        result = self.promote()
        self.assertEqual(result["run_id"], RUN_ID)
        promoted = self.fixture.directory / "promoted"
        self.assertEqual(len(list(promoted.iterdir())), 26)
        for path in self.sealed_assets.iterdir():
            self.assertEqual(path.read_bytes(), (promoted / path.name).read_bytes())
        self.assertEqual(json.loads((promoted / "release-manifest.json").read_text())["validation_run"], fixtures.RUN_URL)

    def prepare_full_attempt_two(self):
        self.client.run.update(run_attempt=2, status="in_progress", conclusion=None,
                               run_started_at=self.client.time(-100))
        for job in self.client.jobs:
            job["id"] += 10000
            job["run_attempt"] = 2
            if job["name"] == "seal-validation":
                job.update(status="in_progress", conclusion=None, started_at=self.client.time(-30), completed_at=None)
            elif job["conclusion"] == "success":
                job.update(started_at=self.client.time(-90), completed_at=self.client.time(-40))
        # Retain all first-attempt SDKs and the first seal. New package bytes
        # deliberately differ, proving the selector cannot silently use old SDKs.
        for index, row in enumerate(self.fixture.rows):
            row["info"]["build_image"] = "attempt-two-image"
            self.fixture.write(row)
            info = row["info"]
            name = ("sdk-linux-%s-%s" % (info["distribution"], info["architecture"]) if info["system"] == "Linux"
                    else "sdk-windows-%s-%s" % (info["architecture"], info["configuration"]))
            files = {row["name"]: (self.fixture.root / row["name"]).read_bytes(),
                     row["name"] + ".sha256": (self.fixture.root / (row["name"] + ".sha256")).read_bytes()}
            self.client.add_artifact(13000 + index, name + "-attempt-2", zip_bytes(files), -60)
        output = self.fixture.directory / "seal-two"
        assets = self.fixture.directory / "validated-two"
        seal = provenance.make_seal(self.client, RUN_ID, 2, REPOSITORY, SHA, TAG, TAG_OBJECT,
                                    fixtures.VERSION, self.fixture.target_file, assets, output)
        self.client.run.update(status="completed", conclusion="success")
        next(job for job in self.client.jobs if job["name"] == "seal-validation").update(
            status="completed", conclusion="success", completed_at=self.client.time(-10))
        self.client.add_artifact(14000, "release-validation-attempt-2", zip_bytes({
            "validation.json": (output / "validation.json").read_bytes(),
            "release-manifest.json": (output / "release-manifest.json").read_bytes()}), -15)
        return seal, assets

    def test_full_attempt_two_retains_but_never_reuses_attempt_one(self):
        seal, expected_assets = self.prepare_full_attempt_two()
        result = self.promote()
        self.assertEqual(result["run_attempt"], 2)
        self.assertEqual(len([item for item in self.client.artifacts if item["name"].startswith("sdk-")]), 24)
        self.assertTrue(all(item["name"].endswith("-attempt-2") for item in seal["sdk_artifacts"]))
        promoted = self.fixture.directory / "promoted"
        for path in expected_assets.iterdir():
            self.assertEqual(path.read_bytes(), (promoted / path.name).read_bytes())
        manifest = json.loads((promoted / "release-manifest.json").read_text())
        self.assertTrue(all(item["build"]["build_image"] == "attempt-two-image" for item in manifest["assets"]))

    def test_partial_attempt_two_cannot_reuse_attempt_one_sdk(self):
        self.prepare_full_attempt_two()
        self.client.artifacts = [item for item in self.client.artifacts if item["id"] != 13000]
        with self.assertRaisesRegex(ValueError, "all twelve SDK artifacts for the current attempt"):
            self.promote()

    def test_partial_attempt_two_cannot_reuse_attempt_one_jobs(self):
        self.prepare_full_attempt_two()
        self.client.jobs[0]["run_attempt"] = 1
        with self.assertRaisesRegex(ValueError, "mix workflow attempts"):
            self.promote()

    def test_reject_unknown_current_attempt_sdk(self):
        self.client.artifacts[0]["name"] = "sdk-unknown-attempt-1"
        with self.assertRaisesRegex(ValueError, "Unexpected or unscoped"):
            self.promote()

    def test_reject_unscoped_sdk(self):
        self.client.artifacts[0]["name"] = self.client.artifacts[0]["name"][:-len("-attempt-1")]
        with self.assertRaisesRegex(ValueError, "Unexpected or unscoped"):
            self.promote()

    def test_reject_pr_source(self):
        self.client.run["event"] = "pull_request"
        with self.assertRaisesRegex(ValueError, "tag-push"):
            self.promote()

    def test_reject_manual_source(self):
        self.client.run["event"] = "workflow_dispatch"
        with self.assertRaisesRegex(ValueError, "tag-push"):
            self.promote()

    def test_reject_fork_source(self):
        self.client.run["head_repository"] = {"id": 999, "full_name": "fork/repo"}
        with self.assertRaisesRegex(ValueError, "never a fork"):
            self.promote()

    def test_reject_wrong_sha(self):
        self.client.run["head_sha"] = "c" * 40
        with self.assertRaisesRegex(ValueError, "SHA/PR origin"):
            self.promote()

    def test_reject_wrong_tag(self):
        self.client.run["head_branch"] = "main"
        with self.assertRaisesRegex(ValueError, "tag-push"):
            self.promote()

    def test_reject_wrong_workflow(self):
        self.client.run["path"] = ".github/workflows/package.yml"
        with self.assertRaisesRegex(ValueError, "tag-push"):
            self.promote()

    def test_reject_wrong_reusable_ref(self):
        self.client.run["referenced_workflows"][0]["ref"] = "refs/heads/main"
        with self.assertRaisesRegex(ValueError, "reusable workflows"):
            self.promote()

    def test_reject_unsuccessful_run(self):
        self.client.run["conclusion"] = "failure"
        with self.assertRaisesRegex(ValueError, "completed and successful"):
            self.promote()

    def test_reject_latest_or_empty_source(self):
        for value in ("latest", "", "0", "-1", "12/../123"):
            with self.assertRaises(ValueError):
                self.promote(value)

    def test_reject_mixed_attempt_jobs(self):
        self.client.jobs[0]["run_attempt"] = 2
        with self.assertRaisesRegex(ValueError, "mix workflow attempts"):
            self.promote()

    def test_reject_missing_matrix_job(self):
        self.client.jobs.pop(0)
        with self.assertRaisesRegex(ValueError, "matrix is incomplete"):
            self.promote()

    def test_reject_skipped_validation_job(self):
        self.client.jobs[0]["conclusion"] = "skipped"
        with self.assertRaisesRegex(ValueError, "did not succeed"):
            self.promote()

    def test_reject_duplicate_job_name(self):
        self.client.jobs.append(copy.deepcopy(self.client.jobs[0]))
        with self.assertRaisesRegex(ValueError, "Duplicate source job"):
            self.promote()

    def test_reject_missing_sdk(self):
        self.client.artifacts.pop(0)
        with self.assertRaisesRegex(ValueError, "all twelve SDK"):
            self.promote()

    def test_reject_duplicate_sdk_name(self):
        self.client.artifacts.append(copy.deepcopy(self.client.artifacts[0]))
        with self.assertRaisesRegex(ValueError, "all twelve SDK"):
            self.promote()

    def test_reject_expired_sdk(self):
        self.client.artifacts[0]["expired"] = True
        with self.assertRaisesRegex(ValueError, "expired"):
            self.promote()

    def test_reject_expired_seal(self):
        self.client.artifacts[-1]["expires_at"] = self.client.time(-1)
        with self.assertRaisesRegex(ValueError, "expired"):
            self.promote()

    def test_reject_artifact_from_other_run(self):
        self.client.artifacts[0]["workflow_run"]["id"] = 456
        with self.assertRaisesRegex(ValueError, "origin mismatch"):
            self.promote()

    def test_reject_old_attempt_artifact(self):
        self.client.artifacts[0]["created_at"] = self.client.time(-3600)
        with self.assertRaisesRegex(ValueError, "predates"):
            self.promote()

    def test_reject_tag_object_recreated_at_same_commit(self):
        self.client.tag_object = "c" * 40
        with self.assertRaisesRegex(ValueError, "Tag object changed"):
            self.promote()

    def test_reject_unsealed_legacy_run(self):
        self.client.artifacts.pop()
        with self.assertRaisesRegex(ValueError, "legacy/unsealed"):
            self.promote()

    def test_reject_corrupt_download_bytes(self):
        identity = self.client.artifacts[0]["id"]
        blob = self.client.blobs[identity]
        self.client.blobs[identity] = blob[:-1] + bytes([blob[-1] ^ 1])
        with self.assertRaisesRegex(ValueError, "ZIP digest mismatch"):
            self.promote()

    def test_reject_replaced_artifact_identity(self):
        self.client.artifacts[0]["id"] = 9999
        with self.assertRaisesRegex(ValueError, "artifact IDs/digests changed"):
            self.promote()

    def test_recheck_tag_after_download(self):
        last_id = sorted(self.client.artifacts[:-1], key=lambda item: item["name"])[-1]["id"]
        def change(identity):
            if identity == last_id:
                self.client.tag_object = "c" * 40
        self.client.on_download = change
        with self.assertRaisesRegex(ValueError, "Tag object changed"):
            self.promote()

    def test_recheck_attempt_after_download(self):
        last_id = sorted(self.client.artifacts[:-1], key=lambda item: item["name"])[-1]["id"]
        def change(identity):
            if identity == last_id:
                self.client.run["run_attempt"] = 2
        self.client.on_download = change
        with self.assertRaisesRegex(ValueError, "mix workflow attempts"):
            self.promote()


class ArtifactTransport(unittest.TestCase):
    def test_reject_traversal_and_directories(self):
        for name in ("../escape", "/escape", "directory/file", "C:escape", "x\\escape"):
            with self.assertRaises(ValueError):
                provenance.unpack_artifact(zip_bytes({name: b"data"}))

    def test_reject_link(self):
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as package:
            member = zipfile.ZipInfo("link")
            member.external_attr = (stat.S_IFLNK | 0o777) << 16
            package.writestr(member, "elsewhere")
        with self.assertRaisesRegex(ValueError, "link or special"):
            provenance.unpack_artifact(output.getvalue())

    def test_reject_zip_bomb_size(self):
        value = zip_bytes({"large": b"0" * 64})
        with patch.object(provenance, "MAX_UNPACKED_BYTES", 32):
            with self.assertRaisesRegex(ValueError, "uncompressed size"):
                provenance.unpack_artifact(value)

    def test_reject_duplicate_zip_name(self):
        output = io.BytesIO()
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            with zipfile.ZipFile(output, "w") as package:
                package.writestr("same", b"one")
                package.writestr("same", b"two")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            provenance.unpack_artifact(output.getvalue())

    def test_redirect_drops_token(self):
        request = urllib.request.Request("https://api.github.com/repos/test/repo/actions/artifacts/1/zip",
                                         headers={"Authorization": "Bearer secret"})
        redirected = provenance.SafeRedirect().redirect_request(request, None, 302, "Found", {}, "https://blob.example/data")
        self.assertIsNone(redirected.get_header("Authorization"))

    def test_redirect_rejects_http(self):
        request = urllib.request.Request("https://api.github.com/data")
        with self.assertRaisesRegex(ValueError, "HTTPS"):
            provenance.SafeRedirect().redirect_request(request, None, 302, "Found", {}, "http://blob.example/data")


if __name__ == "__main__":
    unittest.main()
