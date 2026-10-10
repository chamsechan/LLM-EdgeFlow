#!/usr/bin/env python3
"""Public Pipeline CLI contracts, exercised through real subprocesses."""

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import jsonschema


REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "build/alg_pipeline_tool"
TEST_TOOL = REPO / "build/alg_pipeline_tool_test"
VIEWER = Path(os.environ.get("LLM_EDGEFLOW_ALG_SHOW", REPO / "build/alg_show"))


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


class PipelineCommandsTest(unittest.TestCase):
    CLI_PARITY_ENTRYPOINTS = [
        ("validate",),
        ("plan",),
        ("validate", "--explain"),
    ]

    def command(self, *args, input_pipeline=None):
        process = subprocess.run(
            [str(TEST_TOOL), *args],
            input=None if input_pipeline is None else json.dumps(input_pipeline),
            text=True,
            capture_output=True,
            cwd=REPO,
            check=False,
        )
        payload = json.loads(process.stdout)
        return process.returncode, payload

    def test_production_tool_explains_unknown_registrations(self):
        production = TOOL
        fixture = "demo/fixtures/mock/pipeline_entity_extract_custom.json"
        for command in [("validate", fixture), ("plan", fixture),
                        ("validate", fixture, "--explain"), ("plan", fixture, "--explain"),
                        ("describe-model", "llm", "test_causal_lm_backend"),
                        ("describe-backend", "test_causal_lm_backend"),
                        ("validate-io", "demo/fixtures/mock/pipeline_entity_extract_custom.conf"),
                        ("resolve-conf", "demo/fixtures/mock/pipeline_entity_extract_custom.conf")]:
            with self.subTest(command=command):
                process = subprocess.run([str(production), *command], cwd=REPO,
                                         text=True, capture_output=True)
                self.assertEqual(process.returncode, 1, process.stdout + process.stderr)
                payload = json.loads(process.stdout)
                self.assertFalse(payload["ok"])
                if command[0] in ("validate", "plan"):
                    self.assertEqual([d["code"] for d in payload["diagnostics"]],
                                     ["UNKNOWN_BACKEND"])
                self.assertIn("alg_pipeline_tool_test", process.stderr)
                self.assertEqual(process.stderr.count("提示："), 1)
                test_process = subprocess.run([str(TEST_TOOL), *command], cwd=REPO,
                                              text=True, capture_output=True)
                self.assertEqual(test_process.returncode, 0, test_process.stdout + test_process.stderr)
                self.assertTrue(json.loads(test_process.stdout)["ok"])
                self.assertNotIn("提示：", test_process.stderr)

    def test_edit_explains_unknown_registrations(self):
        production = TOOL
        fixture = json.loads((REPO / "demo/fixtures/mock/pipeline_entity_extract_custom.json").read_text())
        for require_valid in (False, True):
            with self.subTest(require_valid=require_valid):
                request = {
                    "pipeline": fixture, "require_valid": require_valid,
                    "operation": {"kind": "rename_node", "node": "generate_entities",
                                  "new_name": "renamed_prompt"},
                }
                process = subprocess.run([str(production), "edit", "--stdin"], cwd=REPO,
                                         input=json.dumps(request), text=True, capture_output=True)
                payload = json.loads(process.stdout)
                self.assertEqual(process.returncode, int(require_valid), payload)
                self.assertEqual(payload["ok"], not require_valid)
                self.assertEqual("pipeline" in payload, not require_valid)
                self.assertFalse(payload["validation"]["ok"])
                self.assertEqual([d["code"] for d in payload["validation"]["diagnostics"]],
                                 ["UNKNOWN_BACKEND"])
                self.assertIn("alg_pipeline_tool_test", process.stderr)
                self.assertIn("构建变体", process.stderr)
                self.assertEqual(process.stderr.count("提示："), 1)
                test_process = subprocess.run([str(TEST_TOOL), "edit", "--stdin"], cwd=REPO,
                                              input=json.dumps(request), text=True, capture_output=True)
                test_payload = json.loads(test_process.stdout)
                self.assertEqual(test_process.returncode, 0, test_payload)
                self.assertTrue(test_payload["validation"]["ok"])
                self.assertNotIn("提示：", test_process.stderr)

        keyword = json.loads((REPO / "configs/pipeline_keyword_match_rules.json").read_text())
        node_name = keyword["pipeline"][0]["name"]
        for operation in ({"kind": "rename_node", "node": node_name, "new_name": "renamed_rule"},
                          {"kind": "remove_node", "node": node_name}):
            with self.subTest(operation=operation):
                request = {"pipeline": keyword, "require_valid": False,
                           "operation": operation}
                process = subprocess.run([str(production), "edit", "--stdin"], cwd=REPO,
                                         input=json.dumps(request), text=True, capture_output=True)
                payload = json.loads(process.stdout)
                self.assertEqual(process.returncode, 0, payload)
                self.assertEqual(payload["validation"]["ok"], operation["kind"] == "rename_node")
                self.assertNotIn("提示：", process.stderr)

    def test_selected_output_uses_native_defaults_without_writing_them(self):
        original = json.loads((REPO / "configs/pipeline_keyword_match_rules.json").read_text())
        for explicit in (False, True):
            with self.subTest(explicit=explicit), tempfile.TemporaryDirectory() as directory:
                pipeline = copy.deepcopy(original)
                if explicit:
                    pipeline["io"]["output"][0]["params"] = {}
                before = json.dumps(pipeline)
                code, validated = self.command("validate", "--stdin", input_pipeline=pipeline)
                self.assertEqual(code, 0, validated)
                path = Path(directory)
                (path / "pipeline.json").write_text(before)
                (path / "pipeline.conf").write_text(json.dumps({"pipe_path": "pipeline.json"}))
                code, resolved = self.command("validate-io", str(path / "pipeline.conf"))
                self.assertEqual(code, 0, resolved)
                self.assertEqual(resolved["io"]["output"][0]["type"], "keyword_out")
                self.assertEqual(set(resolved["output_pools"]), {"keyword_out"})
                self.assertEqual(resolved["output_pools"]["keyword_out"]["capacities"],
                                 {"match_result_json": 2047})
                self.assertEqual((path / "pipeline.json").read_text(), before)

    def test_external_io_lists_and_pair_fields_are_required(self):
        original = json.loads((REPO / "configs/pipeline_keyword_match_rules.json").read_text())
        variants = [("/io", None), ("/io/input", None), ("/io/output", None),
                    ("/io/input/0/type", None), ("/io/output/0/name", None)]
        for path, _ in variants:
            pipeline = copy.deepcopy(original)
            parts = path.strip("/").split("/")
            parent = pipeline
            for key in parts[:-1]:
                parent = parent[int(key)] if isinstance(parent, list) else parent[key]
            del parent[parts[-1]]
            for entrypoint in self.CLI_PARITY_ENTRYPOINTS:
                with self.subTest(path=path, entrypoint=entrypoint):
                    code, result = self.command(*entrypoint, "--stdin", input_pipeline=pipeline)
                    self.assertEqual(code, 1, result)
                    self.assertEqual(result["diagnostics"][0]["path"], path)

    def test_all_commands_return_json(self):
        first_code, first = self.command("catalog")
        second_code, second = self.command("catalog")
        self.assertEqual((first_code, first), (second_code, second))
        self.assertTrue(first["nodes"])
        self.assertTrue(first["profiles"])
        self.assertTrue(all(set(profile["io"]) == {"input", "output"} for profile in first["profiles"]))
        code, described = self.command("describe-node", "text_rule_match")
        self.assertEqual(code, 0)
        self.assertEqual(described["node_type"], "text_rule_match")
        code, initialized = self.command("init", "--input", "keyword_in/keyword_match",
                                         "--output", "keyword_out/keyword_match")
        self.assertEqual(code, 0, initialized)
        self.assertEqual(initialized["pipeline"]["pipeline"], [])
        self.assertEqual(initialized["pipeline"]["io"], {
            "input": [{"type": "keyword_in", "name": "keyword_match"}],
            "output": [{"type": "keyword_out", "name": "keyword_match"}]})
        pipeline = json.loads((REPO / "configs/pipeline_keyword_match_rules.json").read_text())
        code, validated = self.command("validate", "--stdin", input_pipeline=pipeline)
        self.assertEqual(code, 0, validated)
        self.assertTrue(validated["ok"])
        code, plan = self.command("plan", "--stdin", input_pipeline=pipeline)
        self.assertEqual(code, 0, plan)
        self.assertNotIn("diagnostics", plan)
        self.assertTrue(plan["plan"]["topological_order"])

    def test_catalog_profile_identity_does_not_require_model_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "demo").mkdir()
            (root / "demo/profiles.json").write_text(json.dumps({"profiles": {
                "unavailable_models": {"config": "pipeline.conf", "dataset": "unused.txt"}}}))
            (root / "pipeline.conf").write_text(json.dumps({"pipe_path": "pipeline.json"}))
            io = {"input": [{"type": "keyword_in", "name": "keyword_match"}],
                  "output": [{"type": "keyword_out", "name": "keyword_match"}]}
            pipeline = {"io": io, "models": [{"type": "unregistered_in_this_build"}], "pipeline": []}
            (root / "pipeline.json").write_text(json.dumps(pipeline))
            process = subprocess.run([str(TEST_TOOL), "catalog"], cwd=root, capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            profiles = json.loads(process.stdout)["profiles"]
            self.assertEqual([p["name"] for p in profiles], ["unavailable_models"])
            self.assertEqual(profiles[0]["io"], io)

    def test_describe_models_and_backends_match_catalog(self):
        code, catalog = self.command("catalog")
        self.assertEqual(code, 0)
        self.assertTrue(catalog["models"])
        self.assertTrue(catalog["backends"])
        for definition in catalog["models"]:
            for backend in definition["backends"]:
                with self.subTest(model=definition["impl_name"], backend=backend):
                    code, described = self.command("describe-model", definition["model_type"], backend)
                    self.assertEqual(code, 0, described)
                    self.assertEqual(described, {**definition, "ok": True})
        for definition in catalog["backends"]:
            code, described = self.command("describe-backend", definition["backend_type"])
            self.assertEqual(code, 0, described)
            self.assertEqual(described, {**definition, "ok": True})

    def test_describe_models_and_backends_reject_unknown_names(self):
        for command, arguments, diagnostic in (
            ("describe-model", ("not_registered", "onnxruntime"), "UNKNOWN_MODEL_TYPE"),
            ("describe-model", ("llm", "not_registered"), "UNKNOWN_BACKEND"),
            ("describe-backend", ("not_registered",), "UNKNOWN_BACKEND"),
        ):
            with self.subTest(command=command, arguments=arguments):
                code, described = self.command(command, *arguments)
                self.assertEqual(code, 1)
                self.assertFalse(described["ok"])
                self.assertEqual(described["diagnostics"][0]["code"], diagnostic)
                self.assertEqual(described["diagnostics"][0]["path"], "/")
                self.assertEqual(described["diagnostics"][0]["severity"], "error")

    def test_describe_commands_require_their_declared_arguments(self):
        for command, variants in (
            ("describe-model", [(), ("llm",), ("llm", "llama_cpp", "extra")]),
            ("describe-backend", [(), ("name", "extra"), ("name", "--raw")]),
        ):
            for arguments in variants:
                with self.subTest(command=command, arguments=arguments):
                    process = subprocess.run([str(TEST_TOOL), command, *arguments],
                                             text=True, capture_output=True, cwd=REPO, check=False)
                    self.assertEqual(process.returncode, 2)
                    self.assertEqual(process.stdout, "")
                    self.assertIn("Usage:", process.stderr)

    def test_init_raw_can_be_saved_and_validated_without_unwrapping(self):
        args = ["init", "--profile", "keyword_match_rules"]
        code, wrapped = self.command(*args)
        self.assertEqual(code, 0, wrapped)
        raw = subprocess.run([str(TEST_TOOL), *args, "--raw"],
                             text=True, capture_output=True, cwd=REPO, check=False)
        self.assertEqual(raw.returncode, 0, raw.stderr)
        pipeline = json.loads(raw.stdout)
        self.assertEqual(pipeline, wrapped["pipeline"])
        self.assertNotIn("ok", pipeline)
        code, validated = self.command("validate", "--stdin", input_pipeline=pipeline)
        self.assertEqual(code, 0, validated)
        with tempfile.TemporaryDirectory() as directory:
            saved = Path(directory) / "pipeline_cloned.json"
            saved.write_text(raw.stdout)
            code, validated = self.command("validate", str(saved))
            self.assertEqual(code, 0, validated)
        empty = subprocess.run([str(TEST_TOOL), "init", "--raw", "--input", "keyword_in/keyword_match",
                               "--output", "keyword_out/keyword_match"],
                               text=True, capture_output=True, cwd=REPO, check=False)
        self.assertEqual(empty.returncode, 0, empty.stderr)
        self.assertEqual(json.loads(empty.stdout), {
            "io": {"input": [{"type": "keyword_in", "name": "keyword_match"}],
                   "output": [{"type": "keyword_out", "name": "keyword_match"}]},
            "models": [], "pipeline": []})

    def test_init_rejects_invalid_options(self):
        for options in (["--profile", "keyword_match_rules", "--input", "keyword_in/keyword_match"],
                        ["--profile"], ["--profile", "--raw"], ["--unknown"],
                        ["--raw", "--raw"], ["--input"], ["--output"],
                        ["--input", "keyword_in"],
                        ["--profile", "keyword_match_rules", "--profile", "keyword_match_rules"]):
            with self.subTest(options=options):
                process = subprocess.run([str(TEST_TOOL), "init", *options],
                                         text=True, capture_output=True, cwd=REPO, check=False)
                self.assertEqual(process.returncode, 2)
                self.assertIn("Usage:", process.stderr)
                self.assertEqual(process.stdout, "")

    def test_resolve_conf_exposes_model_sources_defaults_and_native_pool_errors(self):
        # 解析器语义必须在所有 Backend 变体中运行，
        # 包括 Kite 构建和有意不含 llama.cpp 的最小构建。
        conf_path = REPO / "demo/fixtures/mock/pipeline_entity_extract.conf"
        code, report = self.command("resolve-conf", str(conf_path.relative_to(REPO)), "--root", str(REPO), "--depth", "1")
        self.assertEqual(code, 0, report)
        configuration = report["configuration"]
        self.assertEqual(configuration["io"]["input"][0]["type"], "entity_in")
        self.assertEqual(configuration["io"]["output"][0]["name"], "entity_extract")
        self.assertEqual(configuration["effective_frame_depth"], 1)
        self.assertEqual(configuration["effective_process_batch_limit"], 1)
        self.assertEqual(configuration["max_frame_depth_limit"], 1024)
        for depth, normalized, batch_limit in [(0, 25, 25), (1, 1, 1), (25, 25, 25), (64, 64, 64), (100, 100, 64)]:
            code, limits = self.command("resolve-conf", "configs/pipeline_keyword_match_rules.conf", "--root", str(REPO), "--depth", str(depth))
            self.assertEqual(code, 0, limits)
            self.assertEqual(limits["configuration"]["effective_frame_depth"], normalized)
            self.assertEqual(limits["configuration"]["effective_process_batch_limit"], batch_limit)
        code, oversized = self.command("resolve-conf", "configs/pipeline_keyword_match_rules.conf", "--root", str(REPO), "--depth", "1025")
        self.assertEqual(code, 1)
        self.assertEqual(oversized["diagnostics"][0]["message"], "max_frame_depth (1025) exceeds hard limit 1024")
        self.assertEqual(configuration["effective_pipeline"]["io"]["input"][0]["name"], "entity_extract")
        self.assertEqual(configuration["effective_pipeline"]["models"][0]["file"], str(REPO / "demo/fixtures/mock/artifacts/neutral-llm.fixture"))
        self.assertEqual(configuration["conf_path"], str(conf_path))
        self.assertEqual(configuration["model_files"], [{
            "model": "entity_llm", "path": "/models/0/file",
            "resolved": str(REPO / "demo/fixtures/mock/artifacts/neutral-llm.fixture"),
        }])
        llm_config = configuration["effective_pipeline"]["pipeline"][1]["params"]
        self.assertEqual(llm_config["max_tokens"], 64)
        self.assertEqual(llm_config["top_p"], 0.9, "omitted defaults must come from the native validated plan")
        conf = json.loads(conf_path.read_text())
        with tempfile.TemporaryDirectory(prefix="resolve-conf-", dir=REPO / "build") as directory:
            changed = Path(directory) / "pipeline.conf"
            pipe_file = conf_path.with_name(conf["pipe_path"])
            pipe_doc = json.loads(pipe_file.read_text())
            (Path(directory) / conf["pipe_path"]).write_text(json.dumps(pipe_doc))
            changed.write_text(json.dumps(conf))
            code, direct = self.command("resolve-conf", str(changed.relative_to(REPO)), "--root", str(REPO))
            self.assertEqual(code, 0, direct)
            self.assertEqual(direct["configuration"]["model_files"][0]["path"], "/models/0/file")
            self.assertEqual(direct["configuration"]["model_files"][0]["resolved"],
                             str(Path(directory) / pipe_doc["models"][0]["file"]))
            output = next(entry for entry in pipe_doc["io"]["output"]
                          if entry["type"] == "entity_out")
            output.setdefault("params", {})["entities_json_max_bytes"] = 0
            (Path(directory) / conf["pipe_path"]).write_text(json.dumps(pipe_doc))
            code, rejected = self.command("resolve-conf", str(changed.relative_to(REPO)), "--root", str(REPO))
            self.assertEqual(code, 1)
            self.assertFalse(rejected["ok"])
            self.assertEqual(rejected["diagnostics"][0]["code"], "DEPLOYMENT_CONFIG")
            self.assertEqual(rejected["diagnostics"][0]["path"], "/io/output/0/params/entities_json_max_bytes")
            self.assertIn("entities_json", rejected["diagnostics"][0]["message"])

            pipe_doc["io"]["output"][0].pop("params")
            (Path(directory) / conf["pipe_path"]).write_text(json.dumps(pipe_doc))
            code, defaulted = self.command("resolve-conf", str(changed.relative_to(REPO)), "--root", str(REPO))
            self.assertEqual(code, 0, defaulted)
            self.assertEqual(defaulted["configuration"]["output_pools"]["entity_out"]["capacities"], {"entities_json": 2047})

            bad_conf = Path(directory) / "bad.conf"
            bad_conf.write_text("{}")
            code, bad_res = self.command("resolve-conf", str(bad_conf.relative_to(REPO)), "--root", str(REPO))
            self.assertEqual(code, 1)
            self.assertFalse(bad_res["ok"])
            self.assertEqual(bad_res["diagnostics"][0]["code"], "DEPLOYMENT_CONFIG")
            self.assertEqual(bad_res["diagnostics"][0]["path"], "/pipe_path")
            self.assertIn("pipe_path", bad_res["diagnostics"][0]["message"])

            code, bad_res_root = self.command("resolve-conf", "bad.conf", "--root", str(directory))
            self.assertEqual(code, 1)
            self.assertFalse(bad_res_root["ok"])
            self.assertEqual(bad_res_root["diagnostics"][0]["code"], "DEPLOYMENT_CONFIG")
            self.assertEqual(bad_res_root["diagnostics"][0]["path"], "/pipe_path")
            self.assertIn("pipe_path", bad_res_root["diagnostics"][0]["message"])

    def test_cli_model_file_is_required_and_typed(self):
        pipeline = json.loads((REPO / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        cases = [(None, "FIELD_TYPE"), ("", "INVALID_FILE_PATH"), (12345, "FIELD_TYPE")]
        for value, expected_code in cases + [("missing", "MISSING_FIELD")]:
            document = copy.deepcopy(pipeline)
            if value == "missing":
                document["models"][0].pop("file")
            else:
                document["models"][0]["file"] = value
            for endpoint in self.CLI_PARITY_ENTRYPOINTS:
                with self.subTest(value=value, endpoint=endpoint):
                    code, report = self.command(*endpoint, "--stdin", input_pipeline=document)
                    self.assertEqual(code, 1)
                    self.assertEqual(report["diagnostics"][0]["code"], expected_code)
                    self.assertEqual(report["diagnostics"][0]["path"], "/models/0/file")
                    if endpoint[0] == "plan":
                        self.assertEqual(report["plan"], {"layers": [], "topological_order": []})

    def test_cli_plan_envelopes_across_entrypoints(self):
        # 部署准备失败时 plan 返回带诊断的信封，Core 失败时返回部分计划，
        # 成功时返回完整拓扑顺序，且与 CLI 一致。
        pipeline = json.loads(
            (REPO / "demo/fixtures/mock/pipeline_entity_extract.json").read_text()
        )
        # 1. 部署段的未知字段被拒绝
        invalid_doc = copy.deepcopy(pipeline)
        invalid_doc["io"]["unknown_field"] = 1
        for ep in self.CLI_PARITY_ENTRYPOINTS:
            with self.subTest(case="deployment_failure", entrypoint=ep):
                code, res = self.command(*ep, "--stdin", input_pipeline=invalid_doc)
                self.assertEqual(code, 1)
                self.assertFalse(res["ok"])
                self.assertIn("diagnostics", res)
                self.assertEqual(res["diagnostics"][0]["code"], "DEPLOYMENT_ERROR")
                self.assertEqual(res["diagnostics"][0]["path"], "/io/unknown_field")
                if ep[0] == "plan":
                    self.assertEqual(res["plan"], {"layers": [], "topological_order": []})

        # 2. 部署合法但 Core 失败
        invalid_core = copy.deepcopy(pipeline)
        invalid_core["pipeline"].append({'name': 'bad_node', 'type': 'completely_unknown_node_type', 'depends_on': []})
        for ep in self.CLI_PARITY_ENTRYPOINTS:
            with self.subTest(case="core_failure", entrypoint=ep):
                code, res = self.command(*ep, "--stdin", input_pipeline=invalid_core)
                self.assertEqual(code, 1)
                self.assertFalse(res["ok"])
                self.assertIn("diagnostics", res)
                self.assertEqual(res["diagnostics"][0]["code"], "UNKNOWN_NODE_TYPE")
                if ep[0] == "plan":
                    self.assertIn("plan", res)

        # 3. 合法的 Pipeline 计划
        for ep in self.CLI_PARITY_ENTRYPOINTS:
            with self.subTest(case="valid_plan", entrypoint=ep):
                code, res = self.command(*ep, "--stdin", input_pipeline=pipeline)
                self.assertEqual(code, 0)
                self.assertTrue(res["ok"])
                if ep[0] == "plan":
                    self.assertIn("plan", res)
                    self.assertNotIn("diagnostics", res)
                    self.assertTrue(res["plan"]["topological_order"])

    def test_cli_validate_io_exact_diagnostic_pointer(self):
        # validate-io 返回带精确 JSON 指针的结构化诊断。
        conf_path = REPO / "demo/fixtures/mock/pipeline_entity_extract.conf"
        conf = json.loads(conf_path.read_text())
        pipe_file = conf_path.with_name(conf["pipe_path"])
        pipe_doc = json.loads(pipe_file.read_text())

        with tempfile.TemporaryDirectory(prefix="validate-io-", dir=REPO / "build") as directory:
            changed_conf = Path(directory) / "pipeline.conf"
            changed_pipe = Path(directory) / conf["pipe_path"]
            # 显式的错误分配保留其精确的原生指针。
            output = next(entry for entry in pipe_doc["io"]["output"]
                          if entry["type"] == "entity_out")
            output.setdefault("params", {})["entities_json_max_bytes"] = 0
            changed_pipe.write_text(json.dumps(pipe_doc))
            changed_conf.write_text(json.dumps(conf))

            code, res = self.command("validate-io", str(changed_conf))
            self.assertEqual(code, 1)
            self.assertFalse(res["ok"])
            self.assertEqual(res["diagnostics"][0]["code"], "IO_VALIDATION_ERROR")
            self.assertEqual(
                res["diagnostics"][0]["path"],
                "/io/output/0/params/entities_json_max_bytes",
            )

    def test_cli_unknown_converter_unknown_root_and_malformed_pair(self):
        pipeline = json.loads((REPO / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        unknown = copy.deepcopy(pipeline)
        unknown["io"]["input"][0]["name"] = "nonexistent_converter"
        root_field = copy.deepcopy(pipeline)
        root_field["unknown_root"] = True
        malformed = copy.deepcopy(pipeline)
        malformed["io"]["input"][0]["name"] = 12345
        for case, document, expected_code, path in (
                ("unknown", unknown, "UNKNOWN_CONVERTER", "/io/input/0"),
                ("root_field", root_field, "DEPLOYMENT_ERROR", "/unknown_root"),
                ("malformed", malformed, "DEPLOYMENT_ERROR", "/io/input/0/name")):
            for endpoint in self.CLI_PARITY_ENTRYPOINTS:
                with self.subTest(case=case, endpoint=endpoint):
                    code, result = self.command(*endpoint, "--stdin", input_pipeline=document)
                    self.assertEqual(code, 1)
                    self.assertFalse(result["ok"])
                    self.assertEqual(result["diagnostics"][0]["code"], expected_code)
                    self.assertEqual(result["diagnostics"][0]["path"], path)
                    if endpoint[0] == "plan":
                        self.assertEqual(result["plan"], {"layers": [], "topological_order": []})

    def test_cli_output_parameters_have_exact_errors(self):
        pipeline = json.loads((REPO / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        for params, expected_code, path in (
            ({"unknown_field": 1}, "UNKNOWN_CONFIG_FIELD", "/io/output/0/params/unknown_field"),
            ([], "DEPLOYMENT_ERROR", "/io/output/0/params"),
            ({"entities_json_max_bytes": 0}, "CONFIG_FIELD_RANGE", "/io/output/0/params/entities_json_max_bytes"),
            ({"entities_json_max_bytes": 2.5}, "CONFIG_FIELD_TYPE", "/io/output/0/params/entities_json_max_bytes"),
        ):
            document = copy.deepcopy(pipeline)
            document["io"]["output"][0]["params"] = params
            for endpoint in self.CLI_PARITY_ENTRYPOINTS:
                with self.subTest(params=params, endpoint=endpoint):
                    code, result = self.command(*endpoint, "--stdin", input_pipeline=document)
                    self.assertEqual(code, 1)
                    self.assertFalse(result["ok"])
                    self.assertEqual(result["diagnostics"][0]["code"], expected_code)
                    self.assertEqual(result["diagnostics"][0]["path"], path)
                    if expected_code == "UNKNOWN_CONFIG_FIELD":
                        self.assertIn("unknown_field", result["diagnostics"][0]["message"])
                    if endpoint[0] == "plan":
                        self.assertEqual(result["plan"], {"layers": [], "topological_order": []})

    def test_cli_multiple_core_errors_with_deployment(self):
        # 部署合法，但 Core 有多个 Node 错误。
        # 所有 CLI 入口的响应数组都必须保留全部诊断。
        pipeline = json.loads(
            (REPO / "demo/fixtures/mock/pipeline_entity_extract.json").read_text()
        )
        # 向 Pipeline 添加两个非法 Node
        doc = copy.deepcopy(pipeline)
        doc["pipeline"].append({'name': 'bad_node_1', 'type': 'completely_unknown_node_type_one', 'depends_on': []})
        doc["pipeline"].append({'name': 'bad_node_2', 'type': 'completely_unknown_node_type_two', 'depends_on': []})

        for ep in self.CLI_PARITY_ENTRYPOINTS:
            with self.subTest(entrypoint=ep):
                code, res = self.command(*ep, "--stdin", input_pipeline=doc)
                self.assertEqual(code, 1)
                self.assertFalse(res["ok"])
                self.assertIn("diagnostics", res)
                # 必须保留多条诊断，不能合并
                self.assertGreaterEqual(len(res["diagnostics"]), 2)
                diag_codes = [d["code"] for d in res["diagnostics"]]
                self.assertIn("UNKNOWN_NODE_TYPE", diag_codes)
                if ep[0] == "plan":
                    self.assertIn("plan", res)

    def test_cli_edit_invalid_deployment_validation_policy(self):
        # 分别以 require_valid=false 和 require_valid=true 编辑非法部署。
        pipeline = json.loads(
            (REPO / "demo/fixtures/mock/pipeline_entity_extract.json").read_text()
        )
        invalid_doc = copy.deepcopy(pipeline)
        invalid_doc["io"]["input"][0]["name"] = "invalid_converter"

        op = {'kind': 'add_node', 'type': 'text_template', 'name': 'draft_node', 'params': {'template': '{{primary}}'}}

        # 情形 1：require_valid=false -> 返回修改后的草稿，validation.ok=false
        req_false = {
            "pipeline": invalid_doc,
            "operation": op,
            "require_valid": False,
        }
        code_false, res_false = self.command("edit", "--stdin", input_pipeline=req_false)
        self.assertEqual(code_false, 0)
        self.assertTrue(res_false["ok"])
        self.assertIn("pipeline", res_false)
        draft_node_names = [n["name"] for n in res_false["pipeline"]["pipeline"]]
        self.assertIn("draft_node", draft_node_names)
        self.assertIn("validation", res_false)
        self.assertFalse(res_false["validation"]["ok"])
        self.assertEqual(res_false["validation"]["diagnostics"][0]["code"], "UNKNOWN_CONVERTER")

        # 情形 2：require_valid=true -> 拒绝，且不返回修改后的 Pipeline
        req_true = {
            "pipeline": invalid_doc,
            "operation": op,
            "require_valid": True,
        }
        code_true, res_true = self.command("edit", "--stdin", input_pipeline=req_true)
        self.assertEqual(code_true, 1)
        self.assertFalse(res_true["ok"])
        self.assertNotIn("pipeline", res_true)
        self.assertIn("validation", res_true)
        self.assertFalse(res_true["validation"]["ok"])
        self.assertEqual(res_true["validation"]["diagnostics"][0]["code"], "UNKNOWN_CONVERTER")


    def test_cli_file_failures_return_json_diagnostics(self):
        # 文件缺失时，各 CLI 入口产出符合 schema 的 JSON 诊断。
        # 1. 对不存在的配置文件执行 validate-io
        code, res = self.command("validate-io", "nonexistent_config_file.conf")
        self.assertEqual(code, 1)
        self.assertFalse(res["ok"])
        self.assertEqual(res["diagnostics"][0]["code"], "IO_VALIDATION_ERROR")
        self.assertIn("nonexistent_config_file.conf", res["diagnostics"][0]["message"])

        # 2. 对不存在的文件执行 resolve-conf
        code, res = self.command("resolve-conf", "nonexistent_config_file.conf", "--root", str(REPO))
        self.assertEqual(code, 1)
        self.assertFalse(res["ok"])
        self.assertEqual(res["diagnostics"][0]["code"], "DEPLOYMENT_CONFIG")
        self.assertEqual(res["diagnostics"][0]["path"], "/")

        # 3. 对不存在的文件执行 validate
        code, res = self.command("validate", "nonexistent_pipeline.json")
        self.assertEqual(code, 1)
        self.assertFalse(res["ok"])
        self.assertEqual(res["diagnostics"][0]["code"], "JSON_READ")

    def test_native_viewer_shows_data_mappings_and_extra_order(self):
        pipeline = json.loads((REPO / "configs/pipeline_doc_qa_default.json").read_text())
        pipeline["pipeline"][1]["depends_on"] = ["chunk_docs"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pipeline_view.json"
            path.write_text(json.dumps(pipeline))
            process = subprocess.run([str(VIEWER), str(path)], text=True, capture_output=True,
                                     cwd=REPO, check=False)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertIn("Node embed_query: text_embedding", process.stdout)
        self.assertIn("input.doc_text -> chunk_docs.text", process.stdout)
        self.assertIn("chunk_docs.chunks -> embed_chunks.text", process.stdout)
        self.assertIn("order: chunk_docs -> embed_query", process.stdout)
        self.assertIn("generate_answer.text -> output.answer_text", process.stdout)
        self.assertNotIn("order: chunk_docs -> embed_chunks", process.stdout)

    def test_native_viewer_shows_declared_backend_batch_fields(self):
        pipeline = json.loads((REPO / "configs/pipeline_doc_qa_cpu.json").read_text())
        pipeline["models"][0]["backend"]["params"] = {"max_batch_size": 4}
        pipeline["models"][1]["backend"].setdefault("params", {})["decode_batch_size"] = 512
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pipeline_batches.json"
            path.write_text(json.dumps(pipeline))
            process = subprocess.run([str(VIEWER), str(path)], text=True, capture_output=True,
                                     cwd=REPO, check=False)
        self.assertEqual(process.returncode, 0, process.stderr)
        embedding, llm = process.stdout.split("Model llm_model:", 1)
        self.assertIn("backend.params.max_batch_size=4", embedding)
        self.assertIn("backend.params.decode_batch_size=512", llm)
        self.assertNotIn("max_batch_size", llm)
        self.assertNotIn("FixedMaxBatch", process.stdout)

    def test_native_viewer_does_not_invent_backend_batch_values(self):
        pipeline = json.loads((REPO / "configs/pipeline_doc_qa_cpu.json").read_text())
        for model in pipeline["models"]:
            model["backend"].pop("params", None)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pipeline_undeclared_batch.json"
            path.write_text(json.dumps(pipeline))
            process = subprocess.run([str(VIEWER), str(path)], text=True, capture_output=True,
                                     cwd=REPO, check=False)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertNotIn("batch_size", process.stdout)
        self.assertNotIn("FixedMaxBatch", process.stdout)


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
