"""Strict tag-run provenance and artifact transport for explicit release promotion."""
import datetime
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import stat
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from release_manifest import build_manifest, expected_targets, write_manifest

WORKFLOW_PATH = ".github/workflows/release.yml"
MAX_ZIP_BYTES = 64 * 1024 * 1024
MAX_UNPACKED_BYTES = 128 * 1024 * 1024
MAX_RELEASE_BYTES = 256 * 1024 * 1024


def timestamp(value):
    return datetime.datetime.fromisoformat(value.replace("Z", "+00:00"))


def expected_sdk_jobs(target_file, attempt=None):
    # expected_targets also rejects matrix omissions/duplicates in the shared file.
    expected_targets(target_file)
    data = json.loads(Path(target_file).read_text())
    jobs = {}
    for row in data["linux"]:
        jobs["sdk-linux-ubuntu%s-%s" % (row["ubuntu"], row["arch"])] = (
            "packages / Ubuntu %s native SDK (%s)" % (row["ubuntu"], row["arch"]))
    for row in data["windows"]:
        jobs["sdk-windows-%s-%s" % (row["arch"], row["configuration"])] = (
            "windows-packages / Native %s (%s)" % (row["arch"], row["configuration"]))
    if attempt is not None:
        jobs = {name + "-attempt-" + str(attempt): job for name, job in jobs.items()}
    return jobs


def required_success_jobs(target_file):
    names = set(expected_sdk_jobs(target_file).values())
    names.update(("validate-tag", "quality-gate", "packages / targets", "compatibility / matrix"))
    data = json.loads(Path(target_file).read_text())
    for row in data["linux"]:
        names.add("compatibility / Focal package on Ubuntu %s (%s)" % (row["ubuntu"], row["arch"]))
    names.update("compatibility / Noetic catkin node and nodelet (%s)" % arch for arch in ("x86_64", "aarch64"))
    return names


def validate_run(run, repository, sha, tag, sealing=False):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("Invalid expected repository")
    if not re.fullmatch(r"[0-9a-f]{40}", sha) or not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag):
        raise ValueError("Expected exact source SHA and version tag")
    repo = run.get("repository", {})
    head_repo = run.get("head_repository", {})
    if (repo.get("full_name") != repository or head_repo.get("full_name") != repository
            or type(repo.get("id")) is not int or repo.get("id") != head_repo.get("id")):
        raise ValueError("Source run must belong to this repository, never a fork")
    if run.get("event") != "push" or run.get("path") != WORKFLOW_PATH or run.get("head_branch") != tag:
        raise ValueError("Source must be an explicit tag-push release workflow run")
    if run.get("head_sha") != sha or run.get("pull_requests"):
        raise ValueError("Source run SHA/PR origin mismatch")
    if type(run.get("id")) is not int or type(run.get("run_attempt")) is not int or run["run_attempt"] < 1:
        raise ValueError("Invalid source run identity/attempt")
    if sealing:
        if run.get("status") != "in_progress" or run.get("conclusion") is not None:
            raise ValueError("Only the currently running tag validation can seal itself")
    elif run.get("status") != "completed" or run.get("conclusion") != "success":
        raise ValueError("Source validation run must be completed and successful")
    expected_refs = {"%s/.github/workflows/%s@%s" % (repository, path, sha)
                     for path in ("package.yml", "windows-minicanfd.yml", "compatibility.yml")}
    refs = run.get("referenced_workflows", [])
    if (len(refs) != 3 or {item.get("path") for item in refs} != expected_refs
            or any(item.get("sha") != sha or item.get("ref") != "refs/tags/" + tag for item in refs)):
        raise ValueError("Source reusable workflows must use this exact repository/tag/SHA")


def validate_jobs(jobs, attempt, target_file, sealing=False):
    by_name = {}
    for job in jobs:
        name = job.get("name")
        if name in by_name:
            raise ValueError("Duplicate source job name: %s" % name)
        if job.get("run_attempt") != attempt:
            raise ValueError("Source jobs mix workflow attempts")
        by_name[name] = job
    required = required_success_jobs(target_file)
    allowed = required | {"seal-validation", "draft", "compatibility / packages"}
    if set(by_name) - allowed or required - set(by_name):
        raise ValueError("Source validation job matrix is incomplete or unexpected")
    for name in required:
        job = by_name[name]
        if job.get("status") != "completed" or job.get("conclusion") != "success":
            raise ValueError("Source validation job did not succeed: " + name)
    for name in ("draft", "compatibility / packages"):
        if name in by_name and by_name[name].get("conclusion") != "skipped":
            raise ValueError("Unexpected source control-job result: " + name)
    seal = by_name.get("seal-validation", {})
    if sealing:
        if seal.get("status") != "in_progress" or seal.get("conclusion") is not None:
            raise ValueError("Seal job must be the current running job")
    elif seal.get("status") != "completed" or seal.get("conclusion") != "success":
        raise ValueError("Source run has no successful validation seal")
    return by_name


def artifact_record(artifact):
    return {key: artifact[key] for key in ("id", "name", "digest", "size_in_bytes", "created_at")}


def validate_artifact(artifact, run, producer, now):
    if (type(artifact.get("id")) is not int or artifact.get("expired") is not False
            or not re.fullmatch(r"sha256:[0-9a-f]{64}", artifact.get("digest", ""))):
        raise ValueError("Artifact is expired or lacks immutable identity/digest")
    if type(artifact.get("size_in_bytes")) is not int or not 0 < artifact["size_in_bytes"] <= MAX_ZIP_BYTES:
        raise ValueError("Unexpected artifact ZIP size")
    if timestamp(artifact["expires_at"]) <= now:
        raise ValueError("Artifact has expired")
    origin = artifact.get("workflow_run", {})
    expected = {"id": run["id"], "repository_id": run["repository"]["id"],
                "head_repository_id": run["repository"]["id"], "head_branch": run["head_branch"],
                "head_sha": run["head_sha"]}
    if any(origin.get(key) != value for key, value in expected.items()):
        raise ValueError("Artifact run/repository/tag/SHA origin mismatch")
    created = timestamp(artifact["created_at"])
    if created < timestamp(run["run_started_at"]) or created < timestamp(producer["started_at"]):
        raise ValueError("Artifact predates the validated run attempt or producer")
    if producer.get("completed_at") and created > timestamp(producer["completed_at"]):
        raise ValueError("Artifact was not uploaded by the validated producer job")


def select_sdk_artifacts(artifacts, run, jobs, target_file, now):
    attempt = run["run_attempt"]
    base_producers = expected_sdk_jobs(target_file)
    producers = expected_sdk_jobs(target_file, attempt)
    sdk = []
    for item in artifacts:
        name = item.get("name", "")
        if not name.startswith("sdk-"):
            continue
        match = re.fullmatch(r"(.+)-attempt-([1-9][0-9]*)", name)
        if not match or match.group(1) not in base_producers:
            raise ValueError("Unexpected or unscoped SDK artifact name")
        artifact_attempt = int(match.group(2))
        if artifact_attempt > attempt:
            raise ValueError("SDK artifact belongs to an unexpected future attempt")
        if artifact_attempt == attempt:
            sdk.append(item)
        # Retained earlier attempts are never selected, even if current SDKs
        # are missing. A full rerun can coexist with immutable older uploads.
    if len(sdk) != len(producers) or {item["name"] for item in sdk} != set(producers):
        raise ValueError("Source must retain exactly all twelve SDK artifacts for the current attempt, without duplicates")
    for item in sdk:
        validate_artifact(item, run, jobs[producers[item["name"]]], now)
    return sorted(sdk, key=lambda item: item["name"])


def unpack_artifact(data):
    """Read a bounded artifact ZIP with flat filenames; never extract paths."""
    if len(data) > MAX_ZIP_BYTES:
        raise ValueError("Artifact ZIP exceeds size limit")
    files = {}
    with zipfile.ZipFile(io.BytesIO(data)) as package:
        entries = package.infolist()
        if not 1 <= len(entries) <= 2 or sum(item.file_size for item in entries) > MAX_UNPACKED_BYTES:
            raise ValueError("Artifact ZIP member count/uncompressed size exceeds contract")
        for item in entries:
            path = PurePosixPath(item.filename)
            if (item.is_dir() or len(path.parts) != 1 or path.name in ("", ".", "..")
                    or "\\" in item.filename or ":" in item.filename or "\0" in item.filename
                    or item.filename != path.name or item.filename in files):
                raise ValueError("Unsafe or duplicate artifact ZIP member path")
            if stat.S_IFMT(item.external_attr >> 16) not in (0, stat.S_IFREG):
                raise ValueError("Artifact ZIP contains a link or special file")
            if item.file_size < 0 or item.file_size > MAX_UNPACKED_BYTES:
                raise ValueError("Artifact ZIP member is too large")
            value = package.read(item)
            if len(value) != item.file_size:
                raise ValueError("Artifact ZIP member size mismatch")
            files[item.filename] = value
    return files


class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme != "https":
            raise ValueError("Artifact redirects must use HTTPS")
        redirected = super().redirect_request(request, fp, code, message, headers, newurl)
        if urllib.parse.urlsplit(newurl).netloc != urllib.parse.urlsplit(request.full_url).netloc:
            redirected.remove_header("Authorization")
        return redirected


class GitHub:
    def __init__(self, repository, token):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) or not token:
            raise ValueError("Expected repository and read-only GitHub token")
        self.base = "https://api.github.com/repos/" + repository
        self.token = token
        self.opener = urllib.request.build_opener(SafeRedirect())

    def open(self, path):
        if not path.startswith("/") or ".." in path or ":" in path:
            raise ValueError("Invalid repository API path")
        request = urllib.request.Request(self.base + path, headers={
            "Authorization": "Bearer " + self.token, "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28"})
        return self.opener.open(request, timeout=60)

    def get(self, path):
        with self.open(path) as response:
            return json.load(response)

    def pages(self, path, key):
        values = []
        for page in range(1, 11):
            data = self.get(path + ("&" if "?" in path else "?") + "per_page=100&page=" + str(page))
            rows = data[key]
            values.extend(rows)
            if len(rows) < 100:
                return values
        raise ValueError("Repository API result exceeds bounded pagination")

    def download(self, artifact):
        try:
            with self.open("/actions/artifacts/%d/zip" % artifact["id"]) as response:
                data = response.read(MAX_ZIP_BYTES + 1)
        except urllib.error.HTTPError as error:
            raise ValueError("Artifact %s download failed (HTTP %s)" % (artifact["id"], error.code)) from None
        if len(data) != artifact["size_in_bytes"] or len(data) > MAX_ZIP_BYTES:
            raise ValueError("Downloaded artifact size mismatch")
        if "sha256:" + hashlib.sha256(data).hexdigest() != artifact["digest"]:
            raise ValueError("Downloaded artifact ZIP digest mismatch")
        return unpack_artifact(data)


def current_tag(client, tag, sha, tag_object):
    target = client.get("/git/ref/tags/" + tag)["object"]
    if target.get("sha") != tag_object:
        raise ValueError("Tag object changed since validation")
    for _ in range(8):
        if target.get("type") != "tag":
            break
        target = client.get("/git/tags/" + target["sha"])["object"]
    if target.get("type") != "commit" or target.get("sha") != sha:
        raise ValueError("Tag does not resolve to the exact validated source commit")


def file_records(root):
    return [{"file": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
             "bytes": path.stat().st_size} for path in sorted(Path(root).iterdir())]


def download_sdk(client, artifacts, root):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()):
        raise ValueError("SDK staging directory must be empty")
    total = 0
    for artifact in artifacts:
        files = client.download(artifact)
        archives = [name for name in files if name.endswith((".tar.gz", ".zip"))]
        if len(archives) != 1 or set(files) != {archives[0], archives[0] + ".sha256"}:
            raise ValueError("Each SDK artifact must contain exactly its archive and SHA256 sidecar")
        for name, data in files.items():
            total += len(data)
            if total > MAX_RELEASE_BYTES:
                raise ValueError("Combined SDK files exceed release size limit")
            with (root / name).open("xb") as output:
                output.write(data)


def load_run(client, run_id, repository, sha, tag, target_file, sealing=False):
    if not re.fullmatch(r"[1-9][0-9]*", str(run_id)):
        raise ValueError("validated_run_id must be an explicit positive run ID, never latest")
    run = client.get("/actions/runs/" + str(run_id))
    validate_run(run, repository, sha, tag, sealing)
    attempt = run["run_attempt"]
    attempt_run = client.get("/actions/runs/%s/attempts/%s" % (run_id, attempt))
    for key in ("id", "event", "path", "head_sha", "head_branch", "run_attempt", "status", "conclusion"):
        if attempt_run.get(key) != run.get(key):
            raise ValueError("Source run changed while selecting its exact attempt")
    run["run_started_at"] = attempt_run["run_started_at"]
    jobs = client.pages("/actions/runs/%s/attempts/%s/jobs" % (run_id, attempt), "jobs")
    by_name = validate_jobs(jobs, attempt, target_file, sealing)
    artifacts = client.pages("/actions/runs/%s/artifacts" % run_id, "artifacts")
    now = datetime.datetime.now(datetime.timezone.utc)
    sdk = select_sdk_artifacts(artifacts, run, by_name, target_file, now)
    return run, by_name, artifacts, sdk


def job_records(jobs, target_file):
    return {name: jobs[name]["id"] for name in sorted(required_success_jobs(target_file))}


def make_seal(client, run_id, attempt, repository, sha, tag, tag_object, version,
              target_file, assets, output):
    """Only called by the final job of the current, still-running tag push."""
    current_tag(client, tag, sha, tag_object)
    run, jobs, _, sdk = load_run(client, run_id, repository, sha, tag, target_file, sealing=True)
    if run["run_attempt"] != attempt:
        raise ValueError("Seal is not being created by this exact run attempt")
    download_sdk(client, sdk, assets)
    raw_files = file_records(assets)
    validation_run = "https://github.com/%s/actions/runs/%s" % (repository, run_id)
    manifest = build_manifest(assets, sha, version, target_file, validation_run)
    write_manifest(assets, manifest)
    manifest_bytes = (Path(assets) / "release-manifest.json").read_bytes()
    seal = {"schema_version": 1, "repository": repository, "repository_id": run["repository"]["id"],
            "run_id": int(run_id), "run_attempt": attempt, "workflow_path": WORKFLOW_PATH,
            "ref": "refs/tags/" + tag, "tag_object_sha": tag_object, "source_sha": sha,
            "version": version, "sdk_artifacts": [artifact_record(item) for item in sdk],
            "successful_job_ids": job_records(jobs, target_file), "validated_files": raw_files,
            "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
            "checksums_sha256": hashlib.sha256((Path(assets) / "SHA256SUMS.txt").read_bytes()).hexdigest()}
    # Recheck tag and all artifact identities after downloading/validating them.
    current_tag(client, tag, sha, tag_object)
    _, _, _, latest_sdk = load_run(client, run_id, repository, sha, tag, target_file, sealing=True)
    if [artifact_record(item) for item in latest_sdk] != seal["sdk_artifacts"]:
        raise ValueError("SDK artifact set changed while sealing validation")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("Seal output directory must be empty")
    (output / "validation.json").write_text(json.dumps(seal, indent=2, sort_keys=True) + "\n")
    (output / "release-manifest.json").write_bytes(manifest_bytes)
    return seal


def validate_seal(seal, manifest_bytes, run, jobs, sdk, repository, sha, tag,
                  tag_object, version, target_file):
    expected = {"schema_version": 1, "repository": repository, "repository_id": run["repository"]["id"],
                "run_id": run["id"], "run_attempt": run["run_attempt"], "workflow_path": WORKFLOW_PATH,
                "ref": "refs/tags/" + tag, "tag_object_sha": tag_object, "source_sha": sha, "version": version}
    for key, value in expected.items():
        if type(seal.get(key)) is not type(value) or seal.get(key) != value:
            raise ValueError("Validation seal identity mismatch: " + key)
    if seal.get("sdk_artifacts") != [artifact_record(item) for item in sdk]:
        raise ValueError("Validation seal SDK artifact IDs/digests changed")
    if seal.get("successful_job_ids") != job_records(jobs, target_file):
        raise ValueError("Validation seal job identities do not match the selected attempt")
    if seal.get("manifest_sha256") != hashlib.sha256(manifest_bytes).hexdigest():
        raise ValueError("Sealed package manifest digest mismatch")


def reuse_validation(client, run_id, current_run_id, repository, sha, tag, tag_object,
                     version, target_file, assets):
    """Promote only a fully sealed, successful same-repository tag-push run."""
    if str(run_id) == str(current_run_id):
        raise ValueError("A draft run cannot validate itself")
    current_tag(client, tag, sha, tag_object)
    run, jobs, artifacts, sdk = load_run(client, run_id, repository, sha, tag, target_file)
    name = "release-validation-attempt-%s" % run["run_attempt"]
    seals = [item for item in artifacts if item.get("name") == name]
    if len(seals) != 1:
        raise ValueError("Source attempt has no unique validation seal; legacy/unsealed runs cannot be promoted")
    seal_artifact = seals[0]
    validate_artifact(seal_artifact, run, jobs["seal-validation"], datetime.datetime.now(datetime.timezone.utc))
    proof = client.download(seal_artifact)
    if set(proof) != {"validation.json", "release-manifest.json"}:
        raise ValueError("Unexpected validation seal artifact contents")
    seal = json.loads(proof["validation.json"])
    validate_seal(seal, proof["release-manifest.json"], run, jobs, sdk, repository, sha, tag,
                  tag_object, version, target_file)
    download_sdk(client, sdk, assets)
    if file_records(assets) != seal.get("validated_files"):
        raise ValueError("Original SDK archive/sidecar bytes differ from their validated seal")
    validation_run = "https://github.com/%s/actions/runs/%s" % (repository, run_id)
    manifest = build_manifest(assets, sha, version, target_file, validation_run)
    if manifest != json.loads(proof["release-manifest.json"]):
        raise ValueError("Revalidated package metadata differs from the original package manifest")
    write_manifest(assets, manifest)
    if ((Path(assets) / "release-manifest.json").read_bytes() != proof["release-manifest.json"]
            or hashlib.sha256((Path(assets) / "SHA256SUMS.txt").read_bytes()).hexdigest() != seal["checksums_sha256"]):
        raise ValueError("Promoted release manifest/checksum bytes changed")
    # Reject a concurrent rerun, artifact deletion/replacement, or tag movement.
    current_tag(client, tag, sha, tag_object)
    latest_run, latest_jobs, latest_artifacts, latest_sdk = load_run(client, run_id, repository, sha, tag, target_file)
    latest_seals = [item for item in latest_artifacts if item.get("name") == name]
    if len(latest_seals) != 1 or artifact_record(latest_seals[0]) != artifact_record(seal_artifact):
        raise ValueError("Validation seal artifact changed while promoting")
    validate_seal(seal, proof["release-manifest.json"], latest_run, latest_jobs, latest_sdk,
                  repository, sha, tag, tag_object, version, target_file)
    return seal
