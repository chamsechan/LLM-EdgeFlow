#!/usr/bin/env python3
"""API and filesystem boundary tests for the local Pipeline Studio."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
PIPELINE_TOOL = Path(
    os.environ.get(
        "LLM_EDGEFLOW_PIPELINE_TOOL", ROOT / "build" / "alg_pipeline_tool"
    )
)
SPEC = importlib.util.spec_from_file_location("edgeflow_show", ROOT / "scripts" / "show.py")
SHOW = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SHOW)


class WorkbenchServiceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.configs = Path(self.temporary.name)
        self.service = SHOW.WorkbenchService(self.configs)
        self.keyword = json.loads((ROOT / "configs" / "pipeline_keyword_match.json").read_text())

    def tearDown(self):
        self.temporary.cleanup()

    def test_create_open_save_as_and_revision_conflict(self):
        created = self.service.save_pipeline(
            "pipeline_api_test.json", self.keyword, None, save_as=True
        )
        opened = self.service.open_pipeline("configs/pipeline_api_test.json")
        self.assertEqual(opened["pipeline"], self.keyword)
        self.assertEqual(opened["revision"], created["revision"])
        changed = dict(self.keyword)
        changed["comment"] = "saved"
        saved = self.service.save_pipeline(
            "pipeline_api_test.json", changed, created["revision"]
        )
        self.assertNotEqual(saved["revision"], created["revision"])
        (self.configs / "pipeline_api_test.json").write_text("{}")
        with self.assertRaisesRegex(SHOW.StudioError, "重新加载或另存") as conflict:
            self.service.save_pipeline(
                "pipeline_api_test.json", changed, saved["revision"]
            )
        self.assertEqual(conflict.exception.code, "REVISION_CONFLICT")

    def test_rejects_paths_symlinks_and_invalid_pipeline_without_writing(self):
        for invalid in (
            "../pipeline_escape.json",
            "other/pipeline_escape.json",
            "pipeline-UPPER.json",
            "pipeline_ok.conf",
        ):
            with self.assertRaises(SHOW.StudioError):
                self.service.managed_path(invalid)
        target = self.configs / "outside.json"
        target.write_text("{}")
        (self.configs / "pipeline_link.json").symlink_to(target)
        with self.assertRaises(SHOW.StudioError) as link:
            self.service.open_pipeline("pipeline_link.json")
        self.assertEqual(link.exception.code, "SYMLINK_REJECTED")
        invalid = {"business_name": "keyword_match_v1", "pipeline": []}
        with self.assertRaises(SHOW.StudioError) as validation:
            self.service.save_pipeline("pipeline_invalid.json", invalid, None, True)
        self.assertEqual(validation.exception.code, "VALIDATION_FAILED")
        self.assertFalse((self.configs / "pipeline_invalid.json").exists())

    def test_profile_mismatch_and_real_demo_roundtrip(self):
        with self.assertRaises(SHOW.StudioError) as mismatch:
            self.service.start_run(self.keyword, "entity_extract_mock")
        self.assertEqual(mismatch.exception.code, "PROFILE_MISMATCH")
        started = self.service.start_run(self.keyword, "keyword_match_mock")
        for _ in range(200):
            job = self.service.run_status(started["job_id"])["job"]
            if job["status"] in ("completed", "failed", "cancelled"):
                break
            time.sleep(0.05)
        self.assertEqual(job["status"], "completed", job)
        self.assertIn("summary.json", job["result"])
        self.assertIn("results.jsonl", job["result"])


class PipelineCliTest(unittest.TestCase):
    def command(self, *args, input_pipeline=None):
        process = subprocess.run(
            [str(PIPELINE_TOOL), *args],
            input=None if input_pipeline is None else json.dumps(input_pipeline),
            text=True,
            capture_output=True,
            cwd=ROOT,
            check=False,
        )
        payload = json.loads(process.stdout)
        self.assertEqual(payload["schema_version"], 1)
        return process.returncode, payload

    def test_all_commands_return_versioned_json(self):
        first_code, first = self.command("catalog", "--business", "keyword_match_v1")
        second_code, second = self.command("catalog", "--business", "keyword_match_v1")
        self.assertEqual((first_code, first), (second_code, second))
        self.assertTrue(first["nodes"])
        code, described = self.command("describe-node", "TextRuleMatchNode")
        self.assertEqual(code, 0)
        self.assertEqual(described["node_type"], "TextRuleMatchNode")
        code, initialized = self.command(
            "init", "--business", "keyword_match_v1", "--empty"
        )
        self.assertEqual(code, 0)
        self.assertEqual(initialized["pipeline"]["pipeline"], [])
        legacy = json.loads(
            (ROOT / "configs" / "pipeline_keyword_match.json").read_text()
        )
        for node in legacy["pipeline"]:
            node.pop("id", None)
            node.pop("depends_on", None)
        code, normalized = self.command(
            "normalize", "--explicit-dag", "--stdin", input_pipeline=legacy
        )
        self.assertEqual(code, 0)
        self.assertEqual(normalized["pipeline"]["pipeline"][0]["depends_on"], [])
        code, validated = self.command(
            "validate", "--stdin", input_pipeline=normalized["pipeline"]
        )
        self.assertEqual(code, 0)
        self.assertTrue(validated["ok"])
        code, plan = self.command(
            "plan", "--stdin", input_pipeline=normalized["pipeline"]
        )
        self.assertEqual(code, 0)
        self.assertNotIn("diagnostics", plan)
        self.assertTrue(plan["plan"]["topological_order"])


class HttpApiTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.service = SHOW.WorkbenchService(Path(self.temporary.name))
        try:
            self.server = SHOW.StudioHttpServer(
                ("127.0.0.1", 0), SHOW.make_handler(self.service)
            )
        except PermissionError:
            self.temporary.cleanup()
            self.skipTest("sandbox forbids binding a loopback test socket")
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}/api/v1"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.temporary.cleanup()

    def post(self):
        pipeline = json.loads((ROOT / "configs" / "pipeline_keyword_match.json").read_text())
        request = urllib.request.Request(
            self.base + "/validate",
            data=json.dumps({"pipeline": pipeline}).encode(),
            method="POST",
            headers={"Content-Type": "application/json"},
        )
        return urllib.request.urlopen(request, timeout=5)

    def test_local_development_api_and_static_modules(self):
        origin = self.base.removesuffix("/api/v1")
        with urllib.request.urlopen(origin + "/index.html", timeout=5) as response:
            index = response.read().decode()
        self.assertIn('type="module" src="app.js"', index)
        with urllib.request.urlopen(origin + "/app.js", timeout=5) as response:
            self.assertIn("new GraphView", response.read().decode())
        with self.post() as response:
            payload = json.load(response)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["schema_version"], 1)

    def test_invalid_fixtures_table_driven_parity_matrix(self):
        fixture_path = (
            ROOT
            / "tests"
            / "fixtures"
            / "pipeline_validation"
            / "invalid_pipeline_cases.json"
        )
        fixtures = json.loads(fixture_path.read_text(encoding="utf-8"))
        self.assertEqual(fixtures["schema_version"], 1)

        for case in fixtures["cases"]:
            with self.subTest(case=case["name"]):
                pipeline = case["pipeline"]
                proc_val = subprocess.run(
                    [str(PIPELINE_TOOL), "validate", "--stdin"],
                    input=json.dumps(pipeline),
                    text=True,
                    capture_output=True,
                    cwd=ROOT,
                    check=False,
                )
                self.assertEqual(proc_val.returncode, 1)
                cli_val = json.loads(proc_val.stdout)
                self.assertFalse(cli_val["ok"])
                self.assertEqual(
                    cli_val["diagnostics"][0]["code"], case["primary_code"]
                )
                self.assertEqual(
                    cli_val["diagnostics"][0]["path"], case["primary_path"]
                )
                actual_codes = {item["code"] for item in cli_val["diagnostics"]}
                self.assertTrue(set(case["required_codes"]).issubset(actual_codes))

                req = urllib.request.Request(
                    self.base + "/validate",
                    data=json.dumps({"pipeline": pipeline}).encode(),
                    method="POST",
                    headers={"Content-Type": "application/json"},
                )
                with urllib.request.urlopen(req, timeout=5) as resp:
                    api_val = json.load(resp)

                # Web is a byte-semantic pass-through of the CLI/Validator
                # report; all optional diagnostic fields are compared.
                self.assertEqual(api_val, cli_val)

                proc_plan = subprocess.run(
                    [str(PIPELINE_TOOL), "plan", "--stdin"],
                    input=json.dumps(pipeline),
                    text=True,
                    capture_output=True,
                    cwd=ROOT,
                    check=False,
                )
                self.assertEqual(proc_plan.returncode, 1)
                cli_plan = json.loads(proc_plan.stdout)
                # Invalid plan requests retain the exact diagnostic report.
                self.assertEqual(cli_plan, cli_val)


if __name__ == "__main__":
    unittest.main()
