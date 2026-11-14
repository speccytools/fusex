"""Regression checks for release provenance and artifact selection."""

import unittest
import contextlib
import io
import json
import urllib.error
from unittest.mock import Mock, patch

from xcode_cloud import AppleAPI, AppleAPIError, main, notarized_artifact, resolve_workflow, start_build, verify_run, wait_for_run, validated_workflow_id


class FakeAPI:
    def __init__(self, documents=None, collections=None):
        self.documents = documents or {}
        self.collections = collections or {}
        self.created = None

    def request(self, path, payload=None):
        if payload is not None:
            self.created = payload
            return {"data": {"id": "new-run"}}
        result = self.documents[path]
        if isinstance(result, Exception):
            raise result
        return result

    def items(self, path):
        return iter(self.collections[path])


def run(sha="a" * 40, status="SUCCEEDED", workflow="workflow"):
    return {"relationships": {"workflow": {"data": {"id": workflow}}},
            "attributes": {"sourceCommit": {"commitSha": sha},
                           "executionProgress": "COMPLETE", "completionStatus": status}}


class CloudTests(unittest.TestCase):
    def test_api_failure_reports_apple_diagnostics_without_credentials(self):
        credentials = {"APP_STORE_CONNECT_API_KEY": "private-key",
                       "APP_STORE_CONNECT_KEY_ID": "key-id",
                       "APP_STORE_CONNECT_ISSUER_ID": "issuer-id"}
        body = json.dumps({"errors": [{
            "code": "FORBIDDEN_ERROR", "title": "Access forbidden",
            "detail": "Agreement required.\nprivate-key key-id issuer-id signed-token",
            "meta": "do-not-log-extra-fields"}]}).encode()
        for payload in (None, {"data": {"type": "ciBuildRuns"}}):
            with self.subTest(payload=payload), patch.dict('os.environ', credentials), \
                    patch.dict('sys.modules', {'jwt': Mock(encode=Mock(return_value='signed-token'))}), \
                    patch('xcode_cloud.urllib.request.urlopen', side_effect=urllib.error.HTTPError(
                        'https://api.appstoreconnect.apple.com/v1/ciWorkflows/workflow',
                        403, 'Forbidden', {}, io.BytesIO(body))) as request:
                with self.assertRaises(AppleAPIError) as caught:
                    AppleAPI().request('ciWorkflows/workflow?secret-query', payload)
                message = str(caught.exception)
                self.assertEqual(caught.exception.status, 403)
                self.assertIn('FORBIDDEN_ERROR', message)
                self.assertIn('Access forbidden', message)
                self.assertIn('Agreement required.', message)
                for secret in (*credentials.values(), 'signed-token', 'secret-query',
                               'do-not-log-extra-fields', '\n'):
                    self.assertNotIn(secret, message)
                request.assert_called_once()

    def test_unexpected_apple_error_bodies_preserve_http_status(self):
        for body in (b'', b'<html>proxy error</html>', b'\xff', b'[]',
                     b'{"errors":null}', b'{"errors":[null,42,{"detail":{}}]}'):
            with self.subTest(body=body):
                error = AppleAPIError(403, 'ciWorkflows/workflow', body)
                self.assertEqual(str(error), 'Apple API returned HTTP 403 for ciWorkflows/workflow')

    def test_apple_error_output_is_bounded(self):
        error = AppleAPIError(403, 'ciWorkflows/workflow', json.dumps({
            'errors': [{'detail': 'x' * 10000}]}))
        self.assertLessEqual(len(str(error)), 4096)

    def test_workflow_uuid_preserves_apple_resource_casing(self):
        self.assertEqual(validated_workflow_id(' 528DCA26-4074-471F-A83B-D477B47DC2F0 '),
                         '528DCA26-4074-471F-A83B-D477B47DC2F0')

    def test_workflow_uses_exact_id_returned_by_apple(self):
        api = FakeAPI({'ciWorkflows/WEB-ID': {'data': {'id': 'api-id'}}})
        self.assertEqual(resolve_workflow(api, 'WEB-ID'), 'api-id')

    def test_404_discovers_exact_workflow_casing(self):
        api = FakeAPI({'ciWorkflows/web-id': AppleAPIError(404, 'ciWorkflows/web-id')},
                      {'ciProducts?limit=200': [{'id': 'product'}],
                       'ciProducts/product/workflows?limit=200': [{'id': 'WEB-ID'}, {'id': 'other'}]})
        self.assertEqual(resolve_workflow(api, 'web-id'), 'WEB-ID')

    def test_inaccessible_workflow_cannot_start_a_build(self):
        api = FakeAPI({'ciWorkflows/missing': AppleAPIError(404, 'ciWorkflows/missing')},
                      {'ciProducts?limit=200': []})
        with self.assertRaisesRegex(ValueError, 'Apple team'):
            resolve_workflow(api, 'missing')
        self.assertIsNone(api.created)

    def test_authentication_failure_does_not_try_discovery(self):
        api = FakeAPI({'ciWorkflows/workflow': AppleAPIError(403, 'ciWorkflows/workflow')})
        with self.assertRaises(AppleAPIError):
            resolve_workflow(api, 'workflow')

    def test_missing_or_invalid_workflow_fails_before_api_request(self):
        for value in ('', '  ', 'not-a-workflow-id'):
            with self.subTest(value=value), patch.dict('os.environ', {'XCODE_CLOUD_WORKFLOW_ID': value}), \
                    patch('sys.argv', ['xcode_cloud.py', '--ref', 'refs/tags/1.10.0-fusex-0.3', '--sha', 'a' * 40]), \
                    patch('xcode_cloud.AppleAPI') as api, contextlib.redirect_stderr(io.StringIO()) as error:
                with self.assertRaises(SystemExit) as exited:
                    main()
                self.assertEqual(exited.exception.code, 2)
                self.assertIn('XCODE_CLOUD_WORKFLOW_ID', error.getvalue())
                api.assert_not_called()

    def test_mismatched_commit_or_workflow_cannot_be_released(self):
        for build in (run(sha="b" * 40), run(workflow="other")):
            with self.assertRaises(ValueError):
                verify_run(build, "workflow", "a" * 40)

    def test_failed_build_is_not_downloaded(self):
        api = FakeAPI({"ciBuildRuns/run?include=workflow": {"data": run(status="FAILED")}})
        with self.assertRaises(RuntimeError):
            wait_for_run(api, "run", "workflow", "a" * 40, 10)

    def test_processing_build_can_be_resumed_after_timeout(self):
        build = run()
        build["attributes"].update(executionProgress="RUNNING", completionStatus=None)
        api = FakeAPI({"ciBuildRuns/run?include=workflow": {"data": build}})
        with self.assertRaisesRegex(TimeoutError, "build_run_id=run"):
            wait_for_run(api, "run", "workflow", "a" * 40, 0)

    def test_development_artifact_is_rejected(self):
        api = FakeAPI(collections={"ciBuildRuns/run/actions?limit=200": [{"id": "action"}],
                                  "ciBuildActions/action/artifacts?limit=200": [
                                      {"id": "dev", "attributes": {"fileType": "XCODEBUILD_PRODUCTS"}}]})
        with self.assertRaises(ValueError):
            notarized_artifact(api, "run")

    def test_notarized_artifact_is_selected_instead_of_archive(self):
        signed = {"id": "signed", "attributes": {"fileType": "STAPLED_NOTARIZED_ARCHIVE"}}
        api = FakeAPI(collections={"ciBuildRuns/run/actions?limit=200": [{"id": "action"}],
                                  "ciBuildActions/action/artifacts?limit=200": [
                                      {"id": "archive", "attributes": {"fileType": "ARCHIVE"}}, signed]})
        self.assertEqual(notarized_artifact(api, "run"), signed)

    def test_trigger_uses_requested_tag_not_default_branch(self):
        api = FakeAPI({"ciWorkflows/workflow/repository": {"data": {"id": "repo"}}},
                      {"scmRepositories/repo/gitReferences?limit=200": [
                          {"id": "main", "attributes": {"canonicalName": "refs/heads/main"}},
                          {"id": "tag", "attributes": {"canonicalName": "refs/tags/release"}}]})
        self.assertEqual(start_build(api, "workflow", "refs/tags/release"), "new-run")
        source = api.created["data"]["relationships"]["sourceBranchOrTag"]["data"]
        self.assertEqual(source["id"], "tag")


if __name__ == "__main__":
    unittest.main()
