#!/usr/bin/env python3
"""API and filesystem boundary tests for the local Pipeline Studio."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import unittest
from unittest import mock
import urllib.request


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TOOL = ROOT / "build" / "alg_pipeline_tool_test"
if not DEFAULT_TOOL.exists():
    DEFAULT_TOOL = ROOT / "build" / "alg_pipeline_tool"
PIPELINE_TOOL = Path(
    os.environ.get("LLM_EDGEFLOW_PIPELINE_TOOL", DEFAULT_TOOL)
)
ALG_SHOW = Path(
    os.environ.get("LLM_EDGEFLOW_ALG_SHOW", ROOT / "build" / "alg_show")
)
STUDIO_SERVER = ROOT / "tools" / "pipeline_studio" / "server.py"
WEB_ROOT = ROOT / "tools" / "pipeline_studio" / "web"
SPEC = importlib.util.spec_from_file_location("edgeflow_show", STUDIO_SERVER)
SHOW = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SHOW)
SHOW.PIPELINE_TOOL = PIPELINE_TOOL


class WorkbenchServiceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.configs = Path(self.temporary.name)
        self.service = SHOW.WorkbenchService(self.configs)
        self.keyword = json.loads(
            (ROOT / "configs" / "pipeline_keyword_match.json").read_text()
        )

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
        invalid = {"biz_name": "keyword_match_v1", "pipeline": []}
        with self.assertRaises(SHOW.StudioError) as validation:
            self.service.save_pipeline("pipeline_invalid.json", invalid, None, True)
        self.assertEqual(validation.exception.code, "VALIDATION_FAILED")
        self.assertFalse((self.configs / "pipeline_invalid.json").exists())

    def test_profile_mismatch_and_real_demo_roundtrip(self):
        with self.assertRaises(SHOW.StudioError) as mismatch:
            self.service.start_run(self.keyword, "entity_extract_mock")
        self.assertEqual(mismatch.exception.code, "PROFILE_MISMATCH")
        self.keyword["pipeline"][0]["config"]["categories"] = {
            "STUDIO_DRAFT": ["VIP"]
        }
        started = self.service.start_run(self.keyword, "keyword_match_mock")
        for _ in range(200):
            job = self.service.run_status(started["job_id"])["job"]
            if job["status"] in ("completed", "failed", "cancelled"):
                break
            time.sleep(0.05)
        self.assertEqual(job["status"], "completed", job)
        self.assertIn("summary.json", job["result"])
        self.assertIn("results.jsonl", job["result"])
        results = job["result"]["results.jsonl"]
        self.assertEqual(len(results), 2)
        self.assertEqual([result["status"] for result in results], [0, 0])
        self.assertEqual(len({result["request_id"] for result in results}), 2)
        self.assertTrue(results[0]["output"]["is_hit"])
        self.assertEqual(results[0]["output"]["match_result"]["intent"], "STUDIO_DRAFT")
        self.assertEqual(results[0]["output"]["match_result"]["matches"], [
            {"category": "STUDIO_DRAFT", "matched_word": "VIP"}
        ])
        self.assertFalse(results[1]["output"]["is_hit"])

    def test_startup_opens_each_kite_file_without_backend_validation(self):
        files = list((ROOT / "configs" / "kite").glob("pipeline_*.json"))
        self.assertTrue(files)
        for path in files:
            with self.subTest(path=path):
                service = SHOW.WorkbenchService(initial=path)
                self.assertEqual(service.initial_document["pipeline"], json.loads(path.read_text()))
                self.assertTrue(service.initial_document["imported"])
                self.assertEqual(service.initial_document["revision"], "")

    def test_startup_external_file_is_a_snapshot_without_write_binding(self):
        path = self.configs / "arbitrary name.json"
        path.write_text(json.dumps(self.keyword))
        service = SHOW.WorkbenchService(initial=path)
        path.write_text("{}")
        self.assertEqual(service.initial_document["pipeline"], self.keyword)
        self.assertTrue(service.initial_document["imported"])
        with self.assertRaises(SHOW.StudioError):
            service.managed_path(str(path))

    def test_startup_managed_file_keeps_revision_and_save_contract(self):
        path = self.configs / "pipeline_managed.json"
        path.write_text(json.dumps(self.keyword))
        service = SHOW.WorkbenchService(self.configs, initial=path)
        self.assertEqual(service.initial_document, service.open_pipeline(path.name))
        self.assertNotIn("imported", service.initial_document)
        original_revision = service.initial_document["revision"]
        changed = {**self.keyword, "comment": "edited after launch"}
        saved = service.save_pipeline(path.name, changed, original_revision)
        self.assertEqual(service.initial_pipeline()["document"], saved)
        self.assertNotEqual(service.initial_pipeline()["document"]["revision"], original_revision)

    def test_file_reader_rejects_directories_invalid_json_and_oversized_files(self):
        with self.assertRaises(SHOW.StudioError):
            SHOW.read_pipeline_file(self.configs)
        path = self.configs / "invalid.json"
        for content in ("{", "[]", '{"pipeline":{}}', " " * (SHOW.MAX_DOCUMENT_BYTES + 1)):
            path.write_text(content)
            with self.subTest(content=content[:30]), self.assertRaises(SHOW.StudioError):
                SHOW.read_pipeline_file(path)

    def test_cli_defaults_to_terminal_and_requires_web_flag_for_studio(self):
        path = str(ROOT / "configs" / "kite" / "pipeline_doc_qa.json")
        for args, selected, port in ((["--web"], None, 8080),
                                     ([path, "--web", "--port", "0"], Path(path), 0)):
            with self.subTest(args=args), mock.patch.object(SHOW, "launch_web") as launch, mock.patch.object(SHOW, "render_terminal") as render:
                SHOW.main(args)
                launch.assert_called_once_with(selected, port)
                render.assert_not_called()
        with mock.patch.object(SHOW, "launch_web") as launch, mock.patch.object(SHOW, "render_terminal") as render:
            SHOW.main([path])
            launch.assert_not_called()
            render.assert_called_once_with(Path(path), json.loads(Path(path).read_text()))
        with mock.patch.object(SHOW, "launch_web") as launch, mock.patch.object(SHOW, "render_terminal") as render, mock.patch.object(SHOW.argparse.ArgumentParser, "print_help") as help_text:
            SHOW.main([])
            launch.assert_not_called()
            render.assert_not_called()
            help_text.assert_called_once()


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
        expected_schema = 2 if args[0] in ("catalog", "describe-node") else 1
        self.assertEqual(payload["schema_version"], expected_schema)
        return process.returncode, payload

    def test_all_commands_return_versioned_json(self):
        first_code, first = self.command("catalog", "--biz", "keyword_match_v1")
        second_code, second = self.command("catalog", "--biz", "keyword_match_v1")
        self.assertEqual((first_code, first), (second_code, second))
        self.assertTrue(first["nodes"])
        code, described = self.command("describe-node", "TextRuleMatchNode")
        self.assertEqual(code, 0)
        self.assertEqual(described["node_type"], "TextRuleMatchNode")
        code, initialized = self.command(
            "init", "--biz", "keyword_match_v1", "--empty"
        )
        self.assertEqual(code, 0)
        self.assertEqual(initialized["pipeline"]["pipeline"], [])
        self.assertEqual(initialized["pipeline"]["biz_name"], "keyword_match_v1")
        self.assertNotIn("business_name", initialized["pipeline"])
        rejected = subprocess.run(
            [str(PIPELINE_TOOL), "catalog", "--business", "keyword_match_v1"],
            text=True,
            capture_output=True,
            cwd=ROOT,
            check=False,
        )
        self.assertEqual(rejected.returncode, 2)
        removed_normalizer = subprocess.run(
            [str(PIPELINE_TOOL), "normalize", "--explicit-dag", "--stdin"],
            input="{}",
            text=True,
            capture_output=True,
            cwd=ROOT,
            check=False,
        )
        self.assertEqual(removed_normalizer.returncode, 2)
        pipeline = json.loads(
            (ROOT / "configs" / "pipeline_keyword_match.json").read_text()
        )
        code, validated = self.command(
            "validate", "--stdin", input_pipeline=pipeline
        )
        self.assertEqual(code, 0)
        self.assertTrue(validated["ok"])
        code, plan = self.command(
            "plan", "--stdin", input_pipeline=pipeline
        )
        self.assertEqual(code, 0)
        self.assertNotIn("diagnostics", plan)
        self.assertTrue(plan["plan"]["topological_order"])

    def test_native_viewer_preserves_explicit_dag_dependencies(self):
        process = subprocess.run(
            [str(ALG_SHOW), str(ROOT / "configs" / "pipeline_doc_qa.json")],
            text=True,
            capture_output=True,
            cwd=ROOT,
            check=False,
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertIn("node_1_QueryEmbeddingNode", process.stdout)
        self.assertIn("depends_on: []", process.stdout)
        self.assertIn(
            'depends_on: ["node_0_TextChunkNode"]', process.stdout
        )
        self.assertIn("alg_pipeline_tool validate/plan", process.stdout)

    def test_native_viewer_shows_declared_backend_batch_fields(self):
        process = subprocess.run(
            [str(ALG_SHOW), str(ROOT / "configs" / "pipeline_doc_qa_onnx.json")],
            text=True,
            capture_output=True,
            cwd=ROOT,
            check=False,
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        model_lines = process.stdout.splitlines()
        embedding = next(line for line in model_lines if "embed_model_onnx" in line)
        llm = next(line for line in model_lines if "llm_model_llamacpp" in line)
        self.assertIn("backend_config.max_batch_size: 4", embedding)
        self.assertIn("backend_config.decode_batch_size: 512", llm)
        self.assertNotIn("max_batch_size", llm)
        self.assertNotIn("FixedMaxBatch", process.stdout)

    def test_native_viewer_does_not_invent_backend_batch_values(self):
        pipeline = json.loads(
            (ROOT / "configs" / "pipeline_doc_qa_onnx.json").read_text()
        )
        for model in pipeline["models"]:
            model.pop("backend_config", None)
            model["model_config"]["max_batch_size"] = 7
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "pipeline_undeclared_batch.json"
            config.write_text(json.dumps(pipeline))
            process = subprocess.run(
                [str(ALG_SHOW), str(config)],
                text=True,
                capture_output=True,
                cwd=ROOT,
                check=False,
            )
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertNotIn("batch_size", process.stdout)
        self.assertNotIn("FixedMaxBatch", process.stdout)


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
        self.assertIn('id="arrow-hover"', index)
        with urllib.request.urlopen(origin + "/styles.css", timeout=5) as response:
            css = response.read().decode()
        self.assertIn(".has-model", css)
        self.assertIn(".has-error", css)
        with urllib.request.urlopen(origin + "/app.js", timeout=5) as response:
            app_js = response.read().decode()
        self.assertIn("new GraphView", app_js)
        self.assertIn('from "./workbench.js"', app_js)
        self.assertNotIn("ensureExplicit", app_js)
        with self.post() as response:
            payload = json.load(response)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["schema_version"], 1)

    def test_initial_endpoint_only_exposes_explicit_startup_document(self):
        with urllib.request.urlopen(self.base + "/initial?filename=/etc/passwd", timeout=5) as response:
            self.assertIsNone(json.load(response)["document"])
        expected = {"filename": "chosen.json", "pipeline": {"pipeline": []}, "imported": True}
        self.service.initial_document = expected
        with urllib.request.urlopen(self.base + "/initial?filename=/etc/passwd", timeout=5) as response:
            self.assertEqual(json.load(response)["document"], expected)

    @unittest.skipUnless(shutil.which("node"), "Node.js is required for Web module tests")
    def test_web_modules_apply_catalog_semantics_and_topological_layout(self):
        script = f"""
import assert from "node:assert/strict";
import {{ readFileSync }} from "node:fs";

const loadModule = async path => {{
  const source = readFileSync(path, "utf8");
  return import(`data:text/javascript;base64,${{Buffer.from(source).toString("base64")}}`);
}};
const workbench = await loadModule({json.dumps(str(WEB_ROOT / "workbench.js"))});
const graph = await loadModule({json.dumps(str(WEB_ROOT / "graph.js"))});

const models = [
  {{ model_id: "embed", model_type: "embed_type", capability: "embedding" }},
  {{ model_id: "llm", model_type: "llm_type", capability: "llm" }},
  {{ model_id: "legacy_embed", model_type: "legacy_embed_type" }},
];
const modelDefinitions = [
  {{ model_type: "legacy_embed_type", capability: "embedding" }},
];
const nodeDefinition = {{ model_capability: "embedding", model_config_field: "model_slot" }};
assert.deepEqual(
  workbench.compatibleModels(models, modelDefinitions, nodeDefinition).map(model => model.model_id),
  ["embed", "legacy_embed"]
);
assert.deepEqual(
  [...workbench.modelBoundNodeIds([
    {{ id: "bound", node_type: "CustomNode", config: {{ model_slot: "embed" }} }},
    {{ id: "hardcoded", node_type: "CustomNode", config: {{ bind_model: "llm" }} }},
  ], [{{ node_type: "CustomNode", model_config_field: "model_slot" }}])],
  ["bound"]
);

const positions = graph.layeredPositions([
  {{ id: "sink", depends_on: ["middle"] }},
  {{ id: "root", depends_on: [] }},
  {{ id: "middle", depends_on: ["root"] }},
  {{ id: "parallel_root", depends_on: [] }},
]);
assert.equal(positions.root.x, 65);
assert.ok(positions.middle.x > positions.root.x);
assert.ok(positions.sink.x > positions.middle.x);
assert.equal(positions.parallel_root.x, 65);
assert.notEqual(positions.root.y, positions.parallel_root.y);

let savedPositions;
const view = {{
  positions: {{ root: {{ x: 1, y: 2 }}, stale: {{ x: 3, y: 4 }} }},
  callbacks: {{ positionsChanged: positions => {{ savedPositions = positions; }} }},
}};
graph.GraphView.prototype.layout.call(view, [{{ id: "root", depends_on: [] }}]);
assert.deepEqual(savedPositions, {{ root: {{ x: 1, y: 2 }} }});

const deferred = () => {{
  let resolve;
  let reject;
  const promise = new Promise((resolvePromise, rejectPromise) => {{
    resolve = resolvePromise;
    reject = rejectPromise;
  }});
  return {{ promise, resolve, reject }};
}};
const gate = workbench.createLatestRequestGate();
const first = deferred();
const second = deferred();
let activeCatalog = "initial";
const firstRun = gate.run(() => first.promise, value => {{ activeCatalog = value; }});
const secondRun = gate.run(() => second.promise, value => {{ activeCatalog = value; }});
second.resolve("newer");
assert.equal(await secondRun, true);
first.resolve("older");
assert.equal(await firstRun, false);
assert.equal(activeCatalog, "newer");

const staleFailure = deferred();
const currentSuccess = deferred();
const staleFailureRun = gate.run(
  () => staleFailure.promise,
  value => {{ activeCatalog = value; }}
);
const currentSuccessRun = gate.run(
  () => currentSuccess.promise,
  value => {{ activeCatalog = value; }}
);
currentSuccess.resolve("current");
assert.equal(await currentSuccessRun, true);
staleFailure.reject(new Error("stale failure"));
assert.equal(await staleFailureRun, false);
assert.equal(activeCatalog, "current");

await assert.rejects(
  gate.run(() => Promise.reject(new Error("current failure")), () => {{}}),
  /current failure/
);
"""
        process = subprocess.run(
            [shutil.which("node"), "--input-type=module", "-e", script],
            text=True,
            capture_output=True,
            cwd=ROOT,
            check=False,
        )
        self.assertEqual(process.returncode, 0, process.stderr)

    @unittest.skipUnless(shutil.which("node"), "Node.js is required for Web module tests")
    def test_graph_navigation_routes_and_editor_history(self):
        for filename in ("studio_graph_test.mjs", "studio_editor_test.mjs"):
            with self.subTest(module=filename):
                process = subprocess.run(
                    [shutil.which("node"), str(Path(__file__).with_name(filename))],
                    text=True, capture_output=True, cwd=ROOT, check=False,
                )
                self.assertEqual(process.returncode, 0, process.stdout + process.stderr)

    @unittest.skipUnless(shutil.which("node"), "Node.js is required for Web module tests")
    def test_port_editing_roundtrips_to_native_validator_and_model_forms(self):
        catalog = json.loads(subprocess.check_output([str(PIPELINE_TOOL), "catalog"], text=True))
        script = """
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const code = readFileSync(process.argv[1], 'utf8');
const w = await import(`data:text/javascript;base64,${Buffer.from(code).toString('base64')}`);
const catalog = JSON.parse(readFileSync(0, 'utf8'));
const pipeline = {biz_name:'keyword_match_v1', models:[], pipeline:[
  {id:'template', node_type:'TextTemplateNode', depends_on:[], config:{template:'{{primary}}'}, ports:{outputs:{text:'template_text'}}},
  {id:'rule', node_type:'TextRuleMatchNode', depends_on:[], config:{}, ports:{outputs:{matches:'rule_result'}}}
]};
w.connectPorts(pipeline,catalog,w.INGRESS,'input_sentences','template','primary');
w.connectPorts(pipeline,catalog,'template','text','rule','text');
w.connectPorts(pipeline,catalog,'rule','matches',w.EGRESS,'rule_matches');
assert.equal(pipeline.pipeline[1].ports.inputs.text,'template_text');
assert.deepEqual(pipeline.pipeline[1].depends_on,['template']);
assert.throws(()=>w.connectPorts(pipeline,catalog,'rule','matches','template','primary'), /类型/);
assert.throws(()=>w.connectPorts(pipeline,catalog,'template','text','template','primary'), /环/);
const detached = structuredClone(pipeline);
w.disconnectPorts(detached,catalog,w.graphDocument(detached,catalog).edges.find(e=>e.target==='rule' && e.targetPort==='text'));
assert.ok(!w.graphDocument(detached,catalog).edges.some(e=>e.source==='template' && e.target==='rule'));
w.removeNode(detached,catalog,'template');
assert.deepEqual(detached.pipeline[0].depends_on,[]);
const modelDef = catalog.models.find(m=>m.model_type==='bge_embedding');
const backend = w.compatibleBackends(catalog.backends,modelDef).find(b=>b.backend_type==='onnxruntime');
if (backend) {
assert.ok(!w.compatibleBackends(catalog.backends,modelDef).some(b=>b.backend_type==='llama_cpp'));
const models = {models:[], pipeline:[]};
w.upsertModel(models,catalog,'',{model_id:'embed',model_type:modelDef.model_type,backend:backend.backend_type,model_path:'embed.onnx',model_config:w.schemaDefaults(modelDef.config_fields),backend_config:w.schemaDefaults(backend.config_fields)});
models.pipeline.push({id:'embed_node',node_type:'TextEmbeddingNode',config:{bind_model:'embed'}});
w.upsertModel(models,catalog,'embed',{...models.models[0],model_id:'renamed'});
assert.equal(models.pipeline[0].config.bind_model,'renamed');
assert.throws(()=>w.removeModel(models,catalog,'renamed'), /使用/);
assert.throws(()=>w.upsertModel(models,catalog,'',{...models.models[0],backend:'llama_cpp'}), /不兼容/);
}
process.stdout.write(JSON.stringify(pipeline));
"""
        process = subprocess.run([shutil.which("node"), "--input-type=module", "-e", script, str(WEB_ROOT / "workbench.js")], input=json.dumps(catalog), text=True, capture_output=True)
        self.assertEqual(process.returncode, 0, process.stderr)
        generated = json.loads(process.stdout)
        with tempfile.TemporaryDirectory() as directory:
            filename = Path(directory) / "pipeline_ports.json"
            filename.write_text(json.dumps(generated))
            result = subprocess.run([str(PIPELINE_TOOL), "validate", str(filename)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_invalid_fixtures_table_driven_parity_matrix(self):
        fixture_path = (
            ROOT
            / "tests"
            / "fixtures"
            / "pipelines"
            / "validation"
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


class SelectionVerificationTest(unittest.TestCase):
    def test_asset_hashes_include_sidecars_and_changed_paths_are_unregistered(self):
        selection = SHOW.SELECTION
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "weights.bin").write_bytes(b"test weights")
            (root / "vocab.txt").write_bytes(b"tokenizer")
            model = {"model_id": "m", "model_type": "bge_embedding", "backend": "onnxruntime", "model_path": "weights.bin", "model_config": {"tokenizer_file": "vocab.txt"}}
            manifest = {"schema_version": 1, "selections": [{"id": "test", "model": json.loads(json.dumps(model)), "paths": {"/model_path": "weights.bin", "/model_config/tokenizer_file": "vocab.txt"}, "files": ["weights.bin", "vocab.txt"]}], "artifacts": {name: {"sha256": selection.file_digest(root / name)} for name in ("weights.bin", "vocab.txt")}}
            pipeline = {"models": [model]}
            incomplete = json.loads(json.dumps(manifest))
            incomplete["selections"][0]["files"] = []
            with self.assertRaises(ValueError):
                selection.verify_assets(pipeline, root, incomplete)
            self.assertEqual(selection.verify_assets(pipeline, root, manifest)[0]["status"], "verified")
            (root / "vocab.txt").write_bytes(b"corrupt")
            self.assertEqual(selection.verify_assets(pipeline, root, manifest)[0]["files"][1]["status"], "hash_mismatch")
            (root / "vocab.txt").unlink()
            self.assertEqual(selection.verify_assets(pipeline, root, manifest)[0]["files"][1]["status"], "missing")
            model["model_config"]["tokenizer_file"] = "another_vocab.txt"
            self.assertEqual(selection.verify_assets(pipeline, root, manifest)[0]["status"], "unregistered")
            with self.assertRaises(ValueError):
                selection.within(root, "../outside")

    def test_native_build_variant_and_real_effects_are_bound_to_selection(self):
        selection = SHOW.SELECTION
        tool = Path(os.environ.get("LLM_EDGEFLOW_SELECTION_TOOL", ROOT / "build/alg_pipeline_tool"))
        demo = SHOW.DEMO_BINARY
        pipeline = json.loads((ROOT / "configs/pipeline_keyword_match.json").read_text())
        spec = ROOT / "tests/fixtures/effects/keyword_exact.json"
        conf = ROOT / "configs/pipeline_keyword_match.conf"
        report = selection.inspect_selection(pipeline, tool, ROOT / "models")
        self.assertTrue(report["ok"])
        self.assertFalse(report["ready_for_biz"])
        self.assertEqual(report["schema_version"], 2)
        self.assertNotIn("ready_for_business", report)
        # Current canonical build has enabled backends; claiming the empty
        # minimal variant must fail independently of this model-free Pipeline.
        mismatched = selection.inspect_selection(pipeline, tool, ROOT / "models", variant="minimal" if report["build"]["enabled_backends"] else "default-cpu")
        self.assertFalse(mismatched["ok"])
        receipt = selection.evaluate(pipeline, report, tool, ROOT / "models", spec, conf, demo)
        self.assertEqual(receipt["schema_version"], 2)
        self.assertEqual(receipt["metrics"]["pass_rate"], 1.0)
        self.assertEqual(receipt["metrics"]["total"], 4)
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "effects.json"
            evidence.write_text(json.dumps(receipt))
            checked = selection.attach_evidence(report, evidence, spec, conf, demo)
            self.assertTrue(checked["ready_for_biz"])
            pipeline["pipeline"][0]["config"]["categories"] = {"OTHER": ["different"]}
            changed = selection.inspect_selection(pipeline, tool, ROOT / "models")
            self.assertEqual(selection.attach_evidence(changed, evidence, spec, conf, demo)["effects"]["status"], "stale")
        broken_records = receipt["records"][:-1]
        self.assertEqual(selection.compare_samples(broken_records, selection.read_json(spec))["status"], "failed")
        records = json.loads(json.dumps(receipt["records"]))
        records[0]["output"]["is_hit"] = False
        self.assertEqual(selection.compare_samples(records, selection.read_json(spec))["status"], "failed")

if __name__ == "__main__":
    unittest.main()
