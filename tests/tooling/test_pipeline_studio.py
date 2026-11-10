#!/usr/bin/env python3
"""实际的 Studio HTTP、本地文件事务和前端使用方。"""

import copy
from concurrent.futures import ThreadPoolExecutor, TimeoutError as FutureTimeout
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

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="studio-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.configs = self.root / "configs"
        self.configs.mkdir()
        shutil.copy(ROOT / "configs/pipeline_keyword_match_rules.json", self.configs)
        shutil.copy(ROOT / "configs/pipeline_keyword_match_rules.conf", self.configs)
        self.root_patch = mock.patch.object(studio, "PROJECT_ROOT", self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)
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
        self.assertNotIn("profiles", catalog)
        status, initial = self.request("GET", "/api/initial")
        self.assertEqual((status, initial["document"]), (200, None))
        with urllib.request.urlopen(self.url + "/index.html") as response:
            self.assertIn('id="graph"', response.read().decode())
        status, missing = self.request("GET", "/api/not-an-endpoint")
        self.assertEqual(status, 404)
        self.assertEqual(missing["error"]["code"], "NOT_FOUND")

    def test_new_draft_selects_io_without_materializing_defaults(self):
        inputs = [{"type": "keyword_in", "name": "keyword_match"}]
        outputs = [{"type": "json_out", "name": "keyword_match"}]
        status, result = self.request("POST", "/api/init", {"input": inputs, "output": outputs})
        self.assertEqual(status, 200, result)
        self.assertTrue(result["ok"])
        self.assertEqual(result["pipeline"], {"io": {"input": inputs, "output": outputs},
                                            "models": [], "pipeline": []})
        self.assertEqual(len(list(self.configs.glob("*.json"))), 1)

    def test_validation_and_authoring_preserve_native_json_status_without_saving(self):
        bad = copy.deepcopy(self.base)
        bad["pipeline"][0]["type"] = "absent_node"
        status, report = self.request("POST", "/api/validate", {"pipeline": bad})
        self.assertEqual(status, 200)
        self.assertFalse(report["ok"])
        self.assertIn(("UNKNOWN_NODE_TYPE", "/pipeline/0/type"),
                      {(item["code"], item["path"]) for item in report["diagnostics"]})
        status, valid = self.request("POST", "/api/validate", {"pipeline": self.base})
        self.assertEqual(status, 200)
        self.assertTrue(valid["ok"])
        status, edited = self.request("POST", "/api/authoring/preview", {
            "pipeline": self.base, "require_valid": True,
            "operation": {"kind": "rename_node", "node": "match_keywords", "new_name": "renamed"}})
        self.assertEqual(status, 200, edited)
        self.assertTrue(edited["ok"])
        self.assertTrue(edited["validation"]["ok"])
        self.assertEqual(edited["pipeline"]["io"]["output"][0]["inputs"]["matches"], "renamed.matches")
        self.assertFalse((self.configs / "pipeline_preview.json").exists())
        status, rejected = self.request("POST", "/api/authoring/preview", {
            "pipeline": self.base, "operation": {}, "operations": []})
        self.assertEqual(status, 200, rejected)
        self.assertFalse(rejected["ok"])
        self.assertIn("互斥", " ".join(item["message"] for item in rejected["diagnostics"]))

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
        self.assertNotIn("deployment", opened)
        status, conflict = self.request("POST", "/api/pipelines", {"filename": path.name, "pipeline": document})
        self.assertEqual(status, 409)
        self.assertEqual(conflict["error"]["code"], "FILE_EXISTS")
        self.assertEqual(path.read_bytes(), raw)

    def test_incomplete_document_saves_without_tool_and_retains_native_errors(self):
        incomplete = copy.deepcopy(self.base)
        incomplete["pipeline"][0]["inputs"] = {}
        incomplete["unknown"] = {"preserve": ["中文", False]}
        with mock.patch.object(studio, "PIPELINE_TOOL", self.root / "missing_tool"):
            saved = self.save("pipeline_incomplete.json", incomplete)
            status, opened = self.request("GET", "/api/pipeline?filename=" + saved["filename"])
            self.assertEqual(status, 200, opened)
            self.assertEqual(opened["pipeline"], incomplete)
            status, unavailable = self.request("POST", "/api/validate", {"pipeline": incomplete})
            self.assertEqual(status, 503, unavailable)
            self.assertEqual(unavailable["error"]["code"], "TOOL_NOT_BUILT")
        status, invalid = self.request("POST", "/api/validate", {"pipeline": incomplete})
        self.assertEqual(status, 200, invalid)
        self.assertFalse(invalid["ok"])
        self.assertTrue(invalid["diagnostics"])
        for invalid in (None, [], {}, {"pipeline": [None]}, {"pipeline": [], "models": {}},
                        {"pipeline": [], "io": {"input": [None]}}, {"pipeline": [{"depends_on": "bad"}]}):
            status, rejected = self.request("POST", "/api/pipelines", {
                "filename": "pipeline_invalid.json", "pipeline": invalid})
            self.assertEqual(status, 400, rejected)
            self.assertEqual(rejected["error"]["code"], "INVALID_PIPELINE_DOCUMENT")
        self.assertFalse((self.configs / "pipeline_invalid.json").exists())

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
            "input": "invalid", "output": []})
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

    def test_change_during_staging_is_detected_before_atomic_replace(self):
        saved = self.save()
        path = self.configs / saved["filename"]
        external_bytes = json.dumps({**self.base, "comment": "external edit"}).encode()
        fsync = studio.os.fsync

        def concurrent_edit(descriptor):
            fsync(descriptor)
            path.write_bytes(external_bytes)

        with mock.patch.object(studio.os, "fsync", side_effect=concurrent_edit):
            status, result = self.request("PUT", "/api/pipeline", {
                "filename": path.name, "pipeline": self.base, "revision": saved["revision"]})
        self.assertEqual(status, 409)
        self.assertEqual(result["error"]["code"], "REVISION_CONFLICT")
        self.assertEqual(path.read_bytes(), external_bytes)
        self.assertFalse(list(self.configs.glob(".pipeline_edit.*")))

    def test_concurrent_saves_have_one_winner_and_one_conflict(self):
        for creating in (False, True):
            with self.subTest(creating=creating):
                name = "pipeline_create_race.json" if creating else "pipeline_update_race.json"
                saved = None if creating else self.save(name)
                path = self.configs / name
                entered, release = threading.Event(), threading.Event()
                publish_name = "link" if creating else "replace"
                publish = getattr(studio.os, publish_name)

                def paused_publish(source, target):
                    if Path(target) == path and not entered.is_set():
                        entered.set()
                        if not release.wait(5):
                            raise RuntimeError("save test did not release the first writer")
                    return publish(source, target)

                def submit(comment):
                    return self.request("POST" if creating else "PUT",
                                        "/api/pipelines" if creating else "/api/pipeline", {
                                            "filename": name, "pipeline": {**self.base, "comment": comment},
                                            "revision": saved["revision"] if saved else None})

                with mock.patch.object(studio.os, publish_name, side_effect=paused_publish), ThreadPoolExecutor(2) as pool:
                    first = pool.submit(submit, "first")
                    try:
                        self.assertTrue(entered.wait(5))
                        second = pool.submit(submit, "second")
                        # 给竞争的 HTTP 请求留出时间，使其到达被挂起的提交。
                        try:
                            second.result(timeout=1)
                        except FutureTimeout:
                            pass
                    finally:
                        release.set()
                    results = [first.result(timeout=5), second.result(timeout=5)]
                self.assertEqual(sorted(status for status, _ in results), [200, 409], results)
                winner = next(result for status, result in results if status == 200)
                loser = next(result for status, result in results if status == 409)
                self.assertEqual(loser["error"]["code"], "FILE_EXISTS" if creating else "REVISION_CONFLICT")
                self.assertEqual(json.loads(path.read_bytes()), winner["pipeline"])
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), winner["revision"])

    def test_save_as_never_overwrites_a_concurrently_created_file(self):
        path = self.configs / "pipeline_concurrent.json"
        external_bytes = b"created by another editor"
        link = studio.os.link

        def external_create(source, target):
            Path(target).write_bytes(external_bytes)
            return link(source, target)

        with mock.patch.object(studio.os, "link", side_effect=external_create):
            status, result = self.request("POST", "/api/pipelines", {
                "filename": path.name, "pipeline": self.base})
        self.assertEqual(status, 409)
        self.assertEqual(result["error"]["code"], "FILE_EXISTS")
        self.assertEqual(path.read_bytes(), external_bytes)
        self.assertFalse(list(self.configs.glob(".pipeline_concurrent.*")))

    def test_save_only_writes_json_and_ignores_conf_changes(self):
        name = "pipeline_keyword_match_rules.json"
        conf = self.configs / "pipeline_keyword_match_rules.conf"
        conf.write_text("externally edited, irrelevant to document saving")
        raw_conf = conf.read_bytes()
        opened = self.service.open_pipeline(name)
        updated = {**self.base, "comment": "JSON only"}
        with mock.patch.object(self.service, "invoke_tool", side_effect=AssertionError("save must not invoke the tool")):
            status, saved = self.request("PUT", "/api/pipeline", {
                "filename": name, "pipeline": updated, "revision": opened["revision"]})
        self.assertEqual(status, 200, saved)
        self.assertEqual(saved["pipeline"], updated)
        self.assertEqual(conf.read_bytes(), raw_conf)
        self.assertNotIn("deployment", saved)
        self.assertFalse((self.root / "output").exists())

    def test_managed_paths_reject_escape_and_symlinks_while_import_is_read_only(self):
        outside = self.root / "pipeline_import.json"
        outside.write_text('{"pipeline":[],"comment":"browsable draft"}')
        imported = studio.WorkbenchService(self.configs, initial=outside).initial_pipeline()["document"]
        self.assertTrue(imported["imported"])
        self.assertEqual(imported["revision"], "")
        self.assertEqual(imported["pipeline"]["comment"], "browsable draft")
        (self.configs / "pipeline_link.json").symlink_to(outside)
        for requested, code in (("../pipeline_import.json", "INVALID_PIPELINE_PATH"),
                                ("configs/pipeline_keyword_match_rules.json", "INVALID_PIPELINE_PATH"),
                                ("我的方案.json", "INVALID_PIPELINE_NAME"),
                                ("asset_manifest.json", "INVALID_PIPELINE_NAME"),
                                ("pipeline_link.json", "SYMLINK_REJECTED")):
            with self.subTest(requested=requested):
                with self.assertRaises(studio.StudioError) as caught:
                    self.service.open_pipeline(requested)
                self.assertEqual(caught.exception.code, code)
        self.assertEqual(outside.read_text(), '{"pipeline":[],"comment":"browsable draft"}')

    def test_file_read_boundaries_are_shared_and_bad_files_do_not_hide_valid_ones(self):
        cases = (("pipeline_bad_utf8.json", b"\xff", "JSON_PARSE"),
                 ("pipeline_bad_json.json", b"{", "JSON_PARSE"),
                 ("pipeline_wrong_shape.json", b"{}", "INVALID_PIPELINE_DOCUMENT"),
                 ("pipeline_large.json", b" " * (studio.MAX_DOCUMENT_BYTES + 1), "DOCUMENT_TOO_LARGE"))
        for name, raw, code in cases:
            path = self.configs / name
            path.write_bytes(raw)
            with self.subTest(filename=name):
                for read in (lambda: self.service.open_pipeline(name),
                             lambda: studio.read_pipeline_file(path)):
                    with self.assertRaises(studio.StudioError) as caught:
                        read()
                    self.assertEqual(caught.exception.code, code)
        listed = self.service.pipelines()["pipelines"]
        self.assertEqual([item["filename"] for item in listed], ["pipeline_keyword_match_rules.json"])
        raw = (self.configs / listed[0]["filename"]).read_bytes()
        self.assertEqual(listed[0]["revision"], hashlib.sha256(raw).hexdigest())

    def test_terminal_displays_saved_null_and_incomplete_io_drafts(self):
        for io in (None, {"input": None, "output": [{}]}):
            with self.subTest(io=io):
                document = {"pipeline": [{}], "models": None, "io": io}
                saved = self.service.save_pipeline("pipeline_draft.json", document, None, save_as=True)
                path = self.configs / saved["filename"]
                with mock.patch("builtins.print") as output:
                    studio.main([str(path)])
                self.assertTrue(any("io:" in str(call) for call in output.call_args_list))
                path.unlink()

    def test_oversized_http_body_is_rejected_before_reading_the_document(self):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=5)
        self.addCleanup(connection.close)
        connection.putrequest("POST", "/api/validate")
        connection.putheader("Content-Length", str(studio.MAX_DOCUMENT_BYTES + 1))
        connection.endheaders()
        response = connection.getresponse()
        self.assertEqual(response.status, 413)
        self.assertEqual(json.loads(response.read())["error"]["code"], "BODY_TOO_LARGE")

    def test_javascript_library_consumers(self):
        scripts = sorted(path for path in (ROOT / "tests/tooling").rglob("studio_*_test.mjs")
                         if path != ROOT / "tests/tooling/studio_browser_test.mjs")
        self.assertTrue(scripts, "Studio JavaScript test discovery must not be empty")
        for script in scripts:
            with self.subTest(script=script):
                result = subprocess.run(["node", str(script), self.url],
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
