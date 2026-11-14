#!/usr/bin/env python3
"""Start/resume a reviewed Xcode Cloud build and download its notarized app."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import time
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "fusepb/ci_scripts"))
from release_version import release_version, build_number
import urllib.error
import urllib.parse
import urllib.request
from uuid import UUID

API = "https://api.appstoreconnect.apple.com/v1/"


def validated_workflow_id(value):
    value = (value or '').strip()
    if not value:
        raise ValueError('XCODE_CLOUD_WORKFLOW_ID is missing. Configure the Apple workflow ID in the macos-release environment.')
    try:
        UUID(value)
        # Apple resource IDs are opaque: validate without changing their casing.
        return value
    except ValueError:
        raise ValueError('XCODE_CLOUD_WORKFLOW_ID must be a valid workflow UUID from the Xcode Cloud workflow URL') from None


class AppleAPIError(RuntimeError):
    def __init__(self, status, path, body=b"", secrets=()):
        self.status = status
        message = f"Apple API returned HTTP {status} for {path.split('?')[0]}"
        # Only log Apple's diagnostic fields, never raw responses or headers.
        try:
            document = json.loads(body)
        except (ValueError, UnicodeDecodeError):
            document = None
        errors = document.get("errors") if isinstance(document, dict) else None
        if isinstance(errors, list):
            for error in errors[:5]:
                if not isinstance(error, dict):
                    continue
                fields = [f"{field}={error[field]}" for field in ("code", "title", "detail")
                          if isinstance(error.get(field), str)]
                if fields:
                    message += "; " + " | ".join(fields)
        for secret in secrets:
            if secret:
                message = message.replace(secret, "[REDACTED]")
        # Keep server text on one bounded line in CI logs.
        super().__init__(" ".join(message.split())[:4096])


class AppleAPI:
    def __init__(self):
        self.key = os.environ["APP_STORE_CONNECT_API_KEY"]
        self.key_id = os.environ["APP_STORE_CONNECT_KEY_ID"]
        self.issuer = os.environ["APP_STORE_CONNECT_ISSUER_ID"]

    def request(self, path, payload=None):
        import jwt

        url = urllib.parse.urljoin(API, path)
        if not url.startswith(API):
            raise ValueError("Unexpected Apple API URL")
        now = int(time.time())
        token = jwt.encode(
            {"iss": self.issuer, "iat": now, "exp": now + 600,
             "aud": "appstoreconnect-v1"},
            self.key, algorithm="ES256", headers={"kid": self.key_id},
        )
        request = urllib.request.Request(
            url, data=json.dumps(payload).encode() if payload is not None else None,
            headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
        )
        # Never retry a build-creation POST: its outcome may be unknown.
        for attempt in range(4):
            try:
                with urllib.request.urlopen(request, timeout=60) as response:
                    return json.load(response)
            except urllib.error.HTTPError as error:
                if payload is None and error.code in (429, 500, 502, 503, 504) and attempt < 3:
                    time.sleep(10 * (attempt + 1))
                    continue
                body = error.read(65536)
                raise AppleAPIError(error.code, path, body,
                                    (self.key, self.key_id, self.issuer, token)) from None

    def items(self, path):
        while path:
            response = self.request(path)
            yield from response["data"]
            path = response.get("links", {}).get("next")


def verify_run(run, workflow_id, expected_sha):
    workflow = run["relationships"]["workflow"]["data"]["id"]
    if workflow.lower() != workflow_id.lower():
        raise ValueError("Build belongs to a different workflow")
    actual = (run["attributes"].get("sourceCommit") or {}).get("commitSha")
    if actual and actual.lower() != expected_sha.lower():
        raise ValueError(f"Apple build commit {actual} does not match {expected_sha}")
    return actual


def resolve_workflow(api, workflow_id):
    try:
        return api.request(f"ciWorkflows/{workflow_id}")["data"]["id"]
    except AppleAPIError as error:
        if error.status != 404:
            raise
    # The web URL's UUID may differ in casing from the API's resource ID.
    # Discover the exact ID without choosing a different workflow by name.
    matches = []
    for product in api.items("ciProducts?limit=200"):
        for workflow in api.items(f"ciProducts/{product['id']}/workflows?limit=200"):
            if workflow["id"].lower() == workflow_id.lower():
                matches.append(workflow["id"])
    if len(matches) != 1:
        raise ValueError("Configured Xcode Cloud workflow is not uniquely visible to this API key. "
                         "Check the workflow URL and that the API key/issuer belong to its Apple team "
                         "and have access to Xcode Cloud.")
    return matches[0]


def start_build(api, workflow_id, source_ref):
    repository = api.request(f"ciWorkflows/{workflow_id}/repository")["data"]["id"]
    matches = [item for item in api.items(f"scmRepositories/{repository}/gitReferences?limit=200")
               if item["attributes"].get("canonicalName") == source_ref
               and not item["attributes"].get("isDeleted")]
    if len(matches) != 1:
        raise ValueError(f"Expected one Apple Git reference for {source_ref}; found {len(matches)}")
    return api.request("ciBuildRuns", {"data": {
        "type": "ciBuildRuns", "attributes": {"clean": True},
        "relationships": {
            "workflow": {"data": {"type": "ciWorkflows", "id": workflow_id}},
            "sourceBranchOrTag": {"data": {"type": "scmGitReferences", "id": matches[0]["id"]}},
        },
    }})["data"]["id"]


def wait_for_run(api, run_id, workflow_id, expected_sha, timeout):
    deadline = time.monotonic() + timeout
    previous = None
    while True:
        run = api.request(f"ciBuildRuns/{run_id}?include=workflow")["data"]
        sha = verify_run(run, workflow_id, expected_sha)
        attributes = run["attributes"]
        state = (attributes.get("executionProgress"), attributes.get("completionStatus"))
        if state != previous:
            print(f"Xcode Cloud build {run_id}: {state[0]} / {state[1]}", flush=True)
            previous = state
        if state[0] == "COMPLETE":
            if state[1] != "SUCCEEDED":
                raise RuntimeError(f"Xcode Cloud build finished with {state[1]}; inspect its Apple build logs")
            if not sha:
                raise ValueError("Completed build has no source commit to verify")
            return run
        if time.monotonic() >= deadline:
            raise TimeoutError(f"Apple is still processing. Resume using build_run_id={run_id}")
        time.sleep(min(30, max(0, deadline - time.monotonic())))


def notarized_artifact(api, run_id):
    artifacts = []
    for action in api.items(f"ciBuildRuns/{run_id}/actions?limit=200"):
        artifacts.extend(api.items(f"ciBuildActions/{action['id']}/artifacts?limit=200"))
    matches = {item["id"]: item for item in artifacts
               if item["attributes"].get("fileType") == "STAPLED_NOTARIZED_ARCHIVE"}
    if len(matches) != 1:
        types = sorted({item["attributes"].get("fileType", "UNKNOWN") for item in artifacts})
        raise ValueError(f"Expected one stapled notarized archive; found {len(matches)}. Artifact types: {types}")
    return next(iter(matches.values()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", required=True, help="Canonical Git ref, e.g. refs/tags/1.10.0-fusex-0.2")
    parser.add_argument("--sha", required=True)
    parser.add_argument("--resume", default="")
    parser.add_argument("--timeout", type=int, default=5400)
    parser.add_argument("--output", type=Path, default=Path("cloud-output"))
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-fA-F]{40}", args.sha):
        parser.error("Expected a full commit SHA")
    if not args.ref.startswith("refs/tags/"):
        parser.error("Use a canonical tag ref")
    try:
        workflow = validated_workflow_id(os.environ.get("XCODE_CLOUD_WORKFLOW_ID"))
    except ValueError as error:
        parser.error(str(error))
    api = AppleAPI()
    workflow = resolve_workflow(api, workflow)
    args.output.mkdir(parents=True, exist_ok=True)
    run_id = args.resume or start_build(api, workflow, args.ref)
    metadata = {"build_run_id": run_id, "workflow_id": workflow,
                "source_ref": args.ref, "commit_sha": args.sha}
    (args.output / "build.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"Apple build ID: {run_id}", flush=True)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as summary:
            summary.write(f"Xcode Cloud build: `{run_id}`\n\nCommit: `{args.sha}`\n\n"
                          f"To resume this build, use the same source ref and set `build_run_id` to `{run_id}`.\n")
    run = wait_for_run(api, run_id, workflow, args.sha, args.timeout)
    metadata["build_number"] = build_number(run["attributes"]["number"])
    metadata["release_version"] = release_version(args.ref)
    artifact = notarized_artifact(api, run_id)
    # Refresh the short-lived download link; never send the API token to storage.
    attributes = api.request(f"ciArtifacts/{artifact['id']}")["data"]["attributes"]
    url = attributes["downloadUrl"]
    if urllib.parse.urlparse(url).scheme != "https":
        raise ValueError("Artifact download must use HTTPS")
    with urllib.request.urlopen(url, timeout=120) as response:
        with (args.output / "notarized.zip").open("wb") as destination:
            shutil.copyfileobj(response, destination)
    metadata["artifact_id"] = artifact["id"]
    metadata["artifact_type"] = attributes["fileType"]
    (args.output / "build.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print("Downloaded the stapled notarized archive.")


if __name__ == "__main__":
    main()
