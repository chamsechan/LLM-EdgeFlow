#!/usr/bin/env python3
"""Actual Studio HTTP, local-file transactions and frontend consumers."""

import copy
import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest
from unittest import mock
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("studio_under_test", ROOT / "tools/pipeline_studio/server.py")
studio = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(studio)


class StudioHttpContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not studio.PIPELINE_TOOL.is_file():
            raise RuntimeError(f"Set LLM_EDGEFLOW_PIPELINE_TOOL to a built native tool: {studio.PIPELINE_TOOL}")
        cls.base = json.loads((ROOT / "configs/pipeline_keyword_match_rules.json").read_text())
        cls.profile = json.loads((ROOT / "demo/profiles.json").read_text())["profiles"]["keyword_match_rules"]

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="studio-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.configs = self.root / "configs"
        self.configs.mkdir()
        (self.root / "demo").mkdir()
        (self.root / "data").mkdir()
        (self.root / "demo/profiles.json").write_text(json.dumps({"profiles": {"keyword_match_rules": self.profile}}))
        shutil.copy(ROOT / "configs/pipeline_keyword_match_rules.json", self.configs)
        shutil.copy(ROOT / "configs/pipeline_keyword_match_rules.conf", self.configs)
        shutil.copy(ROOT / "data/corpus_keyword_match.txt", self.root / "data")
        self.root_patch = mock.patch.object(studio, "PROJECT_ROOT", self.root)
        self.profile_patch = mock.patch.object(studio, "PROFILE_FILE", self.root / "demo/profiles.json")
        self.root_patch.start()
        self.profile_patch.start()
        self.addCleanup(self.root_patch.stop)
        self.addCleanup(self.profile_patch.stop)
        self.service = studio.WorkbenchService(self.configs)
        self.server = studio.StudioHttpServer(("127.0.0.1", 0), studio.make_handler(self.service))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.close_server)
        self.url = f"http://127.0.0.1:{self.server.server_port}"

    def close_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def request(self, method, path, body=None):
        encoded = None if body is None else json.dumps(body, ensure_ascii=False).encode()
        request = urllib.request.Request(self.url + path, data=encoded, method=method,
                                         headers={"Content-Type": "application/json"})
        try:
            response = urllib.request.urlopen(request, timeout=15)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            payload = json.loads(response.read())
            self.assertIn("application/json", response.headers["Content-Type"])
            self.assertEqual(response.headers["Cache-Control"], "no-store")
            return response.status, payload

    def save(self, name="pipeline_edit.json", pipeline=None):
        status, result = self.request("POST", "/api/pipelines", {
            "filename": name, "pipeline": self.base if pipeline is None else pipeline})
        self.assertEqual(status, 200, result)
        self.assertTrue(result["ok"])
        return result

    def test_http_catalog_initial_and_static_page_are_real_consumable_responses(self):
        status, catalog = self.request("GET", "/api/catalog")
        self.assertEqual(status, 200)
        self.assertTrue(any(node["node_type"] == "text_rule_match" for node in catalog["nodes"]))
        status, initial = self.request("GET", "/api/initial")
        self.assertEqual((status, initial["document"]), (200, None))
        with urllib.request.urlopen(self.url + "/index.html") as response:
            self.assertIn('id="graph"', response.read().decode())
        status, missing = self.request("GET", "/api/not-an-endpoint")
        self.assertEqual(status, 404)
        self.assertEqual(missing["error"]["code"], "NOT_FOUND")

    def test_validation_and_authoring_preserve_native_json_status_without_saving(self):
        bad = copy.deepcopy(self.base)
        bad["pipeline"][0]["type"] = "absent_node"
        status, report = self.request("POST", "/api/validate", {"pipeline": bad, "explain": True})
        self.assertEqual(status, 200)
        self.assertFalse(report["ok"])
        self.assertIn(("UNKNOWN_NODE_TYPE", "/pipeline/0/type"),
                      {(item["code"], item["path"]) for item in report["diagnostics"]})
        status, valid = self.request("POST", "/api/validate", {"pipeline": self.base, "explain": True})
        self.assertEqual(status, 200)
        status, edited = self.request("POST", "/api/authoring/preview", {
            "pipeline": self.base, "revision": valid["revision"],
            "tool_fingerprint": valid["tool_fingerprint"], "require_valid": True,
            "operation": {"kind": "rename_node", "node": "match_keywords", "new_name": "renamed"}})
        self.assertEqual(status, 200, edited)
        self.assertTrue(edited["ok"])
        self.assertEqual(edited["pipeline"]["io"]["output"][0]["inputs"]["matches"], "renamed.matches")
        self.assertFalse((self.configs / "pipeline_preview.json").exists())

    def test_save_and_open_roundtrip_complete_unicode_document_and_revision(self):
        document = copy.deepcopy(self.base)
        document["comment"] = "保持 Unicode、空白和完整文档\n第二行"
        saved = self.save(pipeline=document)
        path = self.configs / saved["filename"]
        raw = path.read_bytes()
        self.assertEqual(json.loads(raw), document)
        self.assertIn("Unicode、空白".encode(), raw)
        self.assertTrue(raw.endswith(b"\n"))
        self.assertEqual(saved["revision"], hashlib.sha256(raw).hexdigest())
        status, opened = self.request("GET", "/api/pipeline?filename=pipeline_edit.json")
        self.assertEqual(status, 200)
        self.assertEqual(opened["pipeline"], document)
        self.assertEqual(opened["save_targets"], ["pipeline_edit.json"])
        status, conflict = self.request("POST", "/api/pipelines", {"filename": path.name, "pipeline": document})
        self.assertEqual(status, 409)
        self.assertEqual(conflict["error"]["code"], "FILE_EXISTS")
        self.assertEqual(path.read_bytes(), raw)

    def test_preview_checks_revision_and_tool_fingerprint(self):
        status, validated = self.request("POST", "/api/validate", {
            "pipeline": self.base, "explain": True})
        self.assertEqual(status, 200, validated)
        for endpoint, operation in (
            ("/api/fixes/preview", {"patch": [{"op": "add", "path": "/comment", "value": "preview"}]}),
            ("/api/authoring/preview", {"operation": {
                "kind": "rename_node", "node": "match_keywords", "new_name": "renamed"}}),
        ):
            with self.subTest(endpoint=endpoint):
                body = {"pipeline": self.base, "revision": validated["revision"],
                        "tool_fingerprint": validated["tool_fingerprint"], **operation}
                for field, code in (("revision", "REVISION_CONFLICT"),
                                    ("tool_fingerprint", "TOOL_OUTDATED")):
                    with self.subTest(field=field):
                        status, rejected = self.request("POST", endpoint, {**body, field: "outdated"})
                        self.assertEqual(status, 409, rejected)
                        self.assertEqual(rejected["error"]["code"], code)
                status, previewed = self.request("POST", endpoint, body)
                self.assertEqual(status, 200, previewed)
                self.assertTrue(previewed["ok"])
                if endpoint == "/api/fixes/preview":
                    self.assertEqual(previewed["patched"]["comment"], "preview")
                else:
                    self.assertEqual(previewed["pipeline"]["pipeline"][0]["name"], "renamed")

    def test_deployment_association_preflight_and_save_use_canonical_fields(self):
        filename = "pipeline_keyword_match_rules.json"
        conf_name = "pipeline_keyword_match_rules.conf"
        status, associated = self.request("POST", "/api/deployment/associate", {
            "filename": filename, "conf_name": conf_name})
        self.assertEqual(status, 200, associated)
        self.assertTrue(associated["ok"])
        self.assertEqual(associated["save_targets"], [filename, conf_name])

        status, ready = self.request("POST", "/api/preflight", {
            "pipeline": self.base, "conf_name": conf_name})
        self.assertEqual(status, 200, ready)
        self.assertEqual(ready["summary"]["status"], "ready")
        self.assertEqual(ready["summary"]["conf_path"], conf_name)

        status, opened = self.request("GET", "/api/pipeline?filename=" + filename)
        self.assertEqual(status, 200, opened)
        updated = {**self.base, "comment": "saved after canonical association"}
        status, saved = self.request("PUT", "/api/pipeline", {
            "filename": filename, "pipeline": updated, "revision": opened["revision"]})
        self.assertEqual(status, 200, saved)
        self.assertEqual(json.loads((self.configs / filename).read_text()), updated)

    def test_native_validation_failure_rejects_save_before_any_file_publication(self):
        invalid = copy.deepcopy(self.base)
        invalid["pipeline"][0]["type"] = "unregistered"
        status, rejected = self.request("POST", "/api/pipelines", {
            "filename": "pipeline_invalid.json", "pipeline": invalid})
        self.assertEqual(status, 400)
        self.assertEqual(rejected["error"]["code"], "VALIDATION_FAILED")
        self.assertFalse((self.configs / "pipeline_invalid.json").exists())
        saved = self.save()
        path = self.configs / saved["filename"]
        original = path.read_bytes()
        status, rejected = self.request("PUT", "/api/pipeline", {
            "filename": path.name, "pipeline": invalid, "revision": saved["revision"]})
        self.assertEqual(status, 400)
        self.assertEqual(rejected["error"]["code"], "VALIDATION_FAILED")
        self.assertEqual(path.read_bytes(), original)
        self.assertFalse(list(self.configs.glob(".pipeline_edit.*")))

    def test_http_body_parse_errors_have_their_own_status_and_json_envelope(self):
        request = urllib.request.Request(self.url + "/api/validate", data=b"{broken",
                                         headers={"Content-Type": "application/json"})
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(request, timeout=5)
        with caught.exception as response:
            self.assertEqual(response.status, 400)
            payload = json.loads(response.read())
        self.assertFalse(payload["ok"])
        self.assertEqual(studio.ErrorCode.JSON_PARSE.value, -2002)
        self.assertEqual(payload["error"]["code"], studio.ErrorCode.JSON_PARSE.name)
        status, rejected = self.request("POST", "/api/init", {
            "profile": "keyword_match_rules", "input": []})
        self.assertEqual(status, 400)
        self.assertEqual(rejected["error"]["code"], "INVALID_INIT_REQUEST")
        with mock.patch.object(self.service, "initial_pipeline", side_effect=RuntimeError("injected failure")):
            status, failed = self.request("GET", "/api/initial")
        self.assertEqual(status, 500)
        self.assertEqual(failed["error"], {
            "code": studio.ErrorCode.INTERNAL_EXCEPTION.name,
            "message": "injected failure",
        })

    def test_external_revision_conflict_and_replace_failure_preserve_original_file(self):
        saved = self.save()
        path = self.configs / saved["filename"]
        original = path.read_bytes()
        updated = copy.deepcopy(self.base)
        updated["comment"] = "updated"
        status, conflict = self.request("PUT", "/api/pipeline", {
            "filename": path.name, "pipeline": updated, "revision": "outdated"})
        self.assertEqual(status, 409)
        self.assertEqual(conflict["error"]["code"], "REVISION_CONFLICT")
        self.assertEqual(path.read_bytes(), original)
        with mock.patch.object(studio.os, "replace", side_effect=OSError("disk failure")):
            status, failed = self.request("PUT", "/api/pipeline", {
                "filename": path.name, "pipeline": updated, "revision": saved["revision"]})
        self.assertEqual(status, 500)
        self.assertFalse(failed["ok"])
        self.assertEqual(path.read_bytes(), original)
        self.assertFalse(list(self.configs.glob(".pipeline_edit.*")))

    def test_change_during_native_validation_is_detected_before_atomic_replace(self):
        saved = self.save()
        path = self.configs / saved["filename"]
        external = copy.deepcopy(self.base)
        external["comment"] = "external edit"
        external_bytes = json.dumps(external).encode()
        invoke = self.service.invoke_tool

        def concurrent_edit(command, pipeline=None):
            result = invoke(command, pipeline)
            if command[0] == "validate" and command[1] != "--stdin":
                path.write_bytes(external_bytes)
            return result

        with mock.patch.object(self.service, "invoke_tool", side_effect=concurrent_edit):
            status, result = self.request("PUT", "/api/pipeline", {
                "filename": path.name, "pipeline": self.base, "revision": saved["revision"]})
        self.assertEqual(status, 409)
        self.assertEqual(result["error"]["code"], "REVISION_CONFLICT")
        self.assertEqual(path.read_bytes(), external_bytes)
        self.assertFalse(list(self.configs.glob(".pipeline_edit.*")))

    def test_managed_paths_reject_escape_and_symlinks_while_import_is_read_only(self):
        outside = self.root / "pipeline_import.json"
        outside.write_text('{"pipeline":[],"comment":"browsable draft"}')
        imported = studio.WorkbenchService(self.configs, initial=outside).initial_pipeline()["document"]
        self.assertTrue(imported["imported"])
        self.assertEqual(imported["revision"], "")
        self.assertEqual(imported["pipeline"]["comment"], "browsable draft")
        (self.configs / "pipeline_link.json").symlink_to(outside)
        for requested, code in (("../pipeline_import.json", "INVALID_PIPELINE_PATH"),
                                ("pipeline_link.json", "SYMLINK_REJECTED")):
            with self.subTest(requested=requested):
                with self.assertRaises(studio.StudioError) as caught:
                    self.service.open_pipeline(requested)
                self.assertEqual(caught.exception.code, code)
        self.assertEqual(outside.read_text(), '{"pipeline":[],"comment":"browsable draft"}')

    def test_oversized_http_body_is_rejected_before_reading_the_document(self):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=5)
        self.addCleanup(connection.close)
        connection.putrequest("POST", "/api/validate")
        connection.putheader("Content-Length", str(studio.MAX_DOCUMENT_BYTES + 1))
        connection.endheaders()
        response = connection.getresponse()
        self.assertEqual(response.status, 413)
        self.assertEqual(json.loads(response.read())["error"]["code"], "BODY_TOO_LARGE")

    def test_paired_solution_rollback_preserves_both_artifacts_and_session_ownership(self):
        status, saved = self.request("POST", "/api/solutions", {
            "filename": "pipeline_pair.json", "pipeline": self.base, "profile": "keyword_match_rules"})
        self.assertEqual(status, 200, saved)
        path, conf = self.configs / saved["filename"], self.configs / saved["conf_filename"]
        previous = (path.read_bytes(), conf.read_bytes())
        self.assertEqual(json.loads(previous[1]), {"pipe_path": path.name})
        self.assertEqual(saved["save_targets"], [path.name, conf.name])
        self.assertIn("demo-profile.json", saved["command"])
        replace = studio.os.replace

        def fail_conf_install(source, target):
            if Path(target) == conf:
                raise OSError("conf installation failed")
            return replace(source, target)

        updated = copy.deepcopy(self.base)
        updated["comment"] = "new version"
        with mock.patch.object(studio.os, "replace", side_effect=fail_conf_install):
            status, failed = self.request("PUT", "/api/pipeline", {
                "filename": path.name, "pipeline": updated, "revision": saved["revision"],
                "profile": "keyword_match_rules"})
        self.assertEqual(status, 500)
        self.assertEqual(failed["error"]["code"], "SAVE_FAILED")
        self.assertEqual((path.read_bytes(), conf.read_bytes()), previous)
        self.assertFalse(list(self.configs.glob(".studio-save-*")))
        restarted = studio.WorkbenchService(self.configs)
        self.assertEqual(restarted.open_pipeline(path.name)["save_targets"], [path.name])

    def test_pair_creation_preflight_failure_removes_only_its_own_new_files(self):
        resolve = self.service.resolve_run_conf

        def fail_final_preflight(conf_path, profile):
            if conf_path.name == "pipeline_failed.conf":
                self.assertTrue((self.configs / "pipeline_failed.json").exists())
                self.assertTrue(conf_path.exists())
                raise studio.StudioError(studio.ErrorCode.DEPLOYMENT_VALIDATION_FAILED, "preflight failed")
            return resolve(conf_path, profile)

        with mock.patch.object(self.service, "resolve_run_conf", side_effect=fail_final_preflight):
            status, failed = self.request("POST", "/api/solutions", {
                "filename": "pipeline_failed.json", "pipeline": self.base, "profile": "keyword_match_rules"})
        self.assertEqual(status, 400)
        self.assertEqual(failed["error"]["code"], "DEPLOYMENT_VALIDATION_FAILED")
        self.assertFalse((self.configs / "pipeline_failed.json").exists())
        self.assertFalse((self.configs / "pipeline_failed.conf").exists())
        self.assertTrue((self.configs / "pipeline_keyword_match_rules.json").exists())

    def test_background_failure_uses_shared_codes_and_keeps_reason(self):
        for failure, expected in (
            (RuntimeError("worker failure"), "INTERNAL_EXCEPTION"),
            (studio.StudioError(studio.ErrorCode.RUN_TIMEOUT, "worker timeout"), "RUN_TIMEOUT"),
        ):
            with self.subTest(code=expected):
                self.service.jobs["failed-job"] = {"cancel_requested": False}
                with mock.patch.object(self.service, "resolve_run_conf", side_effect=failure):
                    self.service._run_job("failed-job", self.base, self.profile, {})
                job = self.service.run_status("failed-job")["job"]
                self.assertEqual(job["status"], "failed")
                self.assertEqual(job["error"], {"code": expected, "message": str(failure)})

    def test_javascript_library_and_fix_consumers(self):
        for script in ("studio_editor_test.mjs", "studio_graph_test.mjs",
                       "studio_config_roundtrip_test.mjs", "studio_fix_workflow_test.mjs"):
            with self.subTest(script=script):
                result = subprocess.run(["node", str(ROOT / "tests/tooling" / script), self.url],
                                        cwd=ROOT, capture_output=True, text=True, timeout=45)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("PASS", result.stdout)

    @unittest.skipUnless(os.environ.get("STUDIO_PLAYWRIGHT_MODULE"), "Optional real browser requires STUDIO_PLAYWRIGHT_MODULE")
    def test_real_browser_consumer(self):
        result = subprocess.run(["node", str(ROOT / "tests/tooling/studio_browser_test.mjs"), self.url],
                                cwd=ROOT, capture_output=True, text=True, timeout=180)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
