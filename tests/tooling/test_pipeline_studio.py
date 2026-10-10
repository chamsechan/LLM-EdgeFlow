#!/usr/bin/env python3
"""本地 Pipeline Studio 的 API 与文件系统边界测试。"""

import copy
import importlib.util
import io
import json
import os
import sys
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile
import threading
import time
import unittest
from unittest import mock
import urllib.request
import uuid


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TOOL = ROOT / "build" / "alg_pipeline_tool_test"
if not DEFAULT_TOOL.exists():
    DEFAULT_TOOL = ROOT / "build" / "alg_pipeline_tool"
PIPELINE_TOOL = Path(
    os.environ.get("LLM_EDGEFLOW_PIPELINE_TOOL", DEFAULT_TOOL)
)
STUDIO_SERVER = ROOT / "tools" / "pipeline_studio" / "server.py"
WEB_ROOT = ROOT / "tools" / "pipeline_studio" / "web"
SPEC = importlib.util.spec_from_file_location("edgeflow_show", STUDIO_SERVER)
SHOW = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SHOW)
SHOW.PIPELINE_TOOL = PIPELINE_TOOL


def output_params(pipeline, type_name, name=None):
    matches = [entry for entry in pipeline["io"]["output"]
               if entry["type"] == type_name and (name is None or entry["name"] == name)]
    if len(matches) != 1:
        raise ValueError("Select one output converter for the test fixture")
    return matches[0].setdefault("params", {})


class WorkbenchServiceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.configs = Path(self.temporary.name)
        self.service = SHOW.WorkbenchService(self.configs)
        self.keyword = json.loads(
            (ROOT / "configs" / "pipeline_keyword_match_rules.json").read_text()
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
        invalid = {'io': {'input': [{'type': 'keyword_in', 'name': 'keyword_match'}], 'output': [{'type': 'keyword_out', 'name': 'keyword_match', 'inputs': {}}]}, 'pipeline': []}
        with self.assertRaises(SHOW.StudioError) as validation:
            self.service.save_pipeline("pipeline_invalid.json", invalid, None, True)
        self.assertEqual(validation.exception.code, "VALIDATION_FAILED")
        self.assertFalse((self.configs / "pipeline_invalid.json").exists())

    def test_profile_mismatch_and_real_demo_roundtrip(self):
        with self.assertRaises(SHOW.StudioError) as mismatch:
            self.service.start_run(self.keyword, "entity_extract_mock")
        self.assertEqual(mismatch.exception.code, "PROFILE_MISMATCH")
        self.keyword["pipeline"][0]["params"]["categories"] = {
            "STUDIO_DRAFT": ["VIP"]
        }
        started = self.service.start_run(self.keyword, "keyword_match_rules")
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
        files = list((ROOT / "configs").glob("pipeline_*_kite*.json"))
        self.assertTrue(files)
        for path in files:
            with self.subTest(path=path):
                service = SHOW.WorkbenchService(initial=path)
                self.assertEqual(service.initial_document["pipeline"], json.loads(path.read_text()))
                self.assertNotIn("imported", service.initial_document)
                self.assertEqual(service.initial_document, service.open_pipeline(path.name))
                self.assertIn(path.name, [item["filename"] for item in service.pipelines()["pipelines"]])

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

    def test_terminal_viewer_prints_declared_mappings_without_inferring_order(self):
        pipeline = {"io": {"input": [{"type": "keyword_in", "name": "keyword_match"}],
                           "output": [{"type": "keyword_out", "name": "keyword_match", "inputs": {"matches": "consumer.matches"}}]},
                    "pipeline": [{"name": "consumer", "type": "text_rule_match", "inputs": {"text": "producer.text"}},
                                 {"name": "producer", "type": "text_template", "inputs": {"primary": "input.sentence_text"}}]}
        with mock.patch("builtins.print") as output:
            SHOW.render_terminal(Path("pipeline.json"), pipeline)
        rendered = "\n".join(call.args[0] for call in output.call_args_list if call.args)
        self.assertIn('inputs: {"text": "producer.text"}', rendered)
        self.assertIn('output keyword_out/keyword_match: {"matches": "consumer.matches"}', rendered)
        self.assertIn('depends_on: [] (额外顺序)', rendered)
        self.assertLess(rendered.index("consumer: text_rule_match"), rendered.index("producer: text_template"))

    def test_file_reader_rejects_directories_invalid_json_and_oversized_files(self):
        with self.assertRaises(SHOW.StudioError):
            SHOW.read_pipeline_file(self.configs)
        path = self.configs / "invalid.json"
        for content in ("{", "[]", '{"pipeline":{}}', " " * (SHOW.MAX_DOCUMENT_BYTES + 1)):
            path.write_text(content)
            with self.subTest(content=content[:30]), self.assertRaises(SHOW.StudioError):
                SHOW.read_pipeline_file(path)

    def test_cli_defaults_to_terminal_and_requires_web_flag_for_studio(self):
        path = str(ROOT / "configs" / "pipeline_doc_qa_kite.json")
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

    def test_validate_passes_explain_flag(self):
        with mock.patch.object(self.service, "invoke_tool", return_value={"ok": True}) as mock_invoke:
            self.service.validate(self.keyword, explain=True)
            mock_invoke.assert_called_once_with(["validate", "--stdin", "--explain"], self.keyword)

        with mock.patch.object(self.service, "invoke_tool", return_value={"ok": True}) as mock_invoke:
            self.service.validate(self.keyword, explain=False)
            mock_invoke.assert_called_once_with(["validate", "--stdin"], self.keyword)

        with mock.patch.object(self.service, "invoke_tool", return_value={"ok": True}) as mock_invoke:
            self.service.validate(self.keyword)
            mock_invoke.assert_called_once_with(["validate", "--stdin"], self.keyword)

    def test_invalid_profile_fields_and_shapes_are_rejected_by_all_consumers(self):
        base = {"config": "unused.conf", "dataset": "unused.txt"}
        cases = [(dict(base, batch_szie=1), "Unknown Profile field", "batch_szie"),
                 ([], "must be an object", None), (None, "must be an object", None)]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "demo").mkdir()
            profile_path = root / "demo/profiles.json"
            for profile, expected, field in cases:
                with self.subTest(profile=profile):
                    profile_path.write_text(json.dumps({"profiles": {"invalid": profile}}))
                    for args in (("catalog",), ("init", "--profile", "invalid")):
                        process = subprocess.run([str(PIPELINE_TOOL), *args], cwd=root, capture_output=True, text=True)
                        self.assertEqual(process.returncode, 1, process.stderr)
                        diagnostic = json.loads(process.stdout)["diagnostics"][0]
                        self.assertEqual(diagnostic["code"], "INVALID_PROFILE")
                        self.assertIn(expected, diagnostic["message"])
                        if field:
                            self.assertIn(field, diagnostic["message"])
                    with mock.patch.object(SHOW, "PROFILE_FILE", profile_path):
                        service = SHOW.WorkbenchService(root)
                        for call in (service.profiles, lambda: service.profile_inputs({}, "invalid")):
                            with self.assertRaises(SHOW.StudioError) as error:
                                call()
                            self.assertEqual(error.exception.code, "INVALID_PROFILE")
                            self.assertIn(expected, str(error.exception))
                            if field:
                                self.assertIn(field, str(error.exception))


class PreviewFixTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.configs = Path(self.temporary.name)
        self.service = SHOW.WorkbenchService(self.configs)
        self.keyword = json.loads(
            (ROOT / "configs" / "pipeline_keyword_match_rules.json").read_text()
        )
        self.patch = [{"op": "add", "path": "/comment", "value": "preview_fix_applied"}]

    def tearDown(self):
        self.temporary.cleanup()

    def _valid_fingerprint(self):
        return self.service.get_tool_fingerprint()

    def test_preview_fix_requires_expected_revision(self):
        valid_fp = self._valid_fingerprint()

        # 省略 expected_revision (None) 时抛出 REVISION_CONFLICT
        with self.assertRaises(SHOW.StudioError) as err_none:
            self.service.preview_fix(
                self.keyword, self.patch, expected_revision=None, tool_fingerprint=valid_fp
            )
        self.assertEqual(err_none.exception.code, "REVISION_CONFLICT")

        # 省略 expected_revision (空字符串) 时抛出 REVISION_CONFLICT
        with self.assertRaises(SHOW.StudioError) as err_empty:
            self.service.preview_fix(
                self.keyword, self.patch, expected_revision="", tool_fingerprint=valid_fp
            )
        self.assertEqual(err_empty.exception.code, "REVISION_CONFLICT")

        # expected_revision 不匹配时抛出 REVISION_CONFLICT
        with self.assertRaises(SHOW.StudioError) as err_mismatch:
            self.service.preview_fix(
                self.keyword, self.patch, expected_revision="mismatched_revision", tool_fingerprint=valid_fp
            )
        self.assertEqual(err_mismatch.exception.code, "REVISION_CONFLICT")

    def test_preview_fix_requires_tool_fingerprint(self):
        raw = json.dumps(self.keyword, sort_keys=True).encode("utf-8")
        valid_rev = SHOW.revision_for(raw)

        # 省略 tool_fingerprint (None) 时抛出 TOOL_OUTDATED
        with self.assertRaises(SHOW.StudioError) as err_none:
            self.service.preview_fix(
                self.keyword, self.patch, expected_revision=valid_rev, tool_fingerprint=None
            )
        self.assertEqual(err_none.exception.code, "TOOL_OUTDATED")

        # 省略 tool_fingerprint (空字符串) 时抛出 TOOL_OUTDATED
        with self.assertRaises(SHOW.StudioError) as err_empty:
            self.service.preview_fix(
                self.keyword, self.patch, expected_revision=valid_rev, tool_fingerprint=""
            )
        self.assertEqual(err_empty.exception.code, "TOOL_OUTDATED")

        # tool_fingerprint 不匹配时抛出 TOOL_OUTDATED
        with self.assertRaises(SHOW.StudioError) as err_mismatch:
            self.service.preview_fix(
                self.keyword, self.patch, expected_revision=valid_rev, tool_fingerprint="mismatched_tool_fingerprint"
            )
        self.assertEqual(err_mismatch.exception.code, "TOOL_OUTDATED")

    def test_preview_fix_succeeds_with_valid_revision_and_tool_fingerprint(self):
        raw = json.dumps(self.keyword, sort_keys=True).encode("utf-8")
        valid_rev = SHOW.revision_for(raw)
        valid_fp = self._valid_fingerprint()

        mock_report = {"ok": True, "diagnostics": []}
        with mock.patch.object(self.service, "validate", return_value=mock_report) as mock_validate:
            result = self.service.preview_fix(
                self.keyword,
                self.patch,
                expected_revision=valid_rev,
                tool_fingerprint=valid_fp,
            )
            self.assertTrue(result["ok"])
            self.assertEqual(result["patched"]["comment"], "preview_fix_applied")
            self.assertEqual(result["report"], mock_report)
            expected_new_rev = SHOW.revision_for(
                json.dumps(result["patched"], sort_keys=True).encode("utf-8")
            )
            self.assertEqual(result["revision"], expected_new_rev)
            self.assertNotEqual(result["revision"], valid_rev)
            mock_validate.assert_called_once_with(result["patched"], explain=True)

    def test_preview_fix_patch_application_failure(self):
        raw = json.dumps(self.keyword, sort_keys=True).encode("utf-8")
        valid_rev = SHOW.revision_for(raw)
        valid_fp = self._valid_fingerprint()

        bad_patch = [{"op": "test", "path": "/io/output/0/name", "value": "mismatched_name"}]
        with self.assertRaises(SHOW.StudioError) as patch_err:
            self.service.preview_fix(
                self.keyword,
                bad_patch,
                expected_revision=valid_rev,
                tool_fingerprint=valid_fp,
            )
        self.assertEqual(patch_err.exception.code, "PATCH_APPLICATION_FAILED")

    def test_output_source_explain_patch_revalidates_the_complete_document(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        pipeline["io"]["output"][0]["inputs"]["entities"] = "parse_entitiez.document"
        snapshot = copy.deepcopy(pipeline)
        report = self.service.validate(pipeline, explain=True)
        self.assertFalse(report["ok"], report)
        diagnostic = next(item for item in report["diagnostics"]
                          if item["path"] == "/io/output/0/inputs/entities")
        self.assertEqual(diagnostic["code"], "UNKNOWN_NODE_REFERENCE")
        fixes = diagnostic["remediation"]["fixes"]
        repair = next(fix for fix in fixes if SHOW.apply_json_patch(pipeline, fix["patch"])
                      ["io"]["output"][0]["inputs"]["entities"] == "parse_entities.document")
        self.assertEqual(repair["patch"][0], {"op": "test", "path": "/io/output/0",
                                             "value": pipeline["io"]["output"][0]})
        self.assertEqual(repair["patch"][1], {"op": "add", "path": "/io/output/0/inputs",
                                             "value": {"entities": "parse_entities.document"}})
        self.assertEqual(repair["verification"], "pipeline_valid")
        preview = self.service.preview_fix(
            pipeline, repair["patch"],
            expected_revision=SHOW.revision_for(json.dumps(pipeline, sort_keys=True).encode()),
            tool_fingerprint=self.service.get_tool_fingerprint())
        self.assertTrue(preview["report"]["ok"], preview)
        self.assertEqual(preview["patched"]["io"]["output"][0]["inputs"]["entities"], "parse_entities.document")
        self.assertEqual(preview["patched"]["models"], pipeline["models"])
        self.assertEqual(preview["patched"]["pipeline"], pipeline["pipeline"])
        self.assertEqual(pipeline, snapshot, "preview must preserve the source document")


class RunnableSolutionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="studio-test-", dir=ROOT / "build")
        self.root = Path(self.temporary.name)
        self.configs = self.root / "configs"
        self.service = SHOW.WorkbenchService(self.configs)
        self.keyword = json.loads((ROOT / "configs/pipeline_keyword_match_rules.json").read_text())

    def tearDown(self):
        self.temporary.cleanup()

    def test_saved_command_runs_the_selected_pipeline(self):
        self.keyword["pipeline"][0]["params"]["categories"] = {"SAVED_RULE": ["VIP"]}
        filename = f"pipeline_saved_{uuid.uuid4().hex}.json"
        saved = self.service.save_solution(filename, self.keyword, "keyword_match_rules")
        command = shlex.split(saved["command"])
        output = Path(command[command.index("--output-dir") + 1])
        try:
            process = subprocess.run(command[3:], cwd=command[1], text=True, capture_output=True, timeout=30)
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            records = [json.loads(line) for line in (output / Path(saved["conf_filename"]).stem / "results.jsonl").read_text().splitlines()]
            self.assertEqual(records[0]["output"]["match_result"]["intent"], "SAVED_RULE")
        finally:
            shutil.rmtree(output, ignore_errors=True)

    def test_save_targets_follow_session_ownership(self):
        name = "pipeline_targets.json"
        created = self.service.save_solution(name, self.keyword, "keyword_match_rules")
        self.assertEqual(created["save_targets"], [name, "pipeline_targets.conf"])
        opened = self.service.open_pipeline(name)
        self.assertEqual(opened["save_targets"], created["save_targets"])
        updated = self.service.save_pipeline(name, self.keyword, opened["revision"])
        self.assertEqual(updated["save_targets"], created["save_targets"])
        restarted = SHOW.WorkbenchService(self.configs)
        self.assertEqual(restarted.open_pipeline(name)["save_targets"], [name])
        plain = self.service.save_pipeline("pipeline_plain.json", self.keyword, None, save_as=True)
        self.assertEqual(plain["save_targets"], ["pipeline_plain.json"])

    @unittest.skipUnless(os.environ.get("STUDIO_PLAYWRIGHT_MODULE") and shutil.which("node"),
                         "set STUDIO_PLAYWRIGHT_MODULE to run real browser acceptance")
    def test_browser_task_workflow(self):
        self.configs.mkdir()
        for name in ("pipeline_browser.json", "pipeline_browser_other.json"):
            (self.configs / name).write_text(json.dumps(self.keyword))
        shutil.copyfile(ROOT / "configs/pipeline_doc_qa_rerank_cpu.json", self.configs / "pipeline_browser_multi.json")
        server = SHOW.StudioHttpServer(("127.0.0.1", 0), SHOW.make_handler(self.service))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            process = subprocess.run(
                [shutil.which("node"), str(Path(__file__).with_name("studio_browser_test.mjs")),
                 f"http://127.0.0.1:{server.server_address[1]}/index.html", str(self.configs)],
                text=True, capture_output=True, cwd=ROOT, timeout=120,
            )
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_conf_preserves_the_selected_model_file(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        selected = pipeline["models"][0]["file"]
        saved = self.service.save_solution("pipeline_fixture.json", pipeline, "entity_extract_mock")
        self.assertEqual(saved["pipeline"]["models"][0]["file"], selected)
        pipeline["models"][0]["file"] = "replacement.gguf"
        saved = self.service.save_solution("pipeline_replaced.json", pipeline, "entity_extract_mock")
        self.assertEqual(saved["pipeline"]["models"][0]["file"], "replacement.gguf")
        self.assertEqual(json.loads((self.configs / "pipeline_replaced.json").read_text()), pipeline)

    def test_ordinary_save_updates_managed_model_files_and_node_parameters(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        saved = self.service.save_solution("pipeline_paired.json", pipeline, "entity_extract_mock")
        pipeline["models"][0]["file"] = "replacement.gguf"
        pipeline["models"][0]["name"] = "replacement_model"
        pipeline["pipeline"][1]["params"]["bind_model"] = "replacement_model"
        pipeline["pipeline"][1]["params"]["max_tokens"] = 17
        updated = self.service.save_pipeline(
            saved["filename"], pipeline, saved["revision"],
        )
        self.assertEqual(updated["command"], saved["command"])
        self.assertEqual(updated["pipeline"]["models"][0]["file"], "replacement.gguf")
        self.assertEqual(json.loads((self.configs / saved["filename"]).read_text()), pipeline)
        profile = self.service.profile_inputs(pipeline, "entity_extract_mock")
        effective = self.service.resolve_run_conf(self.configs / saved["conf_filename"], profile)
        self.assertEqual(effective, updated["configuration"])
        self.assertEqual(effective["model_files"][0]["resolved"], str(self.configs / "replacement.gguf"))
        self.assertEqual(effective["effective_pipeline"]["pipeline"][1]["params"]["max_tokens"], 17)
        self.assertEqual(sorted(p.name for p in self.configs.iterdir()), ["pipeline_paired.conf", "pipeline_paired.json"])

    def test_managed_save_checks_both_revisions_without_overwriting_external_edits(self):
        saved = self.service.save_solution("pipeline_revision.json", self.keyword, "keyword_match_rules")
        paths = [self.configs / saved["filename"], self.configs / saved["conf_filename"]]
        originals = {path: path.read_bytes() for path in paths}
        self.keyword["pipeline"][0]["params"]["categories"] = {"NEW": ["sample"]}
        for changed in paths:
            with self.subTest(file=changed.name):
                changed.write_bytes(originals[changed] + b"\n")
                before = {path: path.read_bytes() for path in paths}
                with self.assertRaises(SHOW.StudioError) as error:
                    self.service.save_pipeline(saved["filename"], self.keyword, saved["revision"])
                self.assertEqual(error.exception.code, "REVISION_CONFLICT")
                self.assertEqual({path: path.read_bytes() for path in paths}, before)
                changed.write_bytes(originals[changed])

    def test_managed_save_rolls_back_json_if_installing_conf_fails(self):
        saved = self.service.save_solution("pipeline_rollback.json", self.keyword, "keyword_match_rules")
        paths = [self.configs / saved["filename"], self.configs / saved["conf_filename"]]
        originals = {path: path.read_bytes() for path in paths}
        self.keyword["pipeline"][0]["params"]["categories"] = {"NEW": ["sample"]}
        original_replace = SHOW.os.replace
        def reject_conf(source, target):
            if Path(target) == paths[1]:
                raise OSError("simulated conf installation failure")
            return original_replace(source, target)
        with mock.patch.object(SHOW.os, "replace", side_effect=reject_conf):
            with self.assertRaises(SHOW.StudioError) as error:
                self.service.save_pipeline(saved["filename"], self.keyword, saved["revision"])
        self.assertEqual(error.exception.code, "SAVE_FAILED")
        self.assertEqual({path: path.read_bytes() for path in paths}, originals)
        self.assertEqual(set(self.configs.iterdir()), set(paths))
        # 已恢复的失败不得推进任何一个 revision。
        self.assertTrue(self.service.save_pipeline(saved["filename"], self.keyword, saved["revision"])["ok"])

    def test_restarted_service_saves_model_file_without_shadowed_values(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        saved = self.service.save_solution("pipeline_restart.json", pipeline, "entity_extract_mock")
        restarted = SHOW.WorkbenchService(self.configs)
        pipeline["models"][0]["file"] = "replacement.gguf"
        updated = restarted.save_pipeline(saved["filename"], pipeline, saved["revision"])
        self.assertEqual(updated["pipeline"]["models"][0]["file"], "replacement.gguf")
        self.assertEqual(json.loads((self.configs / saved["filename"]).read_text()), pipeline)
        self.assertFalse(restarted.generated_solutions)

    def test_draft_uses_the_original_pipeline_directory_and_cleans_up(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        observed = {}
        original_popen = SHOW.subprocess.Popen
        def inspect_launch(args, **kwargs):
            if "--profiles-file" in args:
                document = json.loads(Path(args[args.index("--profiles-file") + 1]).read_text())
                profile = document["profiles"][args[args.index("--profile") + 1]]
                conf_path = Path(profile["config"])
                observed["conf_path"] = conf_path
                observed["conf"] = json.loads(conf_path.read_text())
                observed["pipeline_path"] = conf_path.parent / observed["conf"]["pipe_path"]
                observed["pipeline"] = json.loads(observed["pipeline_path"].read_text())
            return original_popen(args, **kwargs)
        with mock.patch.object(SHOW.subprocess, "Popen", side_effect=inspect_launch):
            started = self.service.start_run(pipeline, "entity_extract_mock")
            for _ in range(200):
                job = self.service.run_status(started["job_id"])["job"]
                if (job["status"] in ("completed", "failed", "cancelled") and "conf_path" in observed
                        and not observed["conf_path"].exists()):
                    break
                time.sleep(0.05)
        self.assertEqual(job["status"], "completed", job)
        self.assertEqual(observed["pipeline"]["models"], pipeline["models"])
        self.assertEqual(observed["pipeline_path"].parent, self.configs)
        self.assertEqual(observed["conf_path"].parent, self.configs)
        self.assertTrue(observed["pipeline_path"].name.startswith(".studio-run-"))
        self.assertFalse(observed["pipeline_path"].exists())
        self.assertFalse(observed["conf_path"].exists())
        self.assertEqual(job["configuration"]["model_files"][0]["resolved"],
                         str(self.configs / pipeline["models"][0]["file"]))

    def test_conflicts_bad_paths_and_mismatches_leave_no_new_files(self):
        self.configs.mkdir()
        for suffix in ("json", "conf"):
            target = self.configs / f"pipeline_conflict.{suffix}"
            target.write_text("keep this file")
            with self.subTest(suffix=suffix), self.assertRaises(SHOW.StudioError) as error:
                self.service.save_solution("pipeline_conflict.json", self.keyword, "keyword_match_rules")
            self.assertEqual(error.exception.code, "FILE_EXISTS")
            self.assertEqual(list(self.configs.iterdir()), [target])
            self.assertEqual(target.read_text(), "keep this file")
            target.unlink()
        link = self.configs / "pipeline_link.conf"
        link.symlink_to(self.root / "missing_target")
        with self.assertRaises(SHOW.StudioError) as error:
            self.service.save_solution("pipeline_link.json", self.keyword, "keyword_match_rules")
        self.assertEqual(error.exception.code, "SYMLINK_REJECTED")
        self.assertFalse((self.configs / "pipeline_link.json").exists())
        link.unlink()
        for filename, profile in (("../pipeline_escape.json", "keyword_match_rules"),
                                  ("pipeline_bad.json", "entity_extract_mock")):
            with self.subTest(filename=filename, profile=profile), self.assertRaises(SHOW.StudioError):
                self.service.save_solution(filename, self.keyword, profile)
            self.assertEqual(list(self.configs.iterdir()), [])

    def test_second_file_write_failure_rolls_back_only_new_pair(self):
        self.configs.mkdir()
        unrelated = self.configs / "pipeline_keep.json"
        unrelated.write_text("keep")
        original_open = SHOW.os.open
        def fail_conf(path, flags, mode=0o777):
            if Path(path).suffix == ".conf":
                raise OSError("simulated write failure")
            return original_open(path, flags, mode)
        with mock.patch.object(SHOW.os, "open", side_effect=fail_conf):
            with self.assertRaises(SHOW.StudioError) as error:
                self.service.save_solution("pipeline_new.json", self.keyword, "keyword_match_rules")
        self.assertEqual(error.exception.code, "SAVE_FAILED")
        self.assertEqual(list(self.configs.iterdir()), [unrelated])
        self.assertEqual(unrelated.read_text(), "keep")

    def test_native_deployment_rejection_rolls_back_the_pair(self):
        # A failure after exclusive pair creation still removes only the owned pair.
        self.configs.mkdir()
        unrelated = self.configs / "pipeline_keep.json"
        unrelated.write_text("keep")
        resolve = self.service.resolve_run_conf
        reached_pair = []

        def reject_owned_conf(conf_file, profile):
            if Path(conf_file).parent == self.configs:
                self.assertTrue(Path(conf_file).exists())
                self.assertTrue(Path(conf_file).with_suffix(".json").exists())
                reached_pair.append(Path(conf_file))
                raise SHOW.StudioError("DEPLOYMENT_VALIDATION_FAILED", "invalid match_result_json_max_bytes")
            return resolve(conf_file, profile)

        with mock.patch.object(self.service, "resolve_run_conf", side_effect=reject_owned_conf):
            with self.assertRaises(SHOW.StudioError) as error:
                self.service.save_solution("pipeline_invalid_pool.json", self.keyword, "keyword_match_rules")
        self.assertEqual(len(reached_pair), 1)
        self.assertEqual(error.exception.code, "DEPLOYMENT_VALIDATION_FAILED")
        self.assertIn("match_result_json", str(error.exception))
        self.assertEqual(list(self.configs.iterdir()), [unrelated])
        self.assertEqual(unrelated.read_text(), "keep")

    def test_profile_preserves_explicit_io_selection_and_output_parameters(self):
        pipeline = copy.deepcopy(self.keyword)
        output_params(pipeline, "keyword_out")["match_result_json_max_bytes"] = 3071
        snapshot = copy.deepcopy(pipeline)
        saved = self.service.save_solution("pipeline_profile_io.json", pipeline, "keyword_match_rules")
        self.assertEqual(saved["pipeline"], snapshot)
        self.assertEqual(saved["configuration"]["io"]["output"][0]["params"],
                         {"match_result_json_max_bytes": 3071})
        self.assertEqual(saved["configuration"]["output_pools"]["keyword_out"]["capacities"],
                         {"match_result_json": 3071})
        for name in ("", "unknown_converter", "entity_extract"):
            with self.subTest(name=name):
                changed = copy.deepcopy(self.keyword)
                changed["io"]["output"][0]["name"] = name
                before = copy.deepcopy(changed)
                with self.assertRaises(SHOW.StudioError):
                    self.service.save_solution("pipeline_bad_io.json", changed, "keyword_match_rules")
                self.assertEqual(changed, before)
                self.assertFalse((self.configs / "pipeline_bad_io.json").exists())
                self.assertFalse((self.configs / "pipeline_bad_io.conf").exists())


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
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}/api"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.temporary.cleanup()

    def post(self):
        pipeline = json.loads((ROOT / "configs" / "pipeline_keyword_match_rules.json").read_text())
        request = urllib.request.Request(
            self.base + "/validate",
            data=json.dumps({"pipeline": pipeline}).encode(),
            method="POST",
            headers={"Content-Type": "application/json"},
        )
        return urllib.request.urlopen(request, timeout=5)

    def test_authoring_http_rejects_unknown_fields_and_conflicting_revisions(self):
        pipeline = json.loads((ROOT / "configs" / "pipeline_keyword_match_rules.json").read_text())
        body = {
            "pipeline": pipeline,
            "operation": {'kind': 'add_node', 'type': 'text_template'},
            "revision": SHOW.revision_for(json.dumps(pipeline, sort_keys=True).encode()),
            "tool_fingerprint": self.service.get_tool_fingerprint(),
        }
        for fields, status, code in [
            ({"requrie_valid": True}, 400, "INVALID_AUTHORING_REQUEST"),
            ({"expected_revision": "different_revision"}, 409, "REVISION_CONFLICT"),
        ]:
            with self.subTest(fields=fields):
                request = urllib.request.Request(
                    self.base + "/authoring/preview", data=json.dumps(dict(body, **fields)).encode(),
                    method="POST", headers={"Content-Type": "application/json"},
                )
                with mock.patch.object(self.service, "preview_authoring") as authoring:
                    with self.assertRaises(urllib.error.HTTPError) as ctx:
                        urllib.request.urlopen(request, timeout=5)
                    self.assertEqual(ctx.exception.code, status)
                    with ctx.exception as response:
                        result = json.load(response)
                    self.assertFalse(result["ok"])
                    self.assertEqual(result["error"]["code"], code)
                    self.assertNotIn("pipeline", result)
                    authoring.assert_not_called()

    def test_local_development_api_and_static_modules(self):
        origin = self.base.removesuffix("/api")
        with urllib.request.urlopen(origin + "/index.html", timeout=5) as response:
            index = response.read().decode()
        self.assertIn('type="module" src="app.js"', index)
        self.assertIn('id="arrow-hover"', index)
        with urllib.request.urlopen(origin + "/styles.css", timeout=5) as response:
            css = response.read().decode()
        self.assertIn(".has-model", css)
        self.assertIn(".has-error", css)
        with urllib.request.urlopen(origin + "/app.js", timeout=5) as response:
            self.assertEqual(response.status, 200)
            self.assertTrue(response.read())
        with self.post() as response:
            payload = json.load(response)
        self.assertTrue(payload["ok"])

    def test_initial_endpoint_only_exposes_explicit_startup_document(self):
        with urllib.request.urlopen(self.base + "/initial?filename=/etc/passwd", timeout=5) as response:
            self.assertIsNone(json.load(response)["document"])
        expected = {"filename": "chosen.json", "pipeline": {"pipeline": []}, "imported": True}
        self.service.initial_document = expected
        with urllib.request.urlopen(self.base + "/initial?filename=/etc/passwd", timeout=5) as response:
            self.assertEqual(json.load(response)["document"], expected)

    def test_startup_endpoints_load_rerank_pipeline_and_refresh_saved_list(self):
        path = ROOT / "configs/pipeline_doc_qa_rerank_cpu.json"
        pipeline = json.loads(path.read_text())
        managed = Path(self.temporary.name) / path.name
        managed.write_text(json.dumps(pipeline))
        self.service.initial_document = self.service.open_pipeline(path.name)
        for endpoint in ("/catalog", "/profiles", "/pipelines", "/assets", "/initial",
                         "/catalog?view=1"):
            with self.subTest(endpoint=endpoint), urllib.request.urlopen(self.base + endpoint, timeout=10) as response:
                payload = json.load(response)
                self.assertTrue(payload["ok"])
                if endpoint == "/pipelines":
                    self.assertEqual(payload["pipelines"], self.service.pipelines()["pipelines"])
                    self.assertEqual(payload["pipelines"][0]["filename"], path.name)
                elif endpoint == "/initial":
                    self.assertEqual(payload["document"]["pipeline"], pipeline)
        keyword = json.loads((ROOT / "configs/pipeline_keyword_match_rules.json").read_text())
        self.service.save_pipeline("pipeline_saved.json", keyword, None, save_as=True)
        with urllib.request.urlopen(self.base + "/pipelines", timeout=5) as response:
            self.assertEqual({item["filename"] for item in json.load(response)["pipelines"]},
                             {path.name, "pipeline_saved.json"})

    @unittest.skipUnless(shutil.which("node"), "Node.js is required for Web module tests")
    def test_web_api_preserves_proxy_paths_and_reports_non_json_responses(self):
        script = """
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
globalThis.location = { href: "http://127.0.0.1:8080/index.html", hash: "" };
const source = readFileSync(process.argv[1], "utf8");
const { api, write } = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);
let requested, options;
let body = JSON.stringify({ ok: true, pipelines: [{ filename: "pipeline_test.json" }] });
let status = 200;
globalThis.fetch = async (url, init) => {
  requested = String(url); options = init;
  return new Response(body, { status });
};
for (const base of ["http://127.0.0.1:8080/", "https://studio.example/proxy/8080/",
                    "https://studio.example/workspace/proxy/8080/"]) {
  for (const page of ["", "index.html?view=1#pipeline=chosen"]) {
    location.href = base + page;
    assert.equal((await api("/pipelines")).pipelines[0].filename, "pipeline_test.json");
    assert.equal(requested, base + "api/pipelines");
    await api("/catalog?view=1");
    assert.equal(requested, base + "api/catalog?view=1");
  }
}
body = JSON.stringify({ ok: false, error: { code: "INVALID_JSON", message: "invalid pipeline" } });
status = 400;
await assert.rejects(api("/pipeline"), error => error.status === 400 &&
  error.message.includes("invalid pipeline") && error.message.includes("/api/pipeline") &&
  error.message.includes("HTTP 400") && error.message.includes("INVALID_JSON") && error.payload.error.code === "INVALID_JSON");
status = 200;
await assert.rejects(api("/validate"), /invalid pipeline/);
const result = await write("/validate", "POST", { pipeline: [] }, true);
assert.equal(result.ok, false);
assert.equal(options.method, "POST");
assert.deepEqual(JSON.parse(options.body), { pipeline: [] });
assert.equal(options.headers["Content-Type"], "application/json");
assert.equal(options.allowFalse, undefined);
for (const [code, text, summary] of [[404, "Not Found", "Not Found"],
    [502, "<html>Bad Gateway</html>", "Bad Gateway"], [200, "", "空响应"],
    [200, '{"incomplete":', "incomplete"]]) {
  status = code; body = text;
  await assert.rejects(api("/initial"), error => error.status === code &&
    error.message.includes("/workspace/proxy/8080/api/initial") &&
    error.message.includes(`HTTP ${code}`) && error.message.includes(summary));
}
status = 500; body = "Not Found " + "x".repeat(1000) + "END_OF_BODY";
await assert.rejects(api("/initial"), error => !error.message.includes("END_OF_BODY"));
globalThis.fetch = async () => { throw new TypeError("Failed to fetch"); };
await assert.rejects(api("/catalog"), error => error.message.includes("/api/catalog") && error.message.includes("Failed to fetch"));
"""
        process = subprocess.run(
            [shutil.which("node"), "--input-type=module", "-e", script, str(WEB_ROOT / "api.js")],
            text=True, capture_output=True, cwd=ROOT, check=False,
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)

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
  {{ name: "embed", type: "embedding" }},
  {{ name: "llm", type: "llm" }},
  {{ name: "other_embed", type: "embedding" }},
];
const modelDefinitions = [
  {{ impl_name: "embed_type", model_type: "embedding", backends: [] }},
  {{ impl_name: "llm_type", model_type: "llm", backends: [] }},
  {{ impl_name: "other_embed_type", model_type: "embedding", backends: [] }},
];
const nodeDefinition = {{ model_dependencies: [{{ name: "encoder", model_type: "embedding", config_field: "model_slot" }}] }};
assert.deepEqual(
  workbench.compatibleModels(models, modelDefinitions, nodeDefinition).map(model => model.name),
  ["embed", "other_embed"]
);
assert.deepEqual(
  [...workbench.modelBoundNodeIds([
    {{ name: "bound", type: "custom_node", params: {{ model_slot: "embed" }} }},
    {{ name: "hardcoded", type: "custom_node", params: {{ bind_model: "llm" }} }},
  ], [{{ node_type: "custom_node", model_dependencies: [{{ name: "encoder", model_type: "embedding", config_field: "model_slot" }}] }}])],
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
    def test_untouched_apply_preserves_repository_configs(self):
        with urllib.request.urlopen(self.base + "/catalog", timeout=5) as response:
            catalog = json.load(response)
        pipelines = [str(path) for pattern in ("configs/pipeline_*.json", "demo/fixtures/mock/pipeline_*.json")
                     for path in sorted(ROOT.glob(pattern))]
        with tempfile.TemporaryDirectory(prefix="studio-roundtrip-", dir=ROOT / "build") as directory:
            catalog_path = Path(directory) / "catalog.json"
            catalog_path.write_text(json.dumps(catalog), encoding="utf-8")
            process = subprocess.run(
                [shutil.which("node"), str(Path(__file__).with_name("studio_config_roundtrip_test.mjs")),
                 str(catalog_path), *pipelines],
                text=True, capture_output=True, cwd=ROOT, check=False)
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)

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
    def test_actual_fix_handler_history_and_stale_response_protection(self):
        process = subprocess.run(
            [shutil.which("node"), "--experimental-vm-modules",
             str(Path(__file__).with_name("studio_fix_workflow_test.mjs"))],
            text=True, capture_output=True, cwd=ROOT, check=False,
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)

    @unittest.skipUnless(shutil.which("node"), "Node.js is required for Web module tests")
    def test_readonly_graph_matches_native_validator_and_model_forms(self):
        catalog = json.loads(subprocess.check_output([str(PIPELINE_TOOL), "catalog"], text=True))
        script = """
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const code = readFileSync(process.argv[1], 'utf8');
const w = await import(`data:text/javascript;base64,${Buffer.from(code).toString('base64')}`);
const catalog = JSON.parse(readFileSync(0, 'utf8'));
const pipeline = {io:{input:[{type:'keyword_in',name:'keyword_match'}],
  output:[{type:'keyword_out',name:'keyword_match',inputs:{matches:'rule.matches'}}]}, models:[], pipeline:[
  {name:'template', type:'text_template', params:{template:'{{primary}}'}, inputs:{primary:'input.sentence_text'}},
  {name:'rule', type:'text_rule_match', depends_on:['template'], inputs:{text:'template.text'}}
]};
const snapshot = structuredClone(pipeline);
const graph = w.graphDocument(pipeline,catalog);
assert.ok(graph.edges.some(e=>e.source===w.INGRESS && e.target==='template' && e.targetPort==='primary'));
assert.ok(graph.edges.some(e=>e.source==='template' && e.sourcePort==='text' && e.target==='rule' && e.targetPort==='text'));
assert.ok(graph.edges.some(e=>e.source==='rule' && e.target===w.EGRESS && e.targetPort==='matches'));
assert.deepEqual(pipeline,snapshot,'graph rendering must not mutate the document');
const modelDef = catalog.models.find(m=>m.impl_name==='bge_embedding');
const backend = w.compatibleBackends(catalog.backends,modelDef).find(b=>b.backend_type==='onnxruntime');
if (backend) {
  assert.ok(!w.compatibleBackends(catalog.backends,modelDef).some(b=>b.backend_type==='llama_cpp'));
  const nestedAsset = {model:{file:'nested/weights.bin',params:{tokenizer_file:'vocab.txt'}},
    paths:{'/file':'nested/weights.bin','/params/tokenizer_file':'vocab.txt'}};
  assert.deepEqual(w.assetModel(nestedAsset), nestedAsset.model);
  const io = {input:[{type:'doc_in',name:'doc_qa'}],output:[{type:'doc_out',name:'doc_qa'}]};
  const models = {io:structuredClone(io),models:[], pipeline:[]};
  w.upsertModel(models,catalog,'',{name:'embed',type:modelDef.model_type,
    backend:{type:backend.backend_type},file:'embed.onnx',params:{tokenizer_file:'vocab.txt'}});
  assert.deepEqual(models.io,io,'adding a model must preserve I/O selection');
  assert.deepEqual(models.models[0].params,{tokenizer_file:'vocab.txt'},'untouched defaults stay implicit');
  models.pipeline.push({name:'embed_node',type:'text_embedding',params:{bind_model:'embed'}});
  w.upsertModel(models,catalog,'embed',{...models.models[0],name:'renamed'});
  assert.equal(models.pipeline[0].params.bind_model,'renamed');
  assert.equal(models.models[0].file,'embed.onnx');
  assert.deepEqual(models.io,io);
  assert.throws(()=>w.removeModel(models,catalog,'renamed'), /使用/);
  assert.throws(()=>w.upsertModel(models,catalog,'',{...models.models[0],backend:{type:'llama_cpp'}}), /Backend/);
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

    def test_representative_validation_reports_match_cli_and_http(self):
        fixture_path = (
            ROOT
            / "tests"
            / "fixtures"
            / "pipelines"
            / "validation"
            / "invalid_pipeline_cases.json"
        )
        fixtures = json.loads(fixture_path.read_text(encoding="utf-8"))

        for case in fixtures["cases"]:
            if case["name"] not in {"missing_node_name", "config_field_range", "model_type_mismatch"}:
                continue
            with self.subTest(case=case["name"]):
                pipeline = copy.deepcopy(case["pipeline"])
                pipeline["io"] = copy.deepcopy(case["io"])
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

                # Web 按字节语义透传 CLI/Validator 报告；所有可选诊断字段都会比较。
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
                # 准备阶段错误带显式的空 plan 信封；
                # 诊断与 validate 和 HTTP API 保持一致。
                if "plan" not in cli_val:
                    self.assertEqual(cli_plan.pop("plan"), {"layers": [], "topological_order": []})
                self.assertEqual(cli_plan, cli_val)

    def test_init_multiple_io_pairs_roundtrips_and_keeps_independent_output_pools(self):
        inputs = [{"type": "keyword_in", "name": "keyword_match"},
                  {"type": "audit_in", "name": "dialogue_audit"}]
        outputs = [{"type": "entity_out", "name": "entity_extract"},
                   {"type": "entity_out", "name": "translate"}]
        request = urllib.request.Request(
            self.base + "/init", data=json.dumps({"input": inputs, "output": outputs}).encode(),
            method="POST", headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=10) as response:
            initialized = json.load(response)
        self.assertTrue(initialized["ok"], initialized)
        pipeline = initialized["pipeline"]
        self.assertEqual(pipeline["io"], {"input": inputs, "output": outputs})
        pipeline["pipeline"] = [{"type": "structured_json_parse", "name": "parse",
                                 "inputs": {"text": "input.user_text"}}]
        pipeline["io"]["output"][0].update(inputs={"entities": "parse.document"},
                                          params={"entities_json_max_bytes": 511})
        pipeline["io"]["output"][1].update(inputs={"translation": "input.user_text"},
                                          params={"entities_json_max_bytes": 255})
        validation = self.service.validate(pipeline)
        self.assertTrue(validation["ok"], validation)
        saved = self.service.save_pipeline("pipeline_multi.json", pipeline, None, save_as=True)
        opened = self.service.open_pipeline(saved["filename"])
        self.assertEqual(opened["pipeline"], pipeline)
        self.assertEqual(opened["revision"], saved["revision"])
        conf = Path(self.temporary.name) / "pipeline_multi.conf"
        conf.write_text(json.dumps({"pipe_path": saved["filename"]}))
        configured = self.service.resolve_run_conf(conf, {})
        pools = configured["output_pools"]
        self.assertEqual(set(pools), {"entity_extract.entity_out", "translate.entity_out"})
        self.assertEqual(pools["entity_extract.entity_out"]["capacities"], {"entities_json": 511})
        self.assertEqual(pools["translate.entity_out"]["capacities"], {"entities_json": 255})
        self.assertIsInstance(pools["translate.entity_out"]["params"], str)
        self.assertEqual(configured["io"]["output"][1]["params"]["entities_json_max_bytes"], 255)


class SelectionVerificationTest(unittest.TestCase):
    def test_asset_catalog_exposes_only_public_selection_variants(self):
        selection = SHOW.SELECTION
        catalog = selection.asset_catalog()
        self.assertEqual({row["name"] for row in catalog["variants"]},
                         {"minimal", "default-cpu", "kite-cpu"})
        with tempfile.TemporaryDirectory() as directory:
            presets = Path(directory) / "CMakePresets.json"
            presets.write_text(json.dumps({"configurePresets": [
                {"name": "base", "hidden": True},
                {"name": "internal", "hidden": True,
                 "vendor": {"llm-edgeflow/selection": {"backends": ["internal"]}}},
                {"name": "gate"},
                {"name": "unrelated", "vendor": {"other/tool": {}}},
                {"name": "public", "vendor": {"llm-edgeflow/selection": {"backends": []}}}
            ]}))
            with mock.patch.object(selection, "PRESETS", presets):
                self.assertEqual(selection.asset_catalog()["variants"],
                                 [{"name": "public", "backends": []}])

    def test_inspection_rejects_presets_without_public_selection_metadata(self):
        selection = SHOW.SELECTION
        presets = selection.read_json(selection.PRESETS)["configurePresets"]
        private = [preset["name"] for preset in presets
                   if preset.get("hidden") or "llm-edgeflow/selection" not in preset.get("vendor", {})]
        self.assertTrue(private)
        with mock.patch.object(selection, "native", side_effect=lambda tool, command, *args:
                               {"backends": []} if command == "catalog" else {"ok": True}), \
                mock.patch.object(selection, "file_digest", return_value="mock-tool-digest"):
            for variant in [*private, "missing-preset"]:
                with self.subTest(variant=variant), self.assertRaisesRegex(ValueError, "Unknown build variant"):
                    selection.inspect_selection({"models": []}, Path("mock-tool"), ROOT / "configs", variant=variant)
            report = selection.inspect_selection({"models": []}, Path("mock-tool"), ROOT / "configs", variant="minimal")
            self.assertTrue(report["ok"])
            self.assertEqual(report["build"]["status"], "verified")

    def test_studio_selection_uses_the_managed_pipeline_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            service = SHOW.WorkbenchService(root)
            pipeline = {"models": []}
            (root / "pipeline_selected.json").write_text(json.dumps(pipeline))
            with mock.patch.object(SHOW.SELECTION, "inspect_selection", return_value={"ok": True}) as inspect:
                self.assertTrue(service.verify_selection(pipeline, filename="pipeline_selected.json")["ok"])
                self.assertEqual(inspect.call_args.args[2], root)
                self.assertTrue(service.verify_selection(pipeline)["ok"])
                self.assertIsNone(inspect.call_args.args[2], "an imported draft has no known resource directory")
            with self.assertRaises(SHOW.StudioError):
                service.verify_selection(pipeline, filename="../outside.json")

    def test_run_conf_preserves_binding_override_without_inference(self):
        selection = SHOW.SELECTION
        pipeline = {"io": {
            "input": [{"type": "keyword_in", "name": "keyword_match", "params": {"max_input_bytes": 100}}],
            "output": [{"type": "keyword_out", "name": "keyword_match", "params": {"match_result_json_max_bytes": 1024}},
                       {"type": "entity_out", "name": "translate", "params": {"entities_json_max_bytes": 1024}}]},
            "models": []}
        snapshot = copy.deepcopy(pipeline)
        conf = selection.build_run_conf(pipeline, [], "nested/pipeline.json")
        self.assertEqual(conf, {"pipe_path": "pipeline.json"})
        self.assertEqual(pipeline, snapshot)
        selection.build_run_conf(pipeline, [{"type": "keyword_out", "name": "keyword_match",
            "params": {"match_result_json_max_bytes": 2048}}], "pipeline.json")
        self.assertEqual(pipeline["io"]["input"], snapshot["io"]["input"])
        self.assertEqual(pipeline["io"]["output"][1], snapshot["io"]["output"][1])
        self.assertEqual(output_params(pipeline, "keyword_out")["match_result_json_max_bytes"], 2048)
        with self.assertRaises(ValueError):
            selection.build_run_conf(pipeline, [{"type": "keyword_out", "name": "unselected"}], "pipeline.json")

    def test_asset_hashes_include_sidecars_and_changed_paths_are_unregistered(self):
        selection = SHOW.SELECTION
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "weights.bin").write_bytes(b"test weights")
            (root / "vocab.txt").write_bytes(b"tokenizer")
            model = {'name': 'm', 'type': 'embedding', 'backend': {'type': 'onnxruntime'}, 'file': 'weights.bin', 'params': {'tokenizer_file': 'vocab.txt'}}
            manifest = {"selections": [{"id": "test", "model": json.loads(json.dumps(model)), "paths": {"/file": "weights.bin", "/params/tokenizer_file": "vocab.txt"}, "files": ["weights.bin", "vocab.txt"]}], "artifacts": {name: {"sha256": selection.file_digest(root / name)} for name in ("weights.bin", "vocab.txt")}}
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
            model["params"]["tokenizer_file"] = "another_vocab.txt"
            self.assertEqual(selection.verify_assets(pipeline, root, manifest)[0]["status"], "unregistered")
            with self.assertRaises(ValueError):
                selection.within(root, "../outside")

    def test_main_file_and_sidecars_are_relative_to_the_pipeline_directory(self):
        selection = SHOW.SELECTION
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nested = root / "nested"
            nested.mkdir()
            (nested / "weights.bin").write_bytes(b"weights")
            (root / "vocab.txt").write_bytes(b"vocabulary")
            model = {"name": "m", "type": "embedding", "backend": {"type": "onnxruntime"},
                     "file": "nested/weights.bin", "params": {"tokenizer_file": "vocab.txt"}}
            files = ["nested/weights.bin", "vocab.txt"]
            manifest = {"selections": [{"id": "nested", "model": copy.deepcopy(model),
                "paths": {"/file": files[0], "/params/tokenizer_file": files[1]}, "files": files}],
                "artifacts": {name: {"sha256": selection.file_digest(root / name)} for name in files}}
            if not shutil.which("node"):
                self.skipTest("Node.js is required to exercise the asset-selection helper")
            script = """
        import {assetModel} from './tools/pipeline_studio/web/workbench.js';
        import fs from 'node:fs';
        const asset = JSON.parse(fs.readFileSync(0, 'utf8'));
        process.stdout.write(JSON.stringify(assetModel(asset)));
        """
            generated = subprocess.run([shutil.which("node"), "--input-type=module", "-e", script],
                                       input=json.dumps(manifest["selections"][0]), cwd=ROOT,
                                       text=True, capture_output=True, check=True)
            selected = json.loads(generated.stdout)
            self.assertEqual(selected, model)
            pipeline = {"models": [selected]}
            checked = selection.verify_assets(pipeline, root, manifest)[0]
            self.assertEqual(checked["status"], "verified")
            self.assertEqual([row["path"] for row in checked["files"]], [str(root / name) for name in files])
            original = copy.deepcopy(pipeline)
            selection.build_run_conf(pipeline, [], "pipeline.json")
            selection.build_run_conf(pipeline, [], "pipeline.json")
            self.assertEqual(pipeline, original, "saving must preserve paths relative to the Pipeline directory")
            selected["params"]["tokenizer_file"] = "../vocab.txt"
            self.assertEqual(selection.verify_assets(pipeline, root, manifest)[0]["status"], "unregistered")
            selected["params"]["tokenizer_file"] = "nested/vocab.txt"
            self.assertEqual(selection.verify_assets(pipeline, root, manifest)[0]["status"], "unregistered")
            selected["params"]["tokenizer_file"] = "vocab.txt"
            outside = root.parent / (root.name + "-outside")
            outside.write_bytes(b"outside")
            try:
                (root / "vocab.txt").unlink()
                (root / "vocab.txt").symlink_to(outside)
                self.assertEqual(selection.verify_assets(pipeline, root, manifest)[0]["status"], "unregistered")
            finally:
                outside.unlink()

    def test_native_build_variant_and_real_effects_are_bound_to_selection(self):
        selection = SHOW.SELECTION
        tool = Path(os.environ.get("LLM_EDGEFLOW_SELECTION_TOOL", ROOT / "build/alg_pipeline_tool"))
        demo = SHOW.DEMO_BINARY
        pipeline = json.loads((ROOT / "configs/pipeline_keyword_match_rules.json").read_text())
        spec = ROOT / "tests/fixtures/effects/keyword_exact.json"
        conf = ROOT / "configs/pipeline_keyword_match_rules.conf"
        report = selection.inspect_selection(pipeline, tool, ROOT / "configs")
        self.assertTrue(report["ok"])
        self.assertFalse(report["ready_for_biz"])
        self.assertNotIn("ready_for_business", report)
        # 当前规范构建启用了若干 Backend；声明为空的 minimal 变体必须失败，
        # 与这个无模型的 Pipeline 无关。
        mismatched = selection.inspect_selection(pipeline, tool, ROOT / "configs", variant="minimal" if report["build"]["enabled_backends"] else "default-cpu")
        self.assertFalse(mismatched["ok"])
        receipt = selection.evaluate(pipeline, report, tool, ROOT / "configs", spec, conf, demo)
        self.assertEqual(receipt["metrics"]["pass_rate"], 1.0)
        self.assertEqual(receipt["metrics"]["total"], 4)
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "effects.json"
            evidence.write_text(json.dumps(receipt))
            checked = selection.attach_evidence(report, evidence, spec, conf, demo, ROOT / "configs")
            self.assertTrue(checked["ready_for_biz"])
            pipeline["pipeline"][0]["params"]["categories"] = {"OTHER": ["different"]}
            changed = selection.inspect_selection(pipeline, tool, ROOT / "configs")
            self.assertEqual(selection.attach_evidence(changed, evidence, spec, conf, demo, ROOT / "configs")["effects"]["status"], "stale")
        broken_records = receipt["records"][:-1]
        self.assertEqual(selection.compare_samples(broken_records, selection.read_json(spec))["status"], "failed")
        records = json.loads(json.dumps(receipt["records"]))
        records[0]["output"]["is_hit"] = False
        self.assertEqual(selection.compare_samples(records, selection.read_json(spec))["status"], "failed")

    def test_evaluate_inherits_output_configuration_without_changing_selection(self):
        selection = SHOW.SELECTION
        tool = Path(os.environ.get("LLM_EDGEFLOW_SELECTION_TOOL", ROOT / "build/alg_pipeline_tool"))
        pipeline = selection.read_json(ROOT / "configs/pipeline_keyword_match_rules.json")
        spec = ROOT / "tests/fixtures/effects/keyword_exact.json"
        outputs = [{"type": "keyword_out", "name": "keyword_match",
                    "params": {"match_result_json_max_bytes": 2048}}]
        with tempfile.TemporaryDirectory(prefix="selection-inheritance-", dir=ROOT / "build") as directory:
            root = Path(directory)
            source = root / "selected.json"
            source.write_text(json.dumps(pipeline))
            inherited = copy.deepcopy(pipeline)
            inherited["io"]["output"][0]["params"] = copy.deepcopy(outputs[0]["params"])
            deployment = root / "deployment.json"
            deployment.write_text(json.dumps(inherited))
            conf = root / "deployment.conf"
            conf.write_text(json.dumps({"pipe_path": deployment.name}))
            evidence = root / "effects.json"
            arguments = ["verify_selection.py", "evaluate", "--pipeline", str(source),
                         "--tool", str(tool), "--demo", str(SHOW.DEMO_BINARY),
                         "--effects", str(spec), "--conf", str(conf), "--output", str(evidence)]
            with mock.patch.object(sys, "argv", arguments), mock.patch.object(sys, "stdout", io.StringIO()) as stdout:
                self.assertEqual(selection.main(), 0, stdout.getvalue())
            receipt = selection.read_json(evidence)
            self.assertEqual(receipt["metrics"]["status"], "passed")
            self.assertEqual(receipt["metrics"]["total"], 4)
            self.assertEqual(receipt["pipeline"]["io"]["output"][0]["params"], outputs[0]["params"])
            self.assertEqual(selection.read_json(source), pipeline)

            report = selection.inspect_selection(pipeline, tool, root)
            snapshot = copy.deepcopy(pipeline)
            selection.evaluate(pipeline, report, tool, root, spec, conf, SHOW.DEMO_BINARY)
            self.assertEqual(pipeline, snapshot, "evaluation must not mutate the selected Pipeline")
            self.assertEqual(report["selection_fingerprint"], receipt["selection_fingerprint"])
            checked = selection.attach_evidence(copy.deepcopy(report), evidence, spec, conf, SHOW.DEMO_BINARY, root)
            self.assertTrue(checked["ready_for_biz"])

            inherited["io"]["output"][0]["params"]["match_result_json_max_bytes"] = 4096
            deployment.write_text(json.dumps(inherited))
            changed = selection.attach_evidence(copy.deepcopy(report), evidence, spec, conf, SHOW.DEMO_BINARY, root)
            self.assertEqual(changed["effects"]["status"], "stale")
            self.assertFalse(changed["ready_for_biz"])

    def test_evaluate_still_rejects_deployment_changes_during_execution(self):
        selection = SHOW.SELECTION
        tool = Path(os.environ.get("LLM_EDGEFLOW_SELECTION_TOOL", ROOT / "build/alg_pipeline_tool"))
        pipeline = selection.read_json(ROOT / "configs/pipeline_keyword_match_rules.json")
        spec = ROOT / "tests/fixtures/effects/keyword_exact.json"
        with tempfile.TemporaryDirectory(prefix="selection-deployment-change-", dir=ROOT / "build") as directory:
            root = Path(directory)
            deployment = root / "pipeline.json"
            deployment.write_text(json.dumps(pipeline))
            conf = root / "pipeline.conf"
            conf.write_text(json.dumps({"pipe_path": deployment.name}))
            report = selection.inspect_selection(pipeline, tool, root)
            run = subprocess.run

            def run_with_changed_deployment(command, **kwargs):
                result = run(command, **kwargs)
                if Path(command[0]).resolve() == SHOW.DEMO_BINARY.resolve():
                    output_params(pipeline, "keyword_out")["match_result_json_max_bytes"] = 4096
                    deployment.write_text(json.dumps(pipeline))
                return result

            with mock.patch.object(selection.subprocess, "run", side_effect=run_with_changed_deployment):
                with self.assertRaisesRegex(ValueError, "Effect inputs or binaries changed during execution"):
                    selection.evaluate(pipeline, report, tool, root, spec, conf, SHOW.DEMO_BINARY)


class AuthoringAndDeploymentTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="authoring-deployment-test-", dir=ROOT / "build")
        self.root = Path(self.temporary.name)
        self.configs = self.root / "configs"
        self.configs.mkdir(parents=True, exist_ok=True)
        self.service = SHOW.WorkbenchService(self.configs)
        self.keyword = json.loads(
            (ROOT / "configs" / "pipeline_keyword_match_rules.json").read_text()
        )

    def tearDown(self):
        self.temporary.cleanup()

    def run_edit(self, request: dict) -> tuple[int, dict]:
        proc = subprocess.run(
            [str(PIPELINE_TOOL), "edit", "--stdin"],
            input=json.dumps(request),
            text=True,
            capture_output=True,
            cwd=ROOT,
            check=False,
        )
        try:
            payload = json.loads(proc.stdout)
        except json.JSONDecodeError:
            payload = {"raw": proc.stdout, "stderr": proc.stderr}
        return proc.returncode, payload

    def assert_edit_rejected(self, request, failed_index=None):
        code, result = self.run_edit(request)
        self.assertEqual(code, 1, result)
        self.assertIs(result.get("ok"), False, result)
        self.assertTrue(result.get("diagnostics"), result)
        self.assertNotIn("pipeline", result)
        if failed_index is not None:
            self.assertEqual(result.get("failed_operation_index"), failed_index, result)

    def test_authoring_connect_preserves_valid_default_output_fanout(self):
        pipe = copy.deepcopy(self.keyword)
        existing = pipe["pipeline"][0]
        existing["name"] = "b"
        existing.pop("depends_on", None)
        existing["inputs"] = {"text": "a.text"}
        source = {"name": "a", "type": "text_template", "inputs": {"primary": "input.sentence_text"},
                  "params": {"template": "{{primary}}"}}
        consumer = copy.deepcopy(existing)
        consumer.update(name="c", inputs={"text": "input.sentence_text"})
        pipe["pipeline"] = [source, existing, consumer]
        pipe["io"]["output"][0]["inputs"]["matches"] = "b.matches"
        self.assertTrue(self.service.validate(pipe)["ok"])
        code, result = self.run_edit({"pipeline": pipe, "require_valid": True,
            "operation": {"kind": "connect", "source": {"node": "a", "port": "text"},
                          "target": {"node": "c", "port": "text"}}})
        self.assertEqual(code, 0, result)
        self.assertTrue(result["validation"]["ok"])
        a, b, c = result["pipeline"]["pipeline"]
        self.assertEqual(a, source)
        self.assertEqual(b, existing)
        self.assertEqual(c["inputs"]["text"], "a.text")
        self.assertNotIn("depends_on", c)

    def test_authoring_disconnect_rejects_unknown_and_reversed_endpoints(self):
        pipe = copy.deepcopy(self.keyword)
        pipe["pipeline"][0]["name"] = "c"
        pipe["pipeline"].insert(0, {'name': 'a', 'type': 'text_template', 'depends_on': [], 'inputs': {'primary': 'input.sentence_text'}, 'params': {'template': '{{primary}}'}})
        cases = [
            ({"node": "a", "port": "sentence_text"}, {"node": "c", "port": "text"}),
            ({"node": "input", "port": "missing"}, {"node": "c", "port": "text"}),
            ({"node": "output", "port": "matches"}, {"node": "c", "port": "text"}),
            ({"node": "c", "port": "matches"}, {"node": "output", "port": "missing"}),
            ({"node": "c", "port": "matches"}, {"node": "input", "port": "sentence_text"}),
            ({"node": "input", "port": "sentence_text"}, {"node": "c", "port": "missing"}),
        ]
        for source, target in cases:
            with self.subTest(source=source, target=target):
                self.assert_edit_rejected({
                    "pipeline": pipe,
                    "operation": {"kind": "disconnect", "source": source, "target": target},
                }, 0)

    def test_authoring_rejects_ambiguous_duplicate_node_targets(self):
        pipe = copy.deepcopy(self.keyword)
        pipe["pipeline"][0]["name"] = "b"
        pipe["pipeline"].append(copy.deepcopy(pipe["pipeline"][0]))
        operations = [
            {"kind": "rename_node", "node": "b", "new_name": "bb"},
            {"kind": "remove_node", "node": "b"},
            {"kind": "connect", "source": {"node": "input", "port": "sentence_text"},
             "target": {"node": "b", "port": "text"}},
            {"kind": "connect", "source": {"node": "b", "port": "matches"},
             "target": {"node": "output", "port": "matches"}},
        ]
        for operation in operations:
            with self.subTest(operation=operation):
                self.assert_edit_rejected({"pipeline": pipe,
                                           "operation": operation}, 0)

    def test_authoring_rejects_invalid_request_fields_without_crashing(self):
        base = {"pipeline": self.keyword,
                "operation": {'kind': 'add_node', 'type': 'text_template'}}
        requests = [
            dict(base, requrie_valid=True), dict(base, require_valid="true"),
            dict(base, operations=None), dict(base, operation=None, operations=[base["operation"]]),
            dict(base, operations={}), dict(base, pipeline=[]),
        ]
        for request in requests:
            with self.subTest(request=request):
                self.assert_edit_rejected(request)

    def test_authoring_batch_schema_errors_report_index_and_no_candidate(self):
        invalid_operations = [
            {"kind": 42}, {"kind": "remove_node", "node": 42},
            {'kind': 'add_node', 'type': 'text_template', 'params': []},
            {'kind': 'add_node', 'type': 'text_template', 'unexpected': True},
            {"kind": "connect", "source": [], "target": {}},
            {"kind": "connect", "source": {"node": "input", "port": "sentence_text", "unexpected": 1},
             "target": {"node": "added", "port": "primary"}},
            {"kind": "connect", "source": {"node": "input", "port": 42},
             "target": {"node": "added", "port": "primary"}},
            None, [],
        ]
        for operation in invalid_operations:
            with self.subTest(operation=operation):
                self.assert_edit_rejected({
                    "pipeline": self.keyword,
                    "operations": [
                        {'kind': 'add_node', 'type': 'text_template', 'name': 'added'},
                        operation,
                    ],
                }, 1)

    def test_authoring_add_node_explicit_name_and_errors(self):
        req = {
            "pipeline": {'io': {'input': [{'type': 'keyword_in', 'name': 'keyword_match'}], 'output': [{'type': 'keyword_out', 'name': 'keyword_match', 'inputs': {}}]}, 'models': [], 'pipeline': []},
            "operation": {'kind': 'add_node', 'type': 'text_rule_match', 'name': 'custom_rule'},
        }
        code, res = self.run_edit(req)
        self.assertEqual(code, 0)
        self.assertTrue(res["ok"])
        added = res["pipeline"]["pipeline"][0]
        self.assertEqual(added["name"], "custom_rule")
        self.assertNotIn("depends_on", added)
        self.assertEqual(added.get("inputs", {}), {})

        # 拒绝重复名称
        req2 = {
            "pipeline": res["pipeline"],
            "operation": {'kind': 'add_node', 'type': 'text_rule_match', 'name': 'custom_rule'},
        }
        code2, res2 = self.run_edit(req2)
        self.assertEqual(code2, 1)
        self.assertFalse(res2["ok"])
        self.assertIn("DUPLICATE_NODE_NAME", res2["diagnostics"][0]["message"])

        # 未知 Node 类型
        req_unknown = {
            "pipeline": res["pipeline"],
            "operation": {'kind': 'add_node', 'type': 'non_existent'},
        }
        code_unknown, result_unknown = self.run_edit(req_unknown)
        self.assertEqual(code_unknown, 1)
        self.assertFalse(result_unknown["ok"])
        self.assertIn("UNKNOWN_NODE_TYPE", result_unknown["diagnostics"][0]["message"])

    def test_authoring_rename_rejects_duplicate_name(self):
        pipe = {"io": {"input": [{"type": "keyword_in", "name": "keyword_match"}],
            "output": [{"type": "keyword_out", "name": "keyword_match", "inputs": {"matches": "rule.matches"}}]},
            "models": [], "pipeline": [
                {"name": "template", "type": "text_template", "inputs": {"primary": "input.sentence_text"}},
                {"name": "rule", "type": "text_rule_match", "depends_on": ["template"], "inputs": {"text": "template.text"}}]}
        code, duplicate = self.run_edit({"pipeline": pipe,
            "operation": {"kind": "rename_node", "node": "template", "new_name": "rule"}})
        self.assertEqual(code, 1)
        self.assertIn("DUPLICATE_NODE_NAME", duplicate["diagnostics"][0]["message"])

    def test_authoring_connect_and_disconnect_output(self):
        pipe = {"io": {"input": [{"type": "keyword_in", "name": "keyword_match"}],
            "output": [{"type": "keyword_out", "name": "keyword_match", "inputs": {"matches": "audit.matches"}}]},
            "models": [], "pipeline": [
                {"name": "rule", "type": "text_rule_match", "inputs": {"text": "input.sentence_text"}},
                {"name": "audit", "type": "text_rule_match", "depends_on": ["rule"], "inputs": {"text": "rule.matches"}}]}
        code, connected = self.run_edit({"pipeline": pipe,
            "operation": {"kind": "connect", "source": {"node": "rule", "port": "matches"},
                          "target": {"node": "output", "port": "matches"}}})
        self.assertEqual(code, 0, connected)
        self.assertEqual(connected["pipeline"]["io"]["output"][0]["inputs"], {"matches": "rule.matches"})
        self.assertEqual(connected["pipeline"]["pipeline"], pipe["pipeline"])
        code, disconnected = self.run_edit({"pipeline": connected["pipeline"],
            "operation": {"kind": "disconnect", "source": {"node": "rule", "port": "matches"},
                          "target": {"node": "output", "port": "matches"}}})
        self.assertEqual(code, 0, disconnected)
        self.assertNotIn("matches", disconnected["pipeline"]["io"]["output"][0]["inputs"])
        self.assertEqual(disconnected["pipeline"]["pipeline"], pipe["pipeline"], "output edits preserve independent consumers")

    def test_authoring_disconnect_preserves_extra_order_and_removes_input(self):
        pipe = {'io': {'input': [{'type': 'keyword_in', 'name': 'keyword_match'}], 'output': [{'type': 'keyword_out', 'name': 'keyword_match', 'inputs': {'matches': 'rule.matches'}}]}, 'models': [], 'pipeline': [{'name': 'tpl', 'type': 'text_template', 'depends_on': [], 'inputs': {}, 'params': {'template': '{{primary}}'}}, {'name': 'rule', 'type': 'text_rule_match', 'depends_on': ['tpl'], 'inputs': {'text': 'tpl.text'}, 'params': {}}]}
        # 断开数据端口时保留执行依赖 (回归用例 1)
        req = {
            "pipeline": pipe,
            "operation": {
                "kind": "disconnect",
                "source": {"node": "tpl", "port": "text"},
                "target": {"node": "rule", "port": "text"},
            },
        }
        code, res = self.run_edit(req)
        self.assertEqual(code, 0)
        rule = res["pipeline"]["pipeline"][1]
        self.assertEqual(rule["depends_on"], ["tpl"])
        self.assertNotIn("text", rule["inputs"])

        # remove_dependency 移除执行依赖
        req_rm_dep = {
            "pipeline": res["pipeline"],
            "operation": {
                "kind": "remove_dependency",
                "node": "rule",
                "depends_on": "tpl",
            },
        }
        code_rm_dep, res_rm_dep = self.run_edit(req_rm_dep)
        self.assertEqual(code_rm_dep, 0)
        self.assertEqual(res_rm_dep["pipeline"]["pipeline"][1]["depends_on"], [])

    def test_authoring_batch_operations_and_request_limit(self):
        pipe = {'io': {'input': [{'type': 'keyword_in', 'name': 'keyword_match'}], 'output': [{'type': 'keyword_out', 'name': 'keyword_match', 'inputs': {}}]}, 'models': [], 'pipeline': []}
        req_valid = {
            "pipeline": pipe,
            "operations": [
                {'kind': 'add_node', 'type': 'text_template', 'name': 't1', 'params': {'template': '{{primary}}'}},
                {'kind': 'add_node', 'type': 'text_rule_match', 'name': 'r1', 'params': {'categories': {'K': ['v']}}},
                {"kind": "connect", "source": {"node": "input", "port": "sentence_text"}, "target": {"node": "t1", "port": "primary"}},
                {"kind": "connect", "source": {"node": "t1", "port": "text"}, "target": {"node": "r1", "port": "text"}},
                {"kind": "connect", "source": {"node": "r1", "port": "matches"}, "target": {"node": "output", "port": "matches"}},
            ],
            "require_valid": True,
        }
        code_valid, res_valid = self.run_edit(req_valid)
        self.assertEqual(code_valid, 0)
        self.assertTrue(res_valid["ok"])
        self.assertTrue(res_valid["validation"]["ok"])
        self.assertEqual(len(res_valid["pipeline"]["pipeline"]), 2)

        # 批大小超过 128
        req_oversize = {
            "pipeline": pipe,
            "operations": [{'kind': 'add_node', 'type': 'text_template'}] * 129,
        }
        code_oversize, res_oversize = self.run_edit(req_oversize)
        self.assertEqual(code_oversize, 1)
        self.assertFalse(res_oversize["ok"])

    def test_authoring_rename_and_reuse_name_preserves_distinct_references(self):
        pipe = {"io": {"input": [{"type": "keyword_in", "name": "keyword_match"}],
            "output": [{"type": "keyword_out", "name": "keyword_match", "inputs": {}}]}, "models": [], "pipeline": []}
        operations = [
            {"kind": "add_node", "type": "text_rule_match", "name": "foo"},
            {"kind": "rename_node", "node": "foo", "new_name": "bar"},
            {"kind": "add_node", "type": "text_rule_match", "name": "foo"},
            {"kind": "connect", "source": {"node": "input", "port": "sentence_text"}, "target": {"node": "bar", "port": "text"}},
            {"kind": "connect", "source": {"node": "input", "port": "sentence_text"}, "target": {"node": "foo", "port": "text"}},
            {"kind": "connect", "source": {"node": "bar", "port": "matches"}, "target": {"node": "output", "port": "matches"}}]
        code, result = self.run_edit({"pipeline": pipe, "operations": operations, "require_valid": True})
        self.assertEqual(code, 0, result)
        self.assertTrue(result["validation"]["ok"])
        self.assertEqual([node["name"] for node in result["pipeline"]["pipeline"]], ["bar", "foo"])
        self.assertEqual(result["pipeline"]["io"]["output"][0]["inputs"]["matches"], "bar.matches")
        self.assertEqual(result["pipeline"]["pipeline"][1]["inputs"]["text"], "input.sentence_text")


    def test_studio_authoring_preview_endpoint(self):
        pipe = {'io': {'input': [{'type': 'keyword_in', 'name': 'keyword_match'}], 'output': [{'type': 'keyword_out', 'name': 'keyword_match', 'inputs': {}}]}, 'models': [], 'pipeline': []}
        res = self.service.preview_authoring(
            pipe,
            operation={'kind': 'add_node', 'type': 'text_rule_match', 'name': 'n1'},
            expected_revision=SHOW.revision_for(json.dumps(pipe, sort_keys=True).encode()),
            tool_fingerprint=self.service.get_tool_fingerprint(),
        )
        self.assertTrue(res["ok"])
        self.assertIn("revision", res)
        self.assertIn("tool_fingerprint", res)
        self.assertEqual(res["pipeline"]["pipeline"][0]["name"], "n1")

        # 过期 revision 冲突
        with self.assertRaises(SHOW.StudioError) as ctx:
            self.service.preview_authoring(
                pipe,
                operation={'kind': 'add_node', 'type': 'text_rule_match'},
                expected_revision="wrong_rev",
                tool_fingerprint=self.service.get_tool_fingerprint(),
            )
        self.assertEqual(ctx.exception.code, "REVISION_CONFLICT")

    def test_studio_authoring_requires_current_revision_and_tool(self):
        revision = SHOW.revision_for(json.dumps(self.keyword, sort_keys=True).encode())
        fingerprint = self.service.get_tool_fingerprint()
        for expected, tool, error in [
            (None, fingerprint, "REVISION_CONFLICT"),
            (revision, None, "TOOL_OUTDATED"),
            (revision, "outdated", "TOOL_OUTDATED"),
        ]:
            with self.subTest(revision=expected, fingerprint=tool):
                with mock.patch.object(self.service, "invoke_tool") as invoke:
                    with self.assertRaises(SHOW.StudioError) as ctx:
                        self.service.preview_authoring(
                            self.keyword, operation={'kind': 'add_node', 'type': 'text_template'},
                            expected_revision=expected, tool_fingerprint=tool,
                        )
                    self.assertEqual(ctx.exception.code, error)
                    invoke.assert_not_called()

    def test_studio_preflight_independent_summary(self):
        preflight_res = self.service.preflight(
            self.keyword,
            profile_name="keyword_match_rules",
        )
        self.assertTrue(preflight_res["ok"])
        summary = preflight_res["summary"]
        self.assertEqual(summary["io"], self.service.io_pairs(self.keyword))
        self.assertIn("tools", summary)
        self.assertIn("pipeline_snapshot", summary)
        self.assertIn("status", summary)
        # 验证没有写入任何用户文件
        self.assertFalse((self.configs / "pipeline_preflight_leak.json").exists())

    def test_preflight_snapshots_keep_the_pipeline_directory_and_clean_only_owned_files(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        resource = self.configs / pipeline["models"][0]["file"]
        resource.parent.mkdir(parents=True)
        resource.write_bytes(b"neutral fixture")
        sentinel = self.configs / ".studio-preflight-unrelated.json"
        sentinel.write_text("keep")
        observations = []
        resolve = self.service.resolve_run_conf

        def observe(conf_file, profile):
            conf_path = Path(conf_file)
            conf = json.loads(conf_path.read_text())
            pipeline_path = conf_path.parent / conf["pipe_path"]
            observations.append((pipeline_path, conf_path))
            if conf_path.name.startswith(".studio-preflight-"):
                self.assertEqual(conf_path.parent, self.configs)
                self.assertEqual(pipeline_path.parent, self.configs)
                self.assertEqual(json.loads(pipeline_path.read_text()), pipeline)
            return resolve(conf_file, profile)

        with mock.patch.object(self.service, "resolve_run_conf", side_effect=observe):
            preview = self.service.preflight(pipeline, profile_name="entity_extract_mock")
        # The Profile's own conf is resolved before the owned preview snapshot.
        snapshots = [(pipe, conf) for pipe, conf in observations if conf.name.startswith(".studio-preflight-")]
        self.assertEqual(len(snapshots), 1)
        self.assertEqual(preview["configuration"]["model_files"][0]["resolved"], str(resource))
        self.assertEqual(preview["summary"]["assets"][0]["status"], "exists_unverified")
        self.assertTrue(all(not path.exists() for pair in snapshots for path in pair))
        self.assertEqual(sentinel.read_text(), "keep")

        def reject_snapshot(conf_file, profile):
            if Path(conf_file).name.startswith(".studio-preflight-"):
                observations.append((Path(conf_file).parent / json.loads(Path(conf_file).read_text())["pipe_path"], Path(conf_file)))
                raise SHOW.StudioError("DEPLOYMENT_CONFIG", "simulated native failure")
            return resolve(conf_file, profile)

        with mock.patch.object(self.service, "resolve_run_conf", side_effect=reject_snapshot), self.assertRaises(SHOW.StudioError):
            self.service.preflight(pipeline, profile_name="entity_extract_mock")
        self.assertTrue(all(not path.exists() for pair in observations
                            if pair[1].name.startswith(".studio-preflight-") for path in pair))
        self.assertEqual(set(self.configs.iterdir()), {resource.parent, sentinel})
        self.assertEqual(sentinel.read_text(), "keep")

    def associated_doc_qa(self):
        pipeline = json.loads((ROOT / "configs" / "pipeline_doc_qa_cpu.json").read_text())
        conf = json.loads((ROOT / "configs" / "pipeline_doc_qa_cpu.conf").read_text())
        pipeline_path = self.configs / "pipeline_associated.json"
        conf_path = self.configs / "pipeline_associated.conf"
        conf["pipe_path"] = pipeline_path.name
        for model in pipeline.get("models", []):
            model["file"] = "deployed_" + model["name"]
        output_params(pipeline, "doc_out")["answer_text_max_bytes"] = 2047
        pipeline_path.write_text(json.dumps(pipeline))
        conf_path.write_text(json.dumps(conf))
        self.service.associate_deployment(pipeline_path.name, conf_path.name)
        return pipeline, conf, pipeline_path, conf_path

    def test_associated_preflight_run_and_save_share_candidate(self):
        pipeline, original_conf, path, conf_path = self.associated_doc_qa()
        pipeline["models"][0]["file"] = "selected_A.onnx"
        expected_conf = {"pipe_path": path.name}
        # 监视真实的原生解析，确保检查的正是预检所解析的内容。
        resolved_candidates = []
        resolve = self.service.resolve_run_conf

        def capture_resolve(conf_file, profile):
            resolved_candidates.append(json.loads(Path(conf_file).read_text()))
            return resolve(conf_file, profile)

        with mock.patch.object(self.service, "resolve_run_conf", side_effect=capture_resolve):
            preview = self.service.preflight(
                pipeline, filename=path.name,
            )
        self.assertTrue(preview["ok"], preview)
        self.assertEqual(len(resolved_candidates), 1)
        self.assertEqual(preview["configuration"]["model_files"][0]["resolved"], str(self.configs / "selected_A.onnx"))
        self.assertEqual(preview["configuration"]["model_files"][0]["path"], "/models/0/file")
        with mock.patch.object(SHOW.threading, "Thread") as thread:
            self.service.start_run(pipeline, "doc_qa_cpu", filename=path.name)
            args = thread.call_args.kwargs["args"]
            run_conf = args[3]
            self.assertEqual(args[1]["models"], pipeline["models"])
        self.service.save_pipeline(
            path.name, pipeline, SHOW.revision_for(path.read_bytes()),
        )
        saved_conf = json.loads(conf_path.read_text())
        self.assertEqual(saved_conf, expected_conf)
        saved_pipe = json.loads(path.read_text())
        self.assertEqual(saved_pipe["models"][0]["file"], "selected_A.onnx")
        self.assertEqual(run_conf, expected_conf)
        self.assertTrue(resolved_candidates[0]["pipe_path"].startswith(".studio-preflight-"))
        self.assertFalse((self.configs / resolved_candidates[0]["pipe_path"]).exists())

    def test_associated_raw_model_edit_is_the_effective_path(self):
        pipeline, conf, path, _ = self.associated_doc_qa()
        pipeline["models"][0]["file"] = "raw_edit.onnx"
        _, candidate = self.service.deployment_candidate(pipeline, filename=path.name)
        self.assertEqual(pipeline["models"][0]["file"], "raw_edit.onnx")
        self.assertEqual(candidate, {"pipe_path": path.name})
        self.assertEqual(output_params(pipeline, "doc_out")["answer_text_max_bytes"], 2047)

    def test_associated_new_model_does_not_invent_deployment_override(self):
        pipeline, conf, path, _ = self.associated_doc_qa()
        new_model = copy.deepcopy(pipeline["models"][0])
        new_model.update(name="new_model", file="new.onnx")
        pipeline["models"].append(new_model)
        _, candidate = self.service.deployment_candidate(pipeline, filename=path.name)
        self.assertEqual(pipeline["models"][-1]["file"], "new.onnx")

    def test_associated_external_file_changes_block_candidate_and_save(self):
        for changed_name in ["pipeline", "conf"]:
            with self.subTest(changed=changed_name):
                pipeline, _, path, conf_path = self.associated_doc_qa()
                revision = SHOW.revision_for(path.read_bytes())
                changed = path if changed_name == "pipeline" else conf_path
                changed.write_bytes(changed.read_bytes() + b"\n")
                originals = (path.read_bytes(), conf_path.read_bytes())
                with self.assertRaises(SHOW.StudioError) as ctx:
                    self.service.deployment_candidate(pipeline, filename=path.name)
                self.assertEqual(ctx.exception.code, "REVISION_CONFLICT")
                with self.assertRaises(SHOW.StudioError) as ctx:
                    self.service.save_pipeline(path.name, pipeline, revision)
                self.assertEqual(ctx.exception.code, "REVISION_CONFLICT")
                self.assertEqual((path.read_bytes(), conf_path.read_bytes()), originals)

    def test_studio_deployment_associate_and_model_file_update(self):
        # 在 configs 中创建 Pipeline
        pipe_path = self.configs / "pipeline_doc_qa_assoc.json"
        doc_qa_pipe = json.loads((ROOT / "configs" / "pipeline_doc_qa_cpu.json").read_text())
        output_params(doc_qa_pipe, "doc_out")["answer_text_max_bytes"] = 2047
        pipe_path.write_text(json.dumps(doc_qa_pipe, indent=2))

        # 在 configs 中创建指向该 Pipeline 的 conf
        conf_path = self.configs / "pipeline_doc_qa_assoc.conf"
        doc_qa_conf = {"pipe_path": pipe_path.name}
        conf_path.write_text(json.dumps(doc_qa_conf, indent=2))

        assoc_res = self.service.associate_deployment(
            "pipeline_doc_qa_assoc.json", "pipeline_doc_qa_assoc.conf"
        )
        self.assertTrue(assoc_res["ok"])
        self.assertEqual(assoc_res["conf_name"], "pipeline_doc_qa_assoc.conf")
        self.assertIn("pipeline_doc_qa_assoc.conf", self.service.save_targets(pipe_path))

        # 更新 Pipeline 模型并保存
        modified_pipe = copy.deepcopy(doc_qa_pipe)
        # 更新第一个模型的文件路径
        modified_pipe["models"][0]["file"] = "new_embed_model.onnx"
        pipe_raw = pipe_path.read_bytes()
        pipe_rev = SHOW.revision_for(pipe_raw)

        save_res = self.service.save_pipeline(
            "pipeline_doc_qa_assoc.json", modified_pipe, pipe_rev, save_as=False,
        )
        self.assertTrue(save_res["ok"])

        # 验证模型条目持有有效路径
        updated_pipe = json.loads(pipe_path.read_text())
        self.assertEqual(
            updated_pipe["models"][0]["file"],
            "new_embed_model.onnx",
        )
        # 验证 conf 中与模型无关的设置保持不变
        self.assertEqual(output_params(updated_pipe, "doc_out"),
                         {"answer_text_max_bytes": 2047})
        updated_conf = json.loads(conf_path.read_text())
        self.assertEqual(updated_conf, {"pipe_path": pipe_path.name})

    def test_authoring_oversized_payload_rejection_4mib(self):
        pipe = {'io': {'input': [{'type': 'keyword_in', 'name': 'keyword_match'}], 'output': [{'type': 'keyword_out', 'name': 'keyword_match', 'inputs': {}}]}, 'models': [], 'pipeline': []}
        large_comment = "x" * (4 * 1024 * 1024 + 100)
        req = {
            "pipeline": pipe,
            "operation": {'kind': 'add_node', 'type': 'text_rule_match'},
            "comment": large_comment,
        }
        code, res = self.run_edit(req)
        self.assertEqual(code, 1)
        self.assertFalse(res["ok"])
        self.assertIn("REQUEST_TOO_LARGE", res["diagnostics"][0]["message"])

    def test_authoring_reserved_node_names_rejected(self):
        pipe = {'io': {'input': [{'type': 'keyword_in', 'name': 'keyword_match'}], 'output': [{'type': 'keyword_out', 'name': 'keyword_match', 'inputs': {}}]}, 'models': [], 'pipeline': []}
        req_add_ingress = {
            "pipeline": pipe,
            "operation": {'kind': 'add_node', 'type': 'text_rule_match', 'name': 'input'},
        }
        code1, res1 = self.run_edit(req_add_ingress)
        self.assertEqual(code1, 1)
        self.assertFalse(res1["ok"])
        self.assertIn("INVALID_NODE_NAME", res1["diagnostics"][0]["message"])

        pipe2 = {'io': {'input': [{'type': 'keyword_in', 'name': 'keyword_match'}], 'output': [{'type': 'keyword_out', 'name': 'keyword_match', 'inputs': {'matches': 'node_a.matches'}}]}, 'models': [], 'pipeline': [{'name': 'node_a', 'type': 'text_rule_match', 'depends_on': [], 'inputs': {}, 'params': {}}]}
        req_rename_egress = {
            "pipeline": pipe2,
            "operation": {"kind": "rename_node", "node": "node_a", "new_name": "output"},
        }
        code2, res2 = self.run_edit(req_rename_egress)
        self.assertEqual(code2, 1)
        self.assertFalse(res2["ok"])
        self.assertIn("INVALID_NODE_NAME", res2["diagnostics"][0]["message"])

    def test_authoring_disconnect_requires_explicit_binding(self):
        # 同名输出加显式顺序不得产生输入绑定。
        pipe = {'io': {'input': [{'type': 'keyword_in', 'name': 'keyword_match'}], 'output': [{'type': 'keyword_out', 'name': 'keyword_match', 'inputs': {'matches': 'rule.matches'}}]}, 'models': [], 'pipeline': [{'name': 'tpl', 'type': 'text_template', 'depends_on': [], 'inputs': {}, 'params': {'template': '{{primary}}'}}, {'name': 'rule', 'type': 'text_rule_match', 'depends_on': ['tpl'], 'inputs': {}, 'params': {}}]}
        req_disc = {
            "pipeline": pipe,
            "operation": {
                "kind": "disconnect",
                "source": {"node": "tpl", "port": "text"},
                "target": {"node": "rule", "port": "text"},
            },
        }
        code, res = self.run_edit(req_disc)
        self.assertEqual(code, 1)
        self.assertFalse(res["ok"])
        self.assertIn("连线不存在", res["diagnostics"][0]["message"])

        # 断开入口连接
        pipe_ing = {'io': {'input': [{'type': 'keyword_in', 'name': 'keyword_match'}], 'output': [{'type': 'keyword_out', 'name': 'keyword_match', 'inputs': {'matches': 'rule.matches'}}]}, 'models': [], 'pipeline': [{'name': 'rule', 'type': 'text_rule_match', 'depends_on': [], 'inputs': {'text': 'input.sentence_text'}, 'params': {}}]}
        req_ing_disc = {
            "pipeline": pipe_ing,
            "operation": {
                "kind": "disconnect",
                "source": {"node": "input", "port": "sentence_text"},
                "target": {"node": "rule", "port": "text"},
            },
        }
        code_ing, res_ing = self.run_edit(req_ing_disc)
        self.assertEqual(code_ing, 0)
        self.assertTrue(res_ing["ok"])
        self.assertNotIn("text", res_ing["pipeline"]["pipeline"][0]["inputs"])

    def test_authoring_remove_node_handles_consumers_and_egress(self):
        pipe = {'io': {'input': [{'type': 'keyword_in', 'name': 'keyword_match'}], 'output': [{'type': 'keyword_out', 'name': 'keyword_match', 'inputs': {'matches': 'rule.matches'}}]}, 'models': [], 'pipeline': [{'name': 'tpl', 'type': 'text_template', 'depends_on': [], 'inputs': {}, 'params': {'template': '{{primary}}'}}, {'name': 'rule', 'type': 'text_rule_match', 'depends_on': ['tpl'], 'inputs': {'text': 'tpl.text'}, 'params': {}}]}
        req = {
            "pipeline": pipe,
            "operation": {"kind": "remove_node", "node": "rule"},
        }
        code, res = self.run_edit(req)
        self.assertEqual(code, 0)
        self.assertTrue(res["ok"])
        change = res["changes"][0]
        self.assertIn("output", change["affected_nodes"])

    def test_authoring_explicit_indirect_dependency_preserves_topology_and_is_idempotent(self):
        pipe = copy.deepcopy(self.keyword)
        rule = pipe["pipeline"][0]
        rule["name"] = "c"
        rule["inputs"] = {"text": "b.text"}
        rule["depends_on"] = ["b"]
        pipe["pipeline"] = [
            {"name": "a", "type": "text_template", "inputs": {"primary": "input.sentence_text"}},
            {"name": "b", "type": "text_template", "depends_on": ["a"], "inputs": {"primary": "a.text"}},
            rule]
        pipe["io"]["output"][0]["inputs"] = {"matches": "c.matches"}
        plan_before = self.service.invoke_tool(["plan", "--stdin"], pipe)
        self.assertTrue(plan_before["ok"], plan_before)
        operation = {"kind": "add_dependency", "node": "c", "depends_on": "a"}
        code, result = self.run_edit({"pipeline": pipe, "require_valid": True, "operation": operation})
        self.assertEqual(code, 0, result)
        self.assertTrue(result["validation"]["ok"], result)
        self.assertEqual(result["pipeline"]["pipeline"][2]["depends_on"], ["b", "a"])
        plan_after = self.service.invoke_tool(["plan", "--stdin"], result["pipeline"])
        self.assertTrue(plan_after["ok"], plan_after)
        self.assertEqual(plan_after["plan"], plan_before["plan"])
        code, repeated = self.run_edit({"pipeline": result["pipeline"], "require_valid": True,
                                        "operation": operation})
        self.assertEqual(code, 0, repeated)
        self.assertTrue(repeated["validation"]["ok"], repeated)
        self.assertEqual(repeated["pipeline"], result["pipeline"])


if __name__ == "__main__":
    unittest.main()
