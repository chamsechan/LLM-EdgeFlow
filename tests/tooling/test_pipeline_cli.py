#!/usr/bin/env python3
"""Public Pipeline CLI contracts, exercised through real subprocesses."""

import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import jsonschema


REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "build/alg_pipeline_tool"
TEST_TOOL = REPO / "build/alg_pipeline_tool_test"


class PipelineCliTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="edgeflow-cli-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def command(self, *arguments, document=None, tool=None, ok=True):
        executable = TOOL if tool is None else tool
        self.assertTrue(executable.is_file(), f"Build the CLI prerequisite: {executable}")
        result = subprocess.run(
            [str(executable), *map(str, arguments)], cwd=REPO,
            input=None if document is None else json.dumps(document, ensure_ascii=False),
            text=True, capture_output=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0 if ok else 1,
                         f"{arguments}\nstdout: {result.stdout}\nstderr: {result.stderr}")
        try:
            response = json.loads(result.stdout)
        except ValueError:
            self.fail(f"CLI must return JSON: {arguments}\n{result.stdout}\n{result.stderr}")
        if "ok" in response:
            self.assertEqual(response["ok"], ok, response)
        return response

    @staticmethod
    def fields(definition):
        return {field["name"]: field for field in definition["config_fields"]}

    def fixture(self, relative):
        return json.loads((REPO / relative).read_text(encoding="utf-8"))

    def write_pipeline(self, document, name="pipeline.json"):
        path = self.directory / name
        path.write_text(json.dumps(document, ensure_ascii=False, indent=2), encoding="utf-8")
        return path

    def schema(self):
        schema = self.command("export-schema")
        validator_type = jsonschema.validators.validator_for(schema)
        validator_type.check_schema(schema)
        return schema, validator_type(schema)

    def test_catalog_exposes_selection_parameters_and_profile_io(self):
        catalog = self.command("catalog")
        self.assertTrue(catalog["models"])
        self.assertTrue(catalog["backends"])
        for model in catalog["models"]:
            self.assertTrue(model["impl_name"])
            self.assertTrue(model["model_type"])
            self.assertIsInstance(model["backends"], list)
            self.assertIsInstance(model["config_fields"], list)
        embedding = next(model for model in catalog["models"]
                         if model["model_type"] == "embedding"
                         and "onnxruntime" in model["backends"])
        self.assertTrue(self.fields(embedding)["tokenizer_file"]["file"])
        nodes = {node["node_type"]: node for node in catalog["nodes"]}
        for name, node in nodes.items():
            self.assertRegex(name, r"^[a-z][a-z0-9_]*$")
            for dependency in node["model_dependencies"]:
                self.assertTrue(dependency["model_type"])
                self.assertIn(dependency["config_field"], self.fields(node))
        endpoints = self.fields(nodes["llm_generate"])["endpoints"]
        self.assertEqual(endpoints["type"], "map")
        self.assertEqual(endpoints["items"]["type"], "object")
        self.assertEqual(endpoints["items"]["fields"][0]["name"], "prompt")
        self.assertEqual(endpoints["items"]["fields"][0]["default"], "{{input}}")
        categories = self.fields(nodes["text_rule_match"])["categories"]
        self.assertEqual(categories["type"], "map")
        self.assertEqual(categories["items"]["type"], "array")
        self.assertEqual(categories["items"]["items"]["type"], "string")
        self.assertEqual(self.fields(nodes["structured_json_parse"])["fallback"]["type"], "json")
        for converter in catalog["input_converters"] + catalog["output_converters"]:
            self.assertIsInstance(converter["config_fields"], list)
            for port in converter["logical_ports"]:
                self.assertRegex(port["key"], r"^[^.]+$")
        profiles = {profile["name"]: profile for profile in catalog["profiles"]}
        self.assertIn("keyword_match_rules", profiles)
        for profile in profiles.values():
            for direction in ("input", "output"):
                self.assertTrue(profile["io"][direction])
                for pair in profile["io"][direction]:
                    self.assertEqual(set(pair), {"type", "name"})
        expected = self.fixture("configs/pipeline_keyword_match_rules.json")
        self.assertEqual(profiles["keyword_match_rules"]["io"], {
            direction: [{"type": entry["type"], "name": entry["name"]}
                        for entry in expected["io"][direction]]
            for direction in ("input", "output")
        })

    def test_describe_model_selects_category_and_backend(self):
        catalog = self.command("catalog")
        selected = [model for model in catalog["models"]
                    if model["model_type"] == "llm" and "llama_cpp" in model["backends"]]
        self.assertEqual(len(selected), 1)
        described = self.command("describe-model", "llm", "llama_cpp")
        self.assertEqual(described["impl_name"], selected[0]["impl_name"])
        self.assertEqual(described["model_type"], "llm")
        self.assertEqual(described["config_fields"], selected[0]["config_fields"])
        self.assertIn("llama_cpp", described["backends"])
        mismatch = self.command("describe-model", "llm", "onnxruntime", ok=False)
        self.assertEqual(mismatch["diagnostics"][0]["code"], "BACKEND_PROTOCOL_MISMATCH")

    def test_init_preserves_repeated_pairs_as_an_unfilled_draft(self):
        response = self.command(
            "init", "--input", "keyword_in/keyword_match",
            "--input", "entity_in/entity_extract",
            "--output", "keyword_out/keyword_match",
            "--output", "entity_out/entity_extract",
        )
        self.assertEqual(response["pipeline"], {
            "io": {
                "input": [{"type": "keyword_in", "name": "keyword_match"},
                          {"type": "entity_in", "name": "entity_extract"}],
                "output": [{"type": "keyword_out", "name": "keyword_match"},
                           {"type": "entity_out", "name": "entity_extract"}],
            },
            "models": [], "pipeline": [],
        })

    def test_profile_clone_and_first_node_edit_complete_the_draft(self):
        original = self.fixture("configs/pipeline_keyword_match_rules.json")
        self.assertEqual(self.command("init", "--profile", "keyword_match_rules")["pipeline"], original)
        draft = self.command("init", "--input", "keyword_in/keyword_match",
                             "--output", "keyword_out/keyword_match")["pipeline"]
        added = self.command("edit", "--stdin", document={
            "pipeline": draft, "operation": {"kind": "add_node", "type": "text_rule_match"},
        })
        self.assertEqual(added["pipeline"]["pipeline"], [{"type": "text_rule_match", "name": "text_rule_match"}])
        self.assertFalse(added["validation"]["ok"])
        connected = self.command("edit", "--stdin", document={
            "pipeline": added["pipeline"], "require_valid": True,
            "operations": [
                {"kind": "connect", "source": {"node": "input", "port": "sentence_text"},
                 "target": {"node": "text_rule_match", "port": "text"}},
                {"kind": "connect", "source": {"node": "text_rule_match", "port": "matches"},
                 "target": {"node": "output", "port": "matches"}},
            ],
        })
        self.assertTrue(connected["validation"]["ok"], connected)
        node = connected["pipeline"]["pipeline"][0]
        self.assertEqual(node["inputs"], {"text": "input.sentence_text"})
        self.assertNotIn("params", node)
        self.assertEqual(connected["pipeline"]["io"]["output"][0]["inputs"],
                         {"matches": "text_rule_match.matches"})
        self.command("validate", "--stdin", document=connected["pipeline"])

    def test_resolution_uses_pipeline_directory_and_reports_declared_files(self):
        document = self.fixture("configs/pipeline_doc_qa_cpu.json")
        document["io"]["output"][0]["params"] = {"answer_text_max_bytes": 137}
        expected_files = set()
        for index, model in enumerate(document["models"]):
            model["file"] = f"artifacts/{model['name']}.weights"
            expected_files.add((model["name"], f"/models/{index}/file",
                                str(self.directory / model["file"])))
            if model["type"] == "embedding":
                model["params"]["tokenizer_file"] = "artifacts/vocab.txt"
                expected_files.add((model["name"], f"/models/{index}/params/tokenizer_file",
                                    str(self.directory / "artifacts/vocab.txt")))
        pipeline = self.write_pipeline(document)
        conf = self.directory / "pipeline.conf"
        conf.write_text(json.dumps({"pipe_path": pipeline.name}), encoding="utf-8")
        before = pipeline.read_bytes(), conf.read_bytes()
        checked_io = self.command("validate-io", conf)
        self.assertEqual(checked_io["io"]["input"][0], {
            "type": "doc_in", "name": "doc_qa",
            "external_type": "CompanyOperatorDocInput", "params": {},
        })
        output = checked_io["io"]["output"][0]
        self.assertEqual((output["type"], output["name"]), ("doc_out", "doc_qa"))
        self.assertEqual(output["external_type"], "CompanyOperatorDocOutput")
        self.assertEqual(output["params"]["answer_text_max_bytes"], 137)
        self.assertEqual(output["params"]["intent_name_max_bytes"], 63)
        resolved = self.command("resolve-conf", conf.name, "--root", self.directory,
                                "--depth", "3")["configuration"]
        self.assertEqual(resolved["pipeline_path"], str(pipeline))
        self.assertEqual(resolved["conf_path"], str(conf))
        self.assertEqual(resolved["effective_frame_depth"], 3)
        self.assertEqual(resolved["io"], checked_io["io"])
        self.assertEqual({(entry["model"], entry["path"], entry["resolved"])
                          for entry in resolved["model_files"]}, expected_files)
        effective = resolved["effective_pipeline"]
        for index, model in enumerate(effective["models"]):
            self.assertEqual(model["file"], str(self.directory / document["models"][index]["file"]))
            if model["type"] == "embedding":
                self.assertEqual(model["params"]["tokenizer_file"], str(self.directory / "artifacts/vocab.txt"))
        generated = next(node for node in effective["pipeline"] if node["type"] == "llm_generate")
        self.assertEqual(generated["params"]["endpoints"], {"answer": {"prompt": "{{input}}"}})
        self.assertEqual(generated["params"]["max_tokens"], 128)
        self.assertEqual(generated["params"]["temperature"], 0.3)
        self.assertEqual(json.loads(resolved["output_pools"]["doc_out"]["params"]), output["params"])
        self.assertEqual((pipeline.read_bytes(), conf.read_bytes()), before)

    def test_test_build_selects_fixture_category_without_loading_resources(self):
        document = self.fixture("demo/fixtures/mock/pipeline_entity_extract.json")
        pipeline = self.write_pipeline(document)
        self.command("validate", pipeline, tool=TEST_TOOL)
        self.command("validate", "--stdin", document=document, tool=TEST_TOOL)
        model = document["models"][0]
        described = self.command("describe-model", model["type"], model["backend"]["type"], tool=TEST_TOOL)
        self.assertEqual(described["model_type"], "llm")
        self.assertIn(model["backend"]["type"], described["backends"])

    def test_explain_output_fix_revalidates_the_complete_document(self):
        document = {
            "io": {
                "input": [{"type": "entity_in", "name": "entity_extract"}],
                "output": [{"type": "entity_out", "name": "entity_extract",
                            "inputs": {"entities": "extract.text"}}],
            },
            "models": [{"type": "llm", "name": "generator", "file": "artifacts/never_loaded.gguf",
                        "backend": {"type": "llama_cpp"}}],
            "pipeline": [{"type": "llm_generate", "name": "extract",
                          "params": {"bind_model": "generator", "endpoints": {"entities": {}}},
                          "inputs": {"input": "input.sentence_text"}}],
        }
        original = copy.deepcopy(document)
        path = self.write_pipeline(document)
        before = path.read_bytes()
        response = self.command("validate", path, "--explain", ok=False)
        diagnostics = [diagnostic for diagnostic in response["diagnostics"]
                       if diagnostic["code"] == "PORT_TYPE_MISMATCH"
                       and diagnostic["path"] == "/io/output/0/inputs/entities"]
        self.assertEqual(len(diagnostics), 1, response)
        fixes = diagnostics[0]["remediation"]["fixes"]
        self.assertTrue(fixes, response)
        fixed = False
        for fix in fixes:
            patched = copy.deepcopy(document)
            for operation in fix["patch"]:
                tokens = [token.replace("~1", "/").replace("~0", "~")
                          for token in operation["path"].split("/")[1:]]
                parent = patched
                for token in tokens[:-1]:
                    parent = parent[int(token)] if isinstance(parent, list) else parent[token]
                key = int(tokens[-1]) if isinstance(parent, list) else tokens[-1]
                if operation["op"] == "test":
                    self.assertEqual(parent[key], operation["value"])
                else:
                    self.assertEqual(operation["op"], "add")
                    parent[key] = copy.deepcopy(operation["value"])
            if patched["io"]["output"][0]["inputs"]["entities"] != "extract.document":
                continue
            self.assertEqual(fix["verification"], "pipeline_valid")
            patched_path = self.write_pipeline(patched, "patched.json")
            self.command("validate", patched_path)
            self.command("validate", "--stdin", document=patched)
            self.assertEqual(patched["pipeline"], document["pipeline"])
            self.assertEqual(patched["models"], document["models"])
            fixed = True
        self.assertTrue(fixed, response)
        self.assertEqual(document, original)
        self.assertEqual(path.read_bytes(), before)

    def test_exported_schema_validates_current_cpu_solutions(self):
        _, validator = self.schema()
        for name in ("pipeline_keyword_match_rules", "pipeline_doc_qa_cpu",
                     "pipeline_entity_extract_cpu", "pipeline_cross_rerank_cpu",
                     "pipeline_translate_cpu"):
            with self.subTest(pipeline=name):
                document = self.fixture(f"configs/{name}.json")
                validator.validate(document)
                self.command("validate", REPO / f"configs/{name}.json")

    def test_exported_schema_and_native_validation_cover_nested_parameter_types(self):
        _, validator = self.schema()
        base = self.fixture("configs/pipeline_keyword_match_rules.json")
        base["pipeline"][0]["params"] = {
            "categories": {"VIP": ["VIP"]},
            "rules": [{"pattern": "VIP", "constants": {
                "object": {"nested": [1, True]}, "array": [1, "two"],
                "string": "plain", "number": 2.5, "boolean": False,
            }}],
        }
        base["pipeline"].append({
            "type": "structured_json_parse", "name": "parse_unused",
            "params": {"fallback": ["fallback", {"value": True}]},
            "inputs": {"text": "input.sentence_text"},
        })
        validator.validate(base)
        self.command("validate", "--stdin", document=base)
        malformed = [
            ({"categories": {"VIP": ["valid", 42]}}, "/pipeline/0/params/categories/VIP/1"),
            ({"rules": [{"category": "VIP"}]}, "/pipeline/0/params/rules/0/pattern"),
            ({"rules": [{"pattern": "VIP", "score": 1.1}]}, "/pipeline/0/params/rules/0/score"),
            ({"rules": [{"pattern": "VIP", "constants": {"value": None}}]},
             "/pipeline/0/params/rules/0/constants/value"),
        ]
        for params, path in malformed:
            with self.subTest(params=params):
                document = copy.deepcopy(base)
                document["pipeline"][0]["params"] = params
                self.assertFalse(validator.is_valid(document))
                report = self.command("validate", "--stdin", document=document, ok=False)
                self.assertTrue(any(diagnostic["path"] == path for diagnostic in report["diagnostics"]), report)


class PipelineJsonSchemaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.builds = []
        tools = (TEST_TOOL, TOOL)
        for tool in dict.fromkeys(tools):
            artifacts = {}
            for command in ("catalog", "export-schema"):
                result = subprocess.run(
                    [str(tool), command], cwd=REPO, text=True,
                    capture_output=True, timeout=30, check=True)
                artifacts[command] = json.loads(result.stdout)
                artifacts[command + "_raw"] = result.stdout
            cls.builds.append((tool, artifacts))

    def test_export_is_deterministic_bare_draft07_schema(self):
        for tool, artifacts in self.builds:
            with self.subTest(tool=tool):
                schema = artifacts["export-schema"]
                self.assertEqual(schema["$schema"],
                                 "http://json-schema.org/draft-07/schema#")
                self.assertEqual(schema["type"], "object")
                jsonschema.Draft7Validator.check_schema(schema)
                self.assertNotIn("ok", schema)
                repeated = subprocess.run(
                    [str(tool), "export-schema"], cwd=REPO, text=True,
                    capture_output=True, timeout=30, check=True)
                self.assertEqual(repeated.stdout, artifacts["export-schema_raw"])
                catalog = subprocess.run(
                    [str(tool), "catalog"], cwd=REPO, text=True,
                    capture_output=True, timeout=30, check=True)
                self.assertEqual(catalog.stdout, artifacts["catalog_raw"])

    def test_export_rejects_arguments(self):
        for arguments in (("--stdin",), ("pipeline.json",), ("--output", "schema.json")):
            with self.subTest(arguments=arguments):
                result = subprocess.run(
                    [str(TEST_TOOL), "export-schema", *arguments],
                    cwd=REPO, text=True, capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    def test_each_build_exposes_exactly_its_registered_choices(self):
        for tool, artifacts in self.builds:
            with self.subTest(tool=tool):
                catalog = artifacts["catalog"]
                schema = artifacts["export-schema"]
                properties = schema["properties"]
                nodes = properties["pipeline"]["items"]
                self.assertCountEqual(nodes["properties"]["type"]["enum"],
                                      [node["node_type"] for node in catalog["nodes"]])
                self.assertCountEqual(schema["required"], ["pipeline", "io"])
                self.assertEqual(properties["max_parallel_workers"]["minimum"], 1)
                self.assertEqual(properties["max_parallel_workers"]["maximum"], 64)
                model = properties["models"]["items"]["properties"]
                self.assertCountEqual(model["type"]["enum"], sorted({d["model_type"] for d in catalog["models"]}))
                backend_type = model["backend"]["properties"]["type"]
                if catalog["backends"]:
                    self.assertCountEqual(backend_type["enum"], [d["backend_type"] for d in catalog["backends"]])
                else:
                    self.assertIs(backend_type, False)

    def test_editor_associates_schema_without_changing_pipeline_documents(self):
        settings = json.loads((REPO / "edgeflow.code-workspace").read_text())["settings"]
        associations = settings["json.schemas"]
        import fnmatch
        representative = "/configs/pipeline_keyword_match_rules.json"
        self.assertTrue(any(
            a.get("url") and any(fnmatch.fnmatchcase(representative, pattern)
                                 for pattern in a["fileMatch"])
            for a in associations))
        paths = list((REPO / "configs").glob("pipeline_*.json"))
        paths += list((REPO / "demo/fixtures").rglob("pipeline_*.json"))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(path=path):
                self.assertNotIn("$schema", json.loads(path.read_text()))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--tool", type=Path, default=TOOL)
    parser.add_argument("--test-tool", type=Path, default=TEST_TOOL)
    arguments, remaining = parser.parse_known_args()
    REPO, TOOL, TEST_TOOL = (arguments.repo.resolve(), arguments.tool.resolve(),
                             arguments.test_tool.resolve())
    unittest.main(argv=[sys.argv[0], *remaining])
