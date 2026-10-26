#!/usr/bin/env python3
"""Public CLI status, JSON protocol, editing and serialized artifacts."""

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = Path(os.environ.get("LLM_EDGEFLOW_PIPELINE_TOOL", ROOT / "build/alg_pipeline_tool"))


class PipelineCliContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TOOL.is_file():
            raise RuntimeError(f"Build alg_pipeline_tool first or set LLM_EDGEFLOW_PIPELINE_TOOL: {TOOL}")
        result = subprocess.run([str(TOOL), "init", "--profile", "keyword_match_rules"],
                                cwd=ROOT, capture_output=True, text=True, check=True)
        cls.base = json.loads(result.stdout)["pipeline"]

    def call(self, *args, document=None, raw_input=None, expected=0):
        result = subprocess.run([str(TOOL), *args], cwd=ROOT, capture_output=True,
                                text=True, input=json.dumps(document) if document is not None else raw_input)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        try:
            payload = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            self.fail(f"CLI did not return one JSON document: {result.stdout!r}; {result.stderr}; {error}")
        return payload

    def test_catalog_description_and_schema_are_machine_readable(self):
        catalog = self.call("catalog")
        self.assertTrue(catalog["ok"])
        rule = next(item for item in catalog["nodes"] if item["node_type"] == "text_rule_match")
        self.assertIn("text", [port["key"] for port in rule["inputs"]])
        self.assertIn("matches", [port["key"] for port in rule["outputs"]])
        described = self.call("describe-node", "text_rule_match")
        self.assertTrue(described["ok"])
        self.assertEqual(described["node_type"], rule["node_type"])
        schema = self.call("export-schema")
        self.assertEqual(schema["type"], "object")
        self.assertIn("pipeline", schema["properties"])

    def test_stdin_and_file_plan_have_the_same_order_and_layers(self):
        stdin = self.call("plan", "--stdin", document=self.base)
        self.assertTrue(stdin["ok"])
        self.assertEqual(stdin["plan"]["topological_order"], ["match_keywords"])
        self.assertEqual(stdin["plan"]["layers"], [["match_keywords"]])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pipeline.json"
            path.write_text(json.dumps(self.base), encoding="utf-8")
            before = path.read_bytes()
            loaded = self.call("plan", str(path))
            self.assertEqual(loaded["plan"], stdin["plan"])
            self.assertEqual(path.read_bytes(), before)

    def test_one_validator_failure_preserves_native_code_path_and_exit_status(self):
        invalid = copy.deepcopy(self.base)
        invalid["pipeline"][0]["type"] = "missing_node"
        report = self.call("validate", "--stdin", document=invalid, expected=1)
        self.assertFalse(report["ok"])
        errors = {(item["code"], item["path"]) for item in report["diagnostics"]}
        self.assertIn(("UNKNOWN_NODE_TYPE", "/pipeline/0/type"), errors)
        malformed = self.call("validate", "--stdin", raw_input="{", expected=1)
        self.assertEqual(malformed["diagnostics"][0]["code"], "JSON_READ")

    def test_usage_failure_is_distinct_from_json_validation_failure(self):
        result = subprocess.run([str(TOOL), "unknown-command"], cwd=ROOT,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("alg_pipeline_tool", result.stderr)

    def test_raw_profile_and_schema_exports_are_complete_reusable_json_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "exported.json"
            with path.open("wb") as output:
                result = subprocess.run([str(TOOL), "init", "--profile", "keyword_match_rules", "--raw"],
                                        cwd=ROOT, stdout=output, stderr=subprocess.PIPE)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(path.read_text()), self.base)
            self.assertTrue(self.call("validate", str(path))["ok"])
            schema_path = Path(directory) / "schema.json"
            with schema_path.open("wb") as output:
                result = subprocess.run([str(TOOL), "export-schema"], cwd=ROOT,
                                        stdout=output, stderr=subprocess.PIPE)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(schema_path.read_text())["type"], "object")

    def test_rename_updates_output_bindings_and_batch_error_returns_no_candidate(self):
        request = {"pipeline": self.base, "require_valid": True,
                   "operation": {"kind": "rename_node", "node": "match_keywords", "new_name": "renamed"}}
        result = self.call("edit", "--stdin", document=request)
        self.assertTrue(result["ok"])
        self.assertTrue(result["validation"]["ok"])
        self.assertEqual(result["pipeline"]["pipeline"][0]["name"], "renamed")
        self.assertEqual(result["pipeline"]["io"]["output"][0]["inputs"]["matches"], "renamed.matches")
        failed = self.call("edit", "--stdin", document={"pipeline": self.base, "operations": [
            request["operation"], {"kind": "remove_node", "node": "absent"}]}, expected=1)
        self.assertFalse(failed["ok"])
        self.assertEqual(failed["failed_operation_index"], 1)
        self.assertNotIn("pipeline", failed)
        self.assertEqual(self.base["pipeline"][0]["name"], "match_keywords")

    def test_patch_uses_native_semantics_and_keeps_batch_failure_atomic(self):
        patch = {"kind": "apply_patch", "patch": [
            {"op": "test", "path": "/pipeline/0/name", "value": "match_keywords"},
            {"op": "copy", "from": "/pipeline/0/name", "path": "/comment"},
        ]}
        result = self.call("edit", "--stdin", document={
            "pipeline": self.base, "operation": patch, "require_valid": True})
        self.assertTrue(result["validation"]["ok"])
        self.assertEqual(result["pipeline"]["comment"], "match_keywords")
        for invalid in (
            {"op": "add", "path": "/pipeline/99", "value": {}},
            {"op": "remove", "path": "/pipeline/-1"},
            {"op": "replace", "path": "/absent", "value": "missing"},
        ):
            with self.subTest(patch=invalid):
                failed = self.call("edit", "--stdin", document={
                    "pipeline": self.base, "operations": [patch, {"kind": "apply_patch", "patch": [invalid]}],
                }, expected=1)
                self.assertEqual(failed["failed_operation_index"], 1)
                self.assertNotIn("pipeline", failed)
        self.assertNotIn("comment", self.base)

    def test_deployment_resolution_reports_the_selected_file_and_effective_io(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory).resolve()
            (directory / "pipeline.json").write_text(json.dumps(self.base), encoding="utf-8")
            conf = directory / "pipeline.conf"
            conf.write_text('{"pipe_path":"pipeline.json"}', encoding="utf-8")
            result = self.call("resolve-conf", str(conf), "--root", str(directory), "--depth", "2")
            self.assertTrue(result["ok"])
            configuration = result["configuration"]
            self.assertEqual(Path(configuration["pipeline_path"]), directory / "pipeline.json")
            self.assertEqual(configuration["io"]["input"][0]["name"], "keyword_match")
            self.assertEqual(configuration["io"]["output"][0]["name"], "keyword_match")
            self.assertEqual(configuration["effective_frame_depth"], 2)

    def test_resolve_reports_the_effective_decode_batch_size(self):
        catalog = self.call("catalog")
        if not any(backend["backend_type"] == "llama_cpp" for backend in catalog["backends"]):
            self.skipTest("llama_cpp is not enabled in this build")
        pipeline = json.loads((ROOT / "configs/pipeline_translate_cpu.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / "pipeline.conf").write_text('{"pipe_path":"pipeline.json"}', encoding="utf-8")
            for params, expected in (
                ({}, 512),
                ({"context_size": 128}, 128),
                ({"context_size": 128, "decode_batch_size": 64}, 64),
            ):
                with self.subTest(params=params):
                    pipeline["models"][0]["backend"]["params"] = params
                    (directory / "pipeline.json").write_text(json.dumps(pipeline), encoding="utf-8")
                    result = self.call("resolve-conf", str(directory / "pipeline.conf"),
                                       "--root", str(directory))
                    effective = result["configuration"]["effective_pipeline"]
                    self.assertEqual(effective["models"][0]["backend"]["params"]["decode_batch_size"],
                                     expected)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", type=Path, default=TOOL)
    parser.add_argument("--test-tool", type=Path)
    parser.add_argument("--repo", type=Path, default=ROOT)
    arguments, remaining = parser.parse_known_args()
    TOOL = arguments.tool.resolve()
    ROOT = arguments.repo.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
