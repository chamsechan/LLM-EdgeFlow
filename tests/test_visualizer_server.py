#!/usr/bin/env python3
"""API and filesystem boundary tests for the local Pipeline Studio."""

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
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
        entity = json.loads((ROOT / "configs" / "pipeline_entity_extract.json").read_text())
        with self.assertRaises(SHOW.StudioError) as mismatch:
            self.service.start_run(entity, "keyword_match_mock")
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
            [str(ROOT / "build" / "alg_pipeline_tool"), *args],
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
        code, described = self.command("describe-node", "KeywordMatcherNode")
        self.assertEqual(code, 0)
        self.assertEqual(described["node_type"], "KeywordMatcherNode")
        code, initialized = self.command(
            "init", "--business", "keyword_match_v1", "--empty"
        )
        self.assertEqual(code, 0)
        self.assertEqual(initialized["pipeline"]["pipeline"], [])
        legacy = {
            "business_name": "entity_extract_0.6b_v1",
            "models": [
                {
                    "model_id": "llm_0.6b_entity",
                    "engine_type": "mock_npu_llm",
                    "model_path": "./models/qwen2.5_0.6b_instruct_npu.bin",
                    "config": {"max_batch_size": 2, "max_seq_len": 512},
                }
            ],
            "pipeline": [
                {"node_type": "EntityExtractPreNode"},
                {
                    "node_type": "LlmGenerateNode",
                    "config": {"bind_model": "llm_0.6b_entity"},
                },
                {"node_type": "EntityExtractPostNode"},
            ],
        }
        code, normalized = self.command(
            "normalize", "--explicit-dag", "--stdin", input_pipeline=legacy
        )
        self.assertEqual(code, 0)
        self.assertEqual(normalized["pipeline"]["pipeline"][0]["depends_on"], [])
        self.assertEqual(
            normalized["pipeline"]["pipeline"][1]["depends_on"],
            [normalized["pipeline"]["pipeline"][0]["id"]],
        )
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
        cases = [
            (
                "UNKNOWN_BUSINESS",
                {
                    "business_name": "unknown_biz_xyz",
                    "models": [],
                    "pipeline": [{"id": "node_0", "node_type": "KeywordMatcherNode", "depends_on": []}],
                },
                "UNKNOWN_BUSINESS",
            ),
            (
                "UNKNOWN_NODE_TYPE",
                {
                    "business_name": "keyword_match_v1",
                    "models": [],
                    "pipeline": [{"id": "node_0", "node_type": "CompletelyUnknownNode", "depends_on": []}],
                },
                "UNKNOWN_NODE_TYPE",
            ),
            (
                "MISSING_CONFIG_FIELD",
                {
                    "business_name": "keyword_match_v1",
                    "models": [],
                    "pipeline": [
                        {"id": "node_0", "node_type": "KeywordMatcherNode", "depends_on": [], "config": {}}
                    ],
                },
                "MISSING_CONFIG_FIELD",
            ),
            (
                "CONFIG_FIELD_ENUM",
                {
                    "business_name": "smart_doc_qa_v1",
                    "models": [
                        {"model_id": "embed_model_v1", "engine_type": "mock_npu_embedding"},
                        {"model_id": "rerank_model_v1", "engine_type": "mock_npu_rerank"},
                        {"model_id": "llm_model_v1", "engine_type": "mock_npu_llm"},
                    ],
                    "pipeline": [
                        {"id": "chunk_0", "node_type": "DocChunkPreNode", "depends_on": []},
                        {"id": "emb_0", "node_type": "DocEmbeddingNode", "depends_on": ["chunk_0"]},
                        {
                            "id": "search_0",
                            "node_type": "VectorSearchNode",
                            "depends_on": ["emb_0"],
                            "config": {"metric": "invalid_distance_metric"},
                        },
                    ],
                },
                "CONFIG_FIELD_ENUM",
            ),
            (
                "MODEL_CAPABILITY_MISMATCH",
                {
                    "business_name": "entity_extract_0.6b_v1",
                    "models": [{"model_id": "emb_model_id", "engine_type": "mock_npu_embedding"}],
                    "pipeline": [
                        {"id": "pre", "node_type": "EntityExtractPreNode", "depends_on": []},
                        {"id": "llm", "node_type": "LlmGenerateNode", "depends_on": ["pre"], "config": {"bind_model": "emb_model_id"}},
                        {"id": "post", "node_type": "EntityExtractPostNode", "depends_on": ["llm"]},
                    ],
                },
                "MODEL_CAPABILITY_MISMATCH",
            ),
            (
                "DAG_CYCLE",
                {
                    "business_name": "keyword_match_v1",
                    "pipeline": [
                        {"id": "a", "node_type": "KeywordMatcherNode", "depends_on": ["b"]},
                        {"id": "b", "node_type": "KeywordMatcherNode", "depends_on": ["a"]},
                    ],
                },
                "DAG_CYCLE",
            ),
            (
                "MISSING_INPUT_PRODUCER",
                {
                    "business_name": "smart_doc_qa_v1",
                    "pipeline": [{"id": "post_only", "node_type": "DocQaPostNode", "depends_on": []}],
                },
                "MISSING_INPUT_PRODUCER",
            ),
            (
                "PARALLEL_WRITE_CONFLICT",
                {
                    "business_name": "keyword_match_v1",
                    "execution_mode": "parallel",
                    "pipeline": [
                        {"id": "kw1", "node_type": "KeywordMatcherNode", "depends_on": []},
                        {"id": "kw2", "node_type": "KeywordMatcherNode", "depends_on": []},
                    ],
                },
                "PARALLEL_WRITE_CONFLICT",
            ),
            (
                "SERIALIZED_ENGINE_CONCURRENCY",
                {
                    "business_name": "entity_extract_0.6b_v1",
                    "execution_mode": "parallel",
                    "models": [{"model_id": "ser_llm", "engine_type": "mock_npu_llm"}],
                    "pipeline": [
                        {"id": "pre", "node_type": "EntityExtractPreNode", "depends_on": []},
                        {"id": "branch_a", "node_type": "LlmGenerateNode", "depends_on": ["pre"], "config": {"bind_model": "ser_llm"}},
                        {"id": "branch_b", "node_type": "LlmGenerateNode", "depends_on": ["pre"], "config": {"bind_model": "ser_llm"}},
                    ],
                },
                "SERIALIZED_ENGINE_CONCURRENCY",
            ),
        ]

        for desc, pipeline, expected_code in cases:
            with self.subTest(desc=desc):
                # 1. 直接通过 CLI 执行 validate (非法配置退出码为 1)
                proc_val = subprocess.run(
                    [str(ROOT / "build" / "alg_pipeline_tool"), "validate", "--stdin"],
                    input=json.dumps(pipeline),
                    text=True,
                    capture_output=True,
                    cwd=ROOT,
                    check=False,
                )
                self.assertEqual(proc_val.returncode, 1)
                cli_val = json.loads(proc_val.stdout)
                self.assertFalse(cli_val["ok"])
                found_cli = any(d.get("code") == expected_code for d in cli_val.get("diagnostics", []))
                self.assertTrue(found_cli, f"Expected {expected_code} in CLI output: {cli_val}")

                # 2. 通过 Web HTTP API 执行 /api/v1/validate
                req = urllib.request.Request(
                    self.base + "/validate",
                    data=json.dumps({"pipeline": pipeline}).encode(),
                    method="POST",
                    headers={"Content-Type": "application/json"},
                )
                with urllib.request.urlopen(req, timeout=5) as resp:
                    api_val = json.load(resp)

                # 3. 逐字段完全一致性断言 (证明 Web API 零二次计算，纯透传 CLI/Validator)
                self.assertEqual(api_val, cli_val)

                # 4. 执行 CLI plan 并验证在出现结构性错误时拦截
                proc_plan = subprocess.run(
                    [str(ROOT / "build" / "alg_pipeline_tool"), "plan", "--stdin"],
                    input=json.dumps(pipeline),
                    text=True,
                    capture_output=True,
                    cwd=ROOT,
                    check=False,
                )
                self.assertNotEqual(proc_plan.returncode, 0)
                cli_plan = json.loads(proc_plan.stdout)
                self.assertFalse(cli_plan["ok"])


if __name__ == "__main__":
    unittest.main()
