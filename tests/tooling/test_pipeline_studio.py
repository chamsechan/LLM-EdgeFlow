#!/usr/bin/env python3
"""API and filesystem boundary tests for the local Pipeline Studio."""

import copy
import importlib.util
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
ALG_SHOW = Path(
    os.environ.get("LLM_EDGEFLOW_ALG_SHOW", ROOT / "build" / "alg_show")
)
STUDIO_SERVER = ROOT / "tools" / "pipeline_studio" / "server.py"
WEB_ROOT = ROOT / "tools" / "pipeline_studio" / "web"
SPEC = importlib.util.spec_from_file_location("edgeflow_show", STUDIO_SERVER)
SHOW = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SHOW)
SHOW.PIPELINE_TOOL = PIPELINE_TOOL


def io_overrides(pipeline):
    return pipeline.setdefault("deployment", {}).setdefault("io", {})


def output_override(pipeline, slot):
    return io_overrides(pipeline).setdefault("out_mem", {}).setdefault(slot, {})


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
        invalid = {"deployment": {"io": {"io_binding": "keyword_match.operator.v1"}}, "pipeline": []}
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
        with mock.patch("builtins.print") as output:
            SHOW.render_terminal(Path("pipeline.json"), {"pipeline": [
                {"id": "consumer", "node_type": "Consumer", "inputs": {"text": "shared"}},
                {"id": "producer", "node_type": "Producer", "outputs": {"text": "shared"}},
            ]})
        rendered = "\n".join(call.args[0] for call in output.call_args_list if call.args)
        self.assertIn('inputs: {"text": "shared"}', rendered)
        self.assertIn('outputs: {"text": "shared"}', rendered)
        self.assertIn("outputs: <未声明>", rendered)
        self.assertEqual(rendered.count("depends_on: [] (额外顺序)"), 2)
        self.assertLess(rendered.index("consumer"), rendered.index("producer"))

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

        # Omitted expected_revision (None) raises REVISION_CONFLICT
        with self.assertRaises(SHOW.StudioError) as err_none:
            self.service.preview_fix(
                self.keyword, self.patch, expected_revision=None, tool_fingerprint=valid_fp
            )
        self.assertEqual(err_none.exception.code, "REVISION_CONFLICT")

        # Omitted expected_revision (empty string) raises REVISION_CONFLICT
        with self.assertRaises(SHOW.StudioError) as err_empty:
            self.service.preview_fix(
                self.keyword, self.patch, expected_revision="", tool_fingerprint=valid_fp
            )
        self.assertEqual(err_empty.exception.code, "REVISION_CONFLICT")

        # Mismatched expected_revision raises REVISION_CONFLICT
        with self.assertRaises(SHOW.StudioError) as err_mismatch:
            self.service.preview_fix(
                self.keyword, self.patch, expected_revision="mismatched_revision", tool_fingerprint=valid_fp
            )
        self.assertEqual(err_mismatch.exception.code, "REVISION_CONFLICT")

    def test_preview_fix_requires_tool_fingerprint(self):
        raw = json.dumps(self.keyword, sort_keys=True).encode("utf-8")
        valid_rev = SHOW.revision_for(raw)

        # Omitted tool_fingerprint (None) raises TOOL_OUTDATED
        with self.assertRaises(SHOW.StudioError) as err_none:
            self.service.preview_fix(
                self.keyword, self.patch, expected_revision=valid_rev, tool_fingerprint=None
            )
        self.assertEqual(err_none.exception.code, "TOOL_OUTDATED")

        # Omitted tool_fingerprint (empty string) raises TOOL_OUTDATED
        with self.assertRaises(SHOW.StudioError) as err_empty:
            self.service.preview_fix(
                self.keyword, self.patch, expected_revision=valid_rev, tool_fingerprint=""
            )
        self.assertEqual(err_empty.exception.code, "TOOL_OUTDATED")

        # Mismatched tool_fingerprint raises TOOL_OUTDATED
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

        bad_patch = [{"op": "test", "path": "/deployment/io/io_binding", "value": "mismatched_binding"}]
        with self.assertRaises(SHOW.StudioError) as patch_err:
            self.service.preview_fix(
                self.keyword,
                bad_patch,
                expected_revision=valid_rev,
                tool_fingerprint=valid_fp,
            )
        self.assertEqual(patch_err.exception.code, "PATCH_APPLICATION_FAILED")


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
        self.keyword["pipeline"][0]["config"]["categories"] = {"SAVED_RULE": ["VIP"]}
        filename = f"pipeline_saved_{uuid.uuid4().hex}.json"
        saved = self.service.save_solution(filename, self.keyword, "keyword_match_rules")
        command = shlex.split(saved["command"])
        output = Path(command[command.index("--output-dir") + 1])
        try:
            process = subprocess.run(command[3:], cwd=command[1], text=True, capture_output=True, timeout=30)
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            records = [json.loads(line) for line in (output / "keyword_match_v1/results.jsonl").read_text().splitlines()]
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

    def test_conf_preserves_single_model_path_without_adding_model_root(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        selected = pipeline["models"][0]["model_path"]
        saved = self.service.save_solution("pipeline_fixture.json", pipeline, "entity_extract_mock", ".")
        self.assertEqual(saved["pipeline"]["models"][0]["model_path"], selected)
        pipeline["models"][0]["model_path"] = "models/replacement.gguf"
        saved = self.service.save_solution("pipeline_replaced.json", pipeline, "entity_extract_mock", "models")
        self.assertEqual(saved["pipeline"]["models"][0]["model_path"], "models/replacement.gguf")
        self.assertNotIn("model_paths", saved["pipeline"]["deployment"])
        self.assertEqual(json.loads((self.configs / "pipeline_replaced.json").read_text()), pipeline)

    def test_ordinary_save_updates_managed_model_paths_and_node_parameters(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        saved = self.service.save_solution("pipeline_paired.json", pipeline, "entity_extract_mock", "models")
        pipeline["models"][0]["model_path"] = "models/replacement.gguf"
        pipeline["models"][0]["model_id"] = "replacement_model"
        pipeline["pipeline"][1]["config"]["bind_model"] = "replacement_model"
        pipeline["pipeline"][1]["config"]["max_tokens"] = 17
        updated = self.service.save_pipeline(
            saved["filename"], pipeline, saved["revision"],
        )
        self.assertEqual(updated["command"], saved["command"])
        self.assertEqual(updated["pipeline"]["models"][0]["model_path"], "models/replacement.gguf")
        self.assertNotIn("model_paths", updated["pipeline"]["deployment"])
        self.assertEqual(json.loads((self.configs / saved["filename"]).read_text()), pipeline)
        profile, _ = self.service.profile_inputs(pipeline, "entity_extract_mock")
        effective = self.service.resolve_run_conf(self.configs / saved["conf_filename"], profile)
        self.assertEqual(effective, updated["configuration"])
        self.assertEqual(effective["model_paths"][0]["resolved"], str(ROOT / "models/replacement.gguf"))
        self.assertEqual(effective["effective_pipeline"]["pipeline"][1]["config"]["max_tokens"], 17)
        self.assertEqual(sorted(p.name for p in self.configs.iterdir()), ["pipeline_paired.conf", "pipeline_paired.json"])

    def test_managed_save_checks_both_revisions_without_overwriting_external_edits(self):
        saved = self.service.save_solution("pipeline_revision.json", self.keyword, "keyword_match_rules")
        paths = [self.configs / saved["filename"], self.configs / saved["conf_filename"]]
        originals = {path: path.read_bytes() for path in paths}
        self.keyword["pipeline"][0]["config"]["categories"] = {"NEW": ["sample"]}
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
        self.keyword["pipeline"][0]["config"]["categories"] = {"NEW": ["sample"]}
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
        # A recovered failure must not advance either revision.
        self.assertTrue(self.service.save_pipeline(saved["filename"], self.keyword, saved["revision"])["ok"])

    def test_restarted_service_saves_model_path_without_shadowed_values(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        saved = self.service.save_solution("pipeline_restart.json", pipeline, "entity_extract_mock", ".")
        restarted = SHOW.WorkbenchService(self.configs)
        pipeline["models"][0]["model_path"] = "models/replacement.gguf"
        updated = restarted.save_pipeline(saved["filename"], pipeline, saved["revision"])
        self.assertEqual(updated["pipeline"]["models"][0]["model_path"], "models/replacement.gguf")
        self.assertNotIn("model_paths", updated["pipeline"]["deployment"])
        self.assertEqual(json.loads((self.configs / saved["filename"]).read_text()), pipeline)
        self.assertFalse(restarted.generated_solutions)

    def test_draft_uses_the_same_selected_paths_under_project_root_and_cleans_up(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        observed = {}
        original_popen = SHOW.subprocess.Popen
        def inspect_launch(args, **kwargs):
            if "--profiles-file" in args:
                document = json.loads(Path(args[args.index("--profiles-file") + 1]).read_text())
                profile = document["profiles"][args[args.index("--profile") + 1]]
                conf_path = ROOT / profile["config"]
                observed["directory"] = conf_path.parent
                observed["conf"] = json.loads(conf_path.read_text())
                observed["pipeline"] = json.loads((conf_path.parent / "pipeline.json").read_text())
            return original_popen(args, **kwargs)
        with mock.patch.object(SHOW.subprocess, "Popen", side_effect=inspect_launch):
            started = self.service.start_run(pipeline, "entity_extract_mock", ".")
            for _ in range(200):
                job = self.service.run_status(started["job_id"])["job"]
                if job["status"] in ("completed", "failed", "cancelled") and "directory" in observed and not observed["directory"].exists():
                    break
                time.sleep(0.05)
        self.assertEqual(job["status"], "completed", job)
        self.assertEqual(observed["pipeline"]["models"], pipeline["models"])
        self.assertNotIn("model_paths", observed["pipeline"]["deployment"])
        self.assertEqual(observed["conf"]["pipe_path"], "pipeline.json")
        self.assertFalse(observed["directory"].exists())

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
        for filename, profile, model_root in (
            ("../pipeline_escape.json", "keyword_match_rules", "models"),
            ("pipeline_bad.json", "entity_extract_mock", "models"),
            ("pipeline_bad.json", "keyword_match_rules", "../outside"),
            ("pipeline_bad.json", "keyword_match_rules", str(ROOT / "models")),
        ):
            with self.subTest(filename=filename, profile=profile, model_root=model_root), self.assertRaises(SHOW.StudioError):
                self.service.save_solution(filename, self.keyword, profile, model_root)
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
        profile, outputs = self.service.profile_inputs(self.keyword, "keyword_match_rules")
        outputs["keyword_out"] = {"capacities": {"match_result_json": 0}}
        with mock.patch.object(self.service, "profile_inputs", return_value=(profile, outputs)):
            with self.assertRaises(SHOW.StudioError) as error:
                self.service.save_solution("pipeline_invalid_pool.json", self.keyword, "keyword_match_rules")
        self.assertEqual(error.exception.code, "DEPLOYMENT_VALIDATION_FAILED")
        self.assertIn("match_result_json", str(error.exception))
        self.assertEqual(list(self.configs.iterdir()), [])

    def test_profile_preserves_explicit_binding_selection(self):
        pipeline = copy.deepcopy(self.keyword)
        saved = self.service.save_solution("pipeline_profile_binding.json", pipeline, "keyword_match_rules")
        self.assertEqual(saved["pipeline"]["deployment"]["io"]["io_binding"], "keyword_match.operator.v1")
        self.assertNotIn("biz_name", saved["pipeline"])
        self.assertEqual(saved["configuration"]["io_binding"], "keyword_match.operator.v1")
        for binding in ("", "unknown.operator.v1", "entity_extract.operator.v1"):
            with self.subTest(binding=binding):
                pipeline = copy.deepcopy(self.keyword)
                io_overrides(pipeline)["io_binding"] = binding
                with self.assertRaises(SHOW.StudioError):
                    self.service.save_solution("pipeline_bad_binding.json", pipeline, "keyword_match_rules")
                self.assertEqual(pipeline["deployment"]["io"]["io_binding"], binding)
                self.assertFalse((self.configs / "pipeline_bad_binding.json").exists())
                self.assertFalse((self.configs / "pipeline_bad_binding.conf").exists())


class PipelineCliTest(unittest.TestCase):
    CLI_PARITY_ENTRYPOINTS = [
        ("validate",),
        ("plan",),
        ("validate", "--explain"),
    ]

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
        expected_schema = 4 if args[0] == "catalog" and payload.get("ok") else (3 if args[0] == "describe-node" and payload.get("ok") else 1)
        self.assertEqual(payload["schema_version"], expected_schema)
        return process.returncode, payload

    def test_removed_dependency_repair_command_is_rejected_without_writing(self):
        original = (ROOT / "configs/pipeline_keyword_match_rules.json").read_bytes()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pipeline.json"
            path.write_bytes(original)
            result = subprocess.run(
                [str(PIPELINE_TOOL), "fix-deps", str(path), "--in-place"],
                text=True, capture_output=True, cwd=ROOT, check=False,
            )
            self.assertEqual(result.returncode, 2)
            self.assertEqual(path.read_bytes(), original)
            self.assertNotIn("fix-deps", result.stderr)

    def test_explicit_binding_uses_native_required_output_defaults(self):
        original = json.loads((ROOT / "configs/pipeline_keyword_match_rules.json").read_text())
        original.pop("deployment", None)
        variants = [{"io": {"io_binding": "keyword_match.operator.v1"}},
                    {"io": {"io_binding": "keyword_match.operator.v1", "out_mem": {}}}]
        for deployment in variants:
            with self.subTest(deployment=deployment), tempfile.TemporaryDirectory() as directory:
                pipeline = copy.deepcopy(original)
                if deployment is not None:
                    pipeline["deployment"] = deployment
                before = json.dumps(pipeline)
                code, validated = self.command("validate", "--stdin", input_pipeline=pipeline)
                self.assertEqual(code, 0, validated)
                path = Path(directory)
                (path / "pipeline.json").write_text(json.dumps(pipeline))
                (path / "pipeline.conf").write_text(json.dumps({"pipe_path": "pipeline.json"}))
                code, resolved = self.command("validate-io", str(path / "pipeline.conf"))
                self.assertEqual(code, 0, resolved)
                self.assertEqual(resolved["binding"]["biz_name"], "keyword_match_v1")
                self.assertEqual(resolved["binding"]["binding_id"], "keyword_match.operator.v1")
                self.assertEqual(set(resolved["output_pools"]), {"keyword_out"})
                self.assertEqual(resolved["output_pools"]["keyword_out"]["capacities"], {"match_result_json": 2047})
                self.assertEqual((path / "pipeline.json").read_text(), before)

    def test_external_selector_is_required_and_legacy_selectors_are_rejected(self):
        original = json.loads((ROOT / "configs/pipeline_keyword_match_rules.json").read_text())
        variants = [(None, "/deployment"), ({}, "/deployment/io"),
                    ({"io": {}}, "/deployment/io/io_binding"),
                    ({"io": {"io_binding": ""}}, "/deployment/io/io_binding")]
        for deployment, path in variants:
            pipeline = copy.deepcopy(original)
            pipeline.pop("deployment", None)
            if deployment is not None:
                pipeline["deployment"] = deployment
            for entrypoint in self.CLI_PARITY_ENTRYPOINTS:
                with self.subTest(deployment=deployment, entrypoint=entrypoint):
                    code, result = self.command(*entrypoint, "--stdin", input_pipeline=pipeline)
                    self.assertEqual(code, 1, result)
                    self.assertEqual(result["diagnostics"][0]["path"], path)
        for field, path in (("biz_name", "/biz_name"),
                            ("output_allocations", "/deployment/io/output_allocations")):
            pipeline = copy.deepcopy(original)
            if field == "biz_name":
                pipeline[field] = "keyword_match_v1"
            else:
                io_overrides(pipeline)[field] = {}
            code, result = self.command("validate", "--stdin", input_pipeline=pipeline)
            self.assertEqual(code, 1, result)
            self.assertEqual(result["diagnostics"][0]["path"], path)
        code, result = self.command("catalog", "--io-binding", "unknown.binding")
        self.assertEqual(code, 1, result)
        self.assertEqual(result["diagnostics"][0]["code"], "UNKNOWN_IO_BINDING")
        for command in ("catalog", "init"):
            process = subprocess.run([str(PIPELINE_TOOL), command, "--biz", "keyword_match_v1"],
                                     cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(process.returncode, 2, process.stdout)

    def test_all_commands_return_versioned_json(self):
        first_code, first = self.command("catalog", "--io-binding", "keyword_match.operator.v1")
        second_code, second = self.command("catalog", "--io-binding", "keyword_match.operator.v1")
        self.assertEqual((first_code, first), (second_code, second))
        self.assertTrue(first["nodes"])
        self.assertTrue(first["profiles"])
        self.assertTrue(all(profile["io_binding"] == "keyword_match.operator.v1" for profile in first["profiles"]))
        self.assertTrue(all("biz" not in profile and "pipeline_biz" not in profile for profile in first["profiles"]))
        self.assertTrue(all("demo_biz" not in biz for biz in first["bizs"]))
        code, described = self.command("describe-node", "TextRuleMatchNode")
        self.assertEqual(code, 0)
        self.assertEqual(described["node_type"], "TextRuleMatchNode")
        code, initialized = self.command(
            "init", "--io-binding", "keyword_match.operator.v1", "--empty"
        )
        self.assertEqual(code, 0)
        self.assertEqual(initialized["pipeline"]["pipeline"], [])
        self.assertEqual(initialized["pipeline"]["deployment"]["io"]["io_binding"], "keyword_match.operator.v1")
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
            (ROOT / "configs" / "pipeline_keyword_match_rules.json").read_text()
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

    def test_catalog_profile_identity_does_not_require_model_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "demo").mkdir()
            (root / "demo/profiles.json").write_text(json.dumps({"profiles": {
                "unavailable_models": {"config": "pipeline.conf", "dataset": "unused.txt"}
            }}))
            (root / "pipeline.conf").write_text(json.dumps({"pipe_path": "pipeline.json"}))
            pipeline = {"deployment": {"io": {"io_binding": "keyword_match.operator.v1"}},
                        "models": [{"model_type": "unregistered_in_this_build"}], "pipeline": []}
            (root / "pipeline.json").write_text(json.dumps(pipeline))
            process = subprocess.run([str(PIPELINE_TOOL), "catalog", "--io-binding", "keyword_match.operator.v1"],
                                     cwd=root, capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            profiles = json.loads(process.stdout)["profiles"]
            self.assertEqual([p["name"] for p in profiles], ["unavailable_models"])
            self.assertEqual(profiles[0]["io_binding"], "keyword_match.operator.v1")
            self.assertEqual(profiles[0]["biz_name"], "keyword_match_v1")

    def test_invalid_profile_fields_and_shapes_are_rejected_by_all_consumers(self):
        base = {"config": "unused.conf", "dataset": "unused.txt"}
        cases = [(dict(base, biz="keyword_match_v1"), "Unknown Profile field", "biz"),
                 (dict(base, batch_szie=1), "Unknown Profile field", "batch_szie"),
                 ([], "must be an object", None), (None, "must be an object", None)]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "demo").mkdir()
            profile_path = root / "demo/profiles.json"
            for profile, expected, field in cases:
                with self.subTest(profile=profile):
                    profile_path.write_text(json.dumps({"schema_version": 2, "profiles": {"invalid": profile}}))
                    for args in (("catalog",), ("init", "--io-binding", "keyword_match.operator.v1", "--profile", "invalid")):
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

    def test_describe_models_and_backends_match_catalog(self):
        code, catalog = self.command("catalog")
        self.assertEqual(code, 0)
        for command, collection, type_field in (
            ("describe-model", "models", "model_type"),
            ("describe-backend", "backends", "backend_type"),
        ):
            self.assertTrue(catalog[collection])
            for definition in catalog[collection]:
                with self.subTest(command=command, name=definition[type_field]):
                    code, described = self.command(command, definition[type_field])
                    self.assertEqual(code, 0)
                    self.assertEqual(described, {**definition, "schema_version": 1, "ok": True})

    def test_describe_models_and_backends_reject_unknown_names(self):
        for command, diagnostic in (
            ("describe-model", "UNKNOWN_MODEL_TYPE"),
            ("describe-backend", "UNKNOWN_BACKEND"),
        ):
            for name in ("not_registered", ""):
                with self.subTest(command=command, name=name):
                    code, described = self.command(command, name)
                    self.assertEqual(code, 1)
                    self.assertFalse(described["ok"])
                    self.assertEqual(described["diagnostics"], [{
                        "code": diagnostic, "path": "/", "message": name,
                        "severity": "error",
                    }])

    def test_describe_models_and_backends_require_one_argument(self):
        for command in ("describe-model", "describe-backend"):
            for arguments in ((), ("name", "extra"), ("name", "--raw")):
                with self.subTest(command=command, arguments=arguments):
                    process = subprocess.run(
                        [str(PIPELINE_TOOL), command, *arguments],
                        text=True, capture_output=True, cwd=ROOT, check=False,
                    )
                    self.assertEqual(process.returncode, 2)
                    self.assertEqual(process.stdout, "")
                    self.assertIn("Usage:", process.stderr)

    def test_init_raw_can_be_saved_and_validated_without_unwrapping(self):
        args = ["init", "--io-binding", "keyword_match.operator.v1", "--profile", "keyword_match_rules"]
        code, wrapped = self.command(*args)
        self.assertEqual(code, 0)
        raw = subprocess.run(
            [str(PIPELINE_TOOL), *args, "--raw"],
            text=True, capture_output=True, cwd=ROOT, check=False,
        )
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
        empty = subprocess.run(
            [str(PIPELINE_TOOL), "init", "--raw", "--io-binding", "keyword_match.operator.v1", "--empty"],
            text=True, capture_output=True, cwd=ROOT, check=False,
        )
        self.assertEqual(empty.returncode, 0, empty.stderr)
        self.assertEqual(json.loads(empty.stdout), {
            "deployment": {"io": {"io_binding": "keyword_match.operator.v1"}}, "models": [], "pipeline": []
        })

    def test_init_rejects_invalid_options(self):
        for options in (
            ["--profile", "keyword_match_rules", "--empty"],
            ["--profile"], ["--profile", "--raw"], ["--unknown"],
            ["--raw", "--raw"], ["--empty", "--empty"],
            ["--io-binding", "keyword_match.operator.v1"],
            ["--profile", "keyword_match_rules", "--profile", "keyword_match_rules"],
        ):
            with self.subTest(options=options):
                process = subprocess.run(
                    [str(PIPELINE_TOOL), "init", "--io-binding", "keyword_match.operator.v1", *options],
                    text=True, capture_output=True, cwd=ROOT, check=False,
                )
                self.assertEqual(process.returncode, 2)
                self.assertIn("Usage:", process.stderr)
                self.assertEqual(process.stdout, "")

    def test_resolve_conf_exposes_model_sources_defaults_and_native_pool_errors(self):
        # Resolver semantics must run in every backend variant, including Kite
        # and minimal builds where llama.cpp is deliberately unavailable.
        conf_path = ROOT / "demo/fixtures/mock/pipeline_entity_extract.conf"
        code, report = self.command("resolve-conf", str(conf_path.relative_to(ROOT)), "--root", str(ROOT), "--depth", "1")
        self.assertEqual(code, 0, report)
        configuration = report["configuration"]
        self.assertEqual(configuration["biz_name"], "entity_extract_v1")
        self.assertEqual(configuration["io_binding"], "entity_extract.operator.v1")
        self.assertNotIn("biz_name", configuration["effective_pipeline"])
        self.assertEqual(configuration["effective_pipeline"]["deployment"]["io"]["io_binding"], "entity_extract.operator.v1")
        self.assertEqual(configuration["effective_pipeline"]["models"][0]["model_path"], str(ROOT / "demo/fixtures/mock/artifacts/neutral-llm.fixture"))
        self.assertEqual(configuration["conf_path"], str(conf_path))
        self.assertEqual(configuration["model_paths"], [{
            "model_id": "entity_llm", "source": "pipeline.models.model_path",
            "resolved": str(ROOT / "demo/fixtures/mock/artifacts/neutral-llm.fixture"),
        }])
        llm_config = configuration["effective_pipeline"]["pipeline"][1]["config"]
        self.assertEqual(llm_config["max_tokens"], 64)
        self.assertEqual(llm_config["top_p"], 0.9, "omitted defaults must come from the native validated plan")
        conf = json.loads(conf_path.read_text())
        with tempfile.TemporaryDirectory(prefix="resolve-conf-", dir=ROOT / "build") as directory:
            changed = Path(directory) / "pipeline.conf"
            pipe_file = conf_path.with_name(conf["pipe_path"])
            pipe_doc = json.loads(pipe_file.read_text())
            self.assertNotIn("model_paths", pipe_doc["deployment"])
            (Path(directory) / conf["pipe_path"]).write_text(json.dumps(pipe_doc))
            changed.write_text(json.dumps(conf))
            code, direct = self.command("resolve-conf", str(changed.relative_to(ROOT)), "--root", str(ROOT))
            self.assertEqual(code, 0, direct)
            self.assertEqual(direct["configuration"]["model_paths"][0]["source"], "pipeline.models.model_path")
            output_override(pipe_doc, "entity_out")["capacities"] = {"entities_json": 0}
            (Path(directory) / conf["pipe_path"]).write_text(json.dumps(pipe_doc))
            code, rejected = self.command("resolve-conf", str(changed.relative_to(ROOT)), "--root", str(ROOT))
            self.assertEqual(code, 1)
            self.assertFalse(rejected["ok"])
            self.assertEqual(rejected["diagnostics"][0]["code"], "DEPLOYMENT_CONFIG")
            self.assertEqual(rejected["diagnostics"][0]["path"], "/")
            self.assertIn("entities_json", rejected["diagnostics"][0]["message"])

            io_overrides(pipe_doc)["out_mem"] = {}
            (Path(directory) / conf["pipe_path"]).write_text(json.dumps(pipe_doc))
            code, defaulted = self.command("resolve-conf", str(changed.relative_to(ROOT)), "--root", str(ROOT))
            self.assertEqual(code, 0, defaulted)
            self.assertEqual(defaulted["configuration"]["output_pools"]["entity_out"]["capacities"], {"entities_json": 2047})

            bad_conf = Path(directory) / "bad.conf"
            bad_conf.write_text("{}")
            code, bad_res = self.command("resolve-conf", str(bad_conf.relative_to(ROOT)), "--root", str(ROOT))
            self.assertEqual(code, 1)
            self.assertFalse(bad_res["ok"])
            self.assertEqual(bad_res["diagnostics"][0]["code"], "DEPLOYMENT_CONFIG")
            self.assertEqual(bad_res["diagnostics"][0]["path"], "/")
            self.assertIn("pipe_path", bad_res["diagnostics"][0]["message"])

            code, bad_res_root = self.command("resolve-conf", "bad.conf", "--root", str(directory))
            self.assertEqual(code, 1)
            self.assertFalse(bad_res_root["ok"])
            self.assertEqual(bad_res_root["diagnostics"][0]["code"], "DEPLOYMENT_CONFIG")
            self.assertEqual(bad_res_root["diagnostics"][0]["path"], "/")
            self.assertIn("pipe_path", bad_res_root["diagnostics"][0]["message"])

    def test_cli_model_path_is_required_and_typed(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        cases = [(None, "FIELD_TYPE"), ("", "FIELD_RANGE"), (12345, "FIELD_TYPE")]
        for value, expected_code in cases + [("missing", "MISSING_FIELD")]:
            document = copy.deepcopy(pipeline)
            if value == "missing":
                document["models"][0].pop("model_path")
            else:
                document["models"][0]["model_path"] = value
            for endpoint in self.CLI_PARITY_ENTRYPOINTS:
                with self.subTest(value=value, endpoint=endpoint):
                    code, report = self.command(*endpoint, "--stdin", input_pipeline=document)
                    self.assertEqual(code, 1)
                    self.assertEqual(report["diagnostics"][0]["code"], expected_code)
                    self.assertEqual(report["diagnostics"][0]["path"], "/models/0/model_path")
                    if endpoint[0] == "plan":
                        self.assertEqual(report["plan"], {"layers": [], "topological_order": []})

    def test_cli_plan_envelopes_across_entrypoints(self):
        # Plan returns envelope with diagnostics on deployment preparation failure,
        # partial plan on Core failure, and full topological order on success, with CLI parity.
        pipeline = json.loads(
            (ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text()
        )
        # 1. Removed deployment field is rejected
        invalid_doc = copy.deepcopy(pipeline)
        invalid_doc["deployment"]["model_paths"] = {"unknown_model_id": "models/foo.bin"}
        for ep in self.CLI_PARITY_ENTRYPOINTS:
            with self.subTest(case="deployment_failure", entrypoint=ep):
                code, res = self.command(*ep, "--stdin", input_pipeline=invalid_doc)
                self.assertEqual(code, 1)
                self.assertFalse(res["ok"])
                self.assertIn("diagnostics", res)
                self.assertEqual(res["diagnostics"][0]["code"], "DEPLOYMENT_ERROR")
                self.assertEqual(res["diagnostics"][0]["path"], "/deployment/model_paths")
                if ep[0] == "plan":
                    self.assertEqual(res["plan"], {"layers": [], "topological_order": []})

        # 2. Core failure with valid deployment
        invalid_core = copy.deepcopy(pipeline)
        invalid_core["pipeline"].append({
            "id": "bad_node",
            "node_type": "CompletelyUnknownNodeType",
            "depends_on": [],
        })
        for ep in self.CLI_PARITY_ENTRYPOINTS:
            with self.subTest(case="core_failure", entrypoint=ep):
                code, res = self.command(*ep, "--stdin", input_pipeline=invalid_core)
                self.assertEqual(code, 1)
                self.assertFalse(res["ok"])
                self.assertIn("diagnostics", res)
                self.assertEqual(res["diagnostics"][0]["code"], "UNKNOWN_NODE_TYPE")
                if ep[0] == "plan":
                    self.assertIn("plan", res)

        # 3. Valid pipeline plan
        for ep in self.CLI_PARITY_ENTRYPOINTS:
            with self.subTest(case="valid_plan", entrypoint=ep):
                code, res = self.command(*ep, "--stdin", input_pipeline=pipeline)
                self.assertEqual(code, 0)
                self.assertTrue(res["ok"])
                if ep[0] == "plan":
                    self.assertIn("plan", res)
                    self.assertNotIn("diagnostics", res)
                    self.assertTrue(res["plan"]["topological_order"])

    def test_cli_plan_envelopes_repeat(self):
        # Repeat the full CLI parity matrix.
        self.test_cli_plan_envelopes_across_entrypoints()

    def test_cli_validate_io_exact_diagnostic_pointer(self):
        # validate-io returns structured diagnostics with the exact JSON pointer.
        conf_path = ROOT / "demo/fixtures/mock/pipeline_entity_extract.conf"
        conf = json.loads(conf_path.read_text())
        pipe_file = conf_path.with_name(conf["pipe_path"])
        pipe_doc = json.loads(pipe_file.read_text())

        with tempfile.TemporaryDirectory(prefix="validate-io-", dir=ROOT / "build") as directory:
            changed_conf = Path(directory) / "pipeline.conf"
            changed_pipe = Path(directory) / conf["pipe_path"]
            # Explicit malformed allocation retains its exact native pointer.
            output_override(pipe_doc, "entity_out")["capacities"] = {"entities_json": 0}
            changed_pipe.write_text(json.dumps(pipe_doc))
            changed_conf.write_text(json.dumps(conf))

            code, res = self.command("validate-io", str(changed_conf))
            self.assertEqual(code, 1)
            self.assertFalse(res["ok"])
            self.assertEqual(res["diagnostics"][0]["code"], "IO_VALIDATION_ERROR")
            self.assertEqual(
                res["diagnostics"][0]["path"],
                "/deployment/io/out_mem/entity_out",
            )

    def test_cli_rejects_removed_model_paths_field_for_every_value(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        for value in ({}, {"entity_llm": "models/ignored.bin"}, {"bad~id/extra": "ignored"}, None, 42, ""):
            document = copy.deepcopy(pipeline)
            document["deployment"]["model_paths"] = value
            for endpoint in self.CLI_PARITY_ENTRYPOINTS:
                with self.subTest(value=value, endpoint=endpoint):
                    code, report = self.command(*endpoint, "--stdin", input_pipeline=document)
                    self.assertEqual(code, 1)
                    self.assertEqual(report["diagnostics"][0]["code"], "DEPLOYMENT_ERROR")
                    self.assertEqual(report["diagnostics"][0]["path"], "/deployment/model_paths")
                    if endpoint[0] == "plan":
                        self.assertEqual(report["plan"], {"layers": [], "topological_order": []})

    def test_cli_unknown_binding_unknown_root_and_malformed_binding(self):
        # External selector diagnostics are identical across native entrypoints.
        pipeline = json.loads(
            (ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text()
        )
        # 1. Unknown io_binding
        doc1 = copy.deepcopy(pipeline)
        io_overrides(doc1)["io_binding"] = "nonexistent.binding.v99"

        # 2. Removed root selector is rejected even when a binding is present.
        doc2 = copy.deepcopy(pipeline)
        doc2["biz_name"] = "unmatched_biz_name"
        io_overrides(doc2)["io_binding"] = "entity_extract.operator.v1"

        # 3. Binding must be a nonempty string.
        doc3 = copy.deepcopy(pipeline)
        io_overrides(doc3)["io_binding"] = 12345

        cases = [
            ("unknown_binding", doc1, "UNKNOWN_IO_BINDING", "/deployment/io/io_binding"),
            ("removed_biz", doc2, "DEPLOYMENT_ERROR", "/biz_name"),
            ("binding_non_string", doc3, "DEPLOYMENT_ERROR", "/deployment/io/io_binding"),
        ]

        for case_name, doc, exp_code, exp_path in cases:
            for ep in self.CLI_PARITY_ENTRYPOINTS:
                with self.subTest(case=case_name, entrypoint=ep):
                    code, res = self.command(*ep, "--stdin", input_pipeline=doc)
                    self.assertEqual(code, 1)
                    self.assertFalse(res["ok"])
                    self.assertEqual(res["diagnostics"][0]["code"], exp_code)
                    self.assertEqual(res["diagnostics"][0]["path"], exp_path)
                    if ep[0] == "plan":
                        self.assertEqual(res["plan"], {"layers": [], "topological_order": []})

    def test_cli_output_slot_allocation_errors(self):
        # Reject removed type fields, unknown slots, invalid allocation shapes and capacities.
        pipeline = json.loads(
            (ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text()
        )
        # 1. A removed type declaration is an unknown field.
        doc1 = copy.deepcopy(pipeline)
        output_override(doc1, "entity_out")["type"] = "entity_out"

        # 2. Unknown output slot
        doc2 = copy.deepcopy(pipeline)
        io_overrides(doc2)["out_mem"] = {"bogus_slot": {}}

        # 3. Invalid output allocation shape
        doc3 = copy.deepcopy(pipeline)
        io_overrides(doc3)["out_mem"] = {"entity_out": []}

        # 4. Invalid output allocation capacity (non-positive capacity value 0)
        doc4 = copy.deepcopy(pipeline)
        output_override(doc4, "entity_out")["capacities"] = {"entities_json": 0}

        cases = [
            ("removed_type", doc1, "INVALID_OUTPUT_ALLOCATION", "/deployment/io/out_mem/entity_out"),
            ("unknown_slot", doc2, "UNKNOWN_OUTPUT_SLOT", "/deployment/io/out_mem/bogus_slot"),
            ("invalid_alloc", doc3, "INVALID_OUTPUT_ALLOCATION", "/deployment/io/out_mem/entity_out"),
            ("invalid_capacity", doc4, "INVALID_OUTPUT_ALLOCATION", "/deployment/io/out_mem/entity_out"),
        ]

        for case_name, doc, exp_code, exp_path in cases:
            for ep in self.CLI_PARITY_ENTRYPOINTS:
                with self.subTest(case=case_name, entrypoint=ep):
                    code, res = self.command(*ep, "--stdin", input_pipeline=doc)
                    self.assertEqual(code, 1)
                    self.assertFalse(res["ok"])
                    self.assertEqual(res["diagnostics"][0]["code"], exp_code)
                    self.assertEqual(res["diagnostics"][0]["path"], exp_path)
                    if case_name == "removed_type":
                        self.assertIn("Unknown output allocation field: type", res["diagnostics"][0]["message"])
                    if ep[0] == "plan":
                        self.assertEqual(res["plan"], {"layers": [], "topological_order": []})

    def test_cli_multiple_core_errors_with_deployment(self):
        # Valid deployment, but Core has multiple Node errors.
        # All diagnostics must be preserved in the response array across all CLI entrypoints.
        pipeline = json.loads(
            (ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text()
        )
        # Add two invalid nodes to pipeline
        doc = copy.deepcopy(pipeline)
        doc["pipeline"].append({
            "id": "bad_node_1",
            "node_type": "CompletelyUnknownNodeTypeOne",
            "depends_on": [],
        })
        doc["pipeline"].append({
            "id": "bad_node_2",
            "node_type": "CompletelyUnknownNodeTypeTwo",
            "depends_on": [],
        })

        for ep in self.CLI_PARITY_ENTRYPOINTS:
            with self.subTest(entrypoint=ep):
                code, res = self.command(*ep, "--stdin", input_pipeline=doc)
                self.assertEqual(code, 1)
                self.assertFalse(res["ok"])
                self.assertIn("diagnostics", res)
                # Must retain multiple diagnostics, not compressed
                self.assertGreaterEqual(len(res["diagnostics"]), 2)
                diag_codes = [d["code"] for d in res["diagnostics"]]
                self.assertIn("UNKNOWN_NODE_TYPE", diag_codes)
                if ep[0] == "plan":
                    self.assertIn("plan", res)

    def test_cli_edit_invalid_deployment_validation_policy(self):
        # Edit invalid deployment with require_valid=false vs require_valid=true.
        pipeline = json.loads(
            (ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text()
        )
        invalid_doc = copy.deepcopy(pipeline)
        io_overrides(invalid_doc)["io_binding"] = "invalid_binding_id"

        op = {
            "kind": "add_node",
            "node_type": "TextTemplateNode",
            "id": "draft_node",
            "config": {"template": "{{input}}"},
        }

        # Case 1: require_valid=false -> returns modified draft with validation.ok=false
        req_false = {
            "schema_version": 1,
            "pipeline": invalid_doc,
            "operation": op,
            "require_valid": False,
        }
        code_false, res_false = self.command("edit", "--stdin", input_pipeline=req_false)
        self.assertEqual(code_false, 0)
        self.assertTrue(res_false["ok"])
        self.assertIn("pipeline", res_false)
        draft_node_ids = [n["id"] for n in res_false["pipeline"]["pipeline"]]
        self.assertIn("draft_node", draft_node_ids)
        self.assertIn("validation", res_false)
        self.assertFalse(res_false["validation"]["ok"])
        self.assertEqual(res_false["validation"]["diagnostics"][0]["code"], "UNKNOWN_IO_BINDING")

        # Case 2: require_valid=true -> rejects without returning modified pipeline
        req_true = {
            "schema_version": 1,
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
        self.assertEqual(res_true["validation"]["diagnostics"][0]["code"], "UNKNOWN_IO_BINDING")


    def test_cli_file_failures_return_json_diagnostics(self):
        # Missing files produce schema-compliant JSON diagnostics through CLI entrypoints.
        # 1. validate-io on non-existent config file
        code, res = self.command("validate-io", "nonexistent_config_file.conf")
        self.assertEqual(code, 1)
        self.assertFalse(res["ok"])
        self.assertEqual(res["schema_version"], 1)
        self.assertEqual(res["diagnostics"][0]["code"], "IO_VALIDATION_ERROR")
        self.assertIn("nonexistent_config_file.conf", res["diagnostics"][0]["message"])

        # 2. resolve-conf on non-existent file
        code, res = self.command("resolve-conf", "nonexistent_config_file.conf", "--root", str(ROOT))
        self.assertEqual(code, 1)
        self.assertFalse(res["ok"])
        self.assertEqual(res["schema_version"], 1)
        self.assertEqual(res["diagnostics"][0]["code"], "DEPLOYMENT_CONFIG")
        self.assertEqual(res["diagnostics"][0]["path"], "/")

        # 3. validate on non-existent file
        code, res = self.command("validate", "nonexistent_pipeline.json")
        self.assertEqual(code, 1)
        self.assertFalse(res["ok"])
        self.assertEqual(res["schema_version"], 1)
        self.assertEqual(res["diagnostics"][0]["code"], "JSON_READ")

    def test_native_viewer_shows_data_mappings_and_extra_order(self):
        process = subprocess.run(
            [str(ALG_SHOW), str(ROOT / "configs" / "pipeline_doc_qa_default.json")],
            text=True,
            capture_output=True,
            cwd=ROOT,
            check=False,
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertIn("node_1_QueryEmbeddingNode", process.stdout)
        self.assertIn("depends_on: [] (额外顺序)", process.stdout)
        self.assertIn('inputs: {"text":"raw_docs"}', process.stdout)
        self.assertIn('"chunks":"doc_chunks"', process.stdout)
        self.assertNotIn(
            'depends_on: ["node_0_TextChunkNode"]', process.stdout
        )
        self.assertIn("alg_pipeline_tool validate/plan", process.stdout)

    def test_native_viewer_shows_declared_backend_batch_fields(self):
        process = subprocess.run(
            [str(ALG_SHOW), str(ROOT / "configs" / "pipeline_doc_qa_cpu.json")],
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
            (ROOT / "configs" / "pipeline_doc_qa_cpu.json").read_text()
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
            "operation": {"kind": "add_node", "node_type": "TextTemplateNode"},
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
        self.assertNotIn("renderError", app_js)
        self.assertIn("!state.catalogReady", app_js)
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

    def test_startup_endpoints_load_rerank_pipeline_and_refresh_saved_list(self):
        path = ROOT / "configs/pipeline_doc_qa_rerank_cpu.json"
        pipeline = json.loads(path.read_text())
        managed = Path(self.temporary.name) / path.name
        managed.write_text(json.dumps(pipeline))
        self.service.initial_document = self.service.open_pipeline(path.name)
        for endpoint in ("/catalog", "/profiles", "/pipelines", "/assets", "/initial",
                         "/catalog?io_binding=doc_qa.operator.v1"):
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
    assert.equal(requested, base + "api/v1/pipelines");
    await api("/catalog?io_binding=doc_qa.operator.v1");
    assert.equal(requested, base + "api/v1/catalog?io_binding=doc_qa.operator.v1");
  }
}
body = JSON.stringify({ ok: false, error: { code: "INVALID_JSON", message: "invalid pipeline" } });
status = 400;
await assert.rejects(api("/pipeline"), error => error.status === 400 &&
  error.message.includes("invalid pipeline") && error.message.includes("/api/v1/pipeline") &&
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
    error.message.includes("/workspace/proxy/8080/api/v1/initial") &&
    error.message.includes(`HTTP ${code}`) && error.message.includes(summary));
}
status = 500; body = "Not Found " + "x".repeat(1000) + "END_OF_BODY";
await assert.rejects(api("/initial"), error => !error.message.includes("END_OF_BODY"));
globalThis.fetch = async () => { throw new TypeError("Failed to fetch"); };
await assert.rejects(api("/catalog"), error => error.message.includes("/api/v1/catalog") && error.message.includes("Failed to fetch"));
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
  {{ model_id: "embed", model_type: "embed_type" }},
  {{ model_id: "llm", model_type: "llm_type" }},
  {{ model_id: "legacy_embed", model_type: "legacy_embed_type" }},
];
const modelDefinitions = [
  {{ model_type: "embed_type", capability: "embedding" }},
  {{ model_type: "llm_type", capability: "llm" }},
  {{ model_type: "legacy_embed_type", capability: "embedding" }},
];
const nodeDefinition = {{ model_dependencies: [{{ name: "encoder", capability: "embedding", config_field: "model_slot" }}] }};
assert.deepEqual(
  workbench.compatibleModels(models, modelDefinitions, nodeDefinition).map(model => model.model_id),
  ["embed", "legacy_embed"]
);
assert.deepEqual(
  [...workbench.modelBoundNodeIds([
    {{ id: "bound", node_type: "CustomNode", config: {{ model_slot: "embed" }} }},
    {{ id: "hardcoded", node_type: "CustomNode", config: {{ bind_model: "llm" }} }},
  ], [{{ node_type: "CustomNode", model_dependencies: [{{ name: "encoder", capability: "embedding", config_field: "model_slot" }}] }}])],
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
const pipeline = {deployment:{io:{io_binding:'keyword_match.operator.v1'}}, models:[], pipeline:[
  {id:'template', node_type:'TextTemplateNode', depends_on:[], config:{template:'{{primary}}'}, inputs:{primary:'input_sentences'},outputs:{text:'template_text'}},
  {id:'rule', node_type:'TextRuleMatchNode', depends_on:['template'], config:{}, inputs:{text:'template_text'},outputs:{matches:'rule_matches'}}
]};
const snapshot = structuredClone(pipeline);
const graph = w.graphDocument(pipeline,catalog);
assert.ok(graph.edges.some(e=>e.source===w.INGRESS && e.target==='template' && e.targetPort==='primary'));
assert.ok(graph.edges.some(e=>e.source==='template' && e.sourcePort==='text' && e.target==='rule' && e.targetPort==='text'));
assert.ok(graph.edges.some(e=>e.source==='rule' && e.target===w.EGRESS && e.targetPort==='rule_matches'));
assert.deepEqual(pipeline,snapshot,'graph rendering must not mutate the document');
const modelDef = catalog.models.find(m=>m.model_type==='bge_embedding');
const backend = w.compatibleBackends(catalog.backends,modelDef).find(b=>b.backend_type==='onnxruntime');
if (backend) {
assert.ok(!w.compatibleBackends(catalog.backends,modelDef).some(b=>b.backend_type==='llama_cpp'));
assert.equal(w.assetModelPath('weights.bin', 'models'), 'models/weights.bin');
assert.equal(w.assetModelPath('nested/weights.bin', 'assets/'), 'assets/nested/weights.bin');
assert.equal(w.assetModelPath('demo/model.fixture', '.'), 'demo/model.fixture');
const nestedAsset = {model:{model_path:'nested/weights.bin',model_config:{tokenizer_file:'nested/vocab.txt'}},paths:{'/model_path':'nested/weights.bin','/model_config/tokenizer_file':'nested/vocab.txt'}};
assert.deepEqual(w.assetModel(nestedAsset), {model_path:'models/nested/weights.bin',model_config:{tokenizer_file:'vocab.txt'}});
nestedAsset.paths['/model_config/tokenizer_file']='outside/vocab.txt';
assert.throws(()=>w.assetModel(nestedAsset), /绝对附属文件路径/);
const models = {models:[], pipeline:[]};
w.upsertModel(models,catalog,'',{model_id:'embed',model_type:modelDef.model_type,backend:backend.backend_type,model_path:'embed.onnx',model_config:w.schemaDefaults(modelDef.config_fields),backend_config:w.schemaDefaults(backend.config_fields)});
assert.equal(models.deployment,undefined,'adding a model must not invent deployment overrides');
assert.equal(Object.hasOwn(models.models[0], "capability"), false);
models.deployment = {io:{io_binding:'doc_qa.operator.v1'}};
models.pipeline.push({id:'embed_node',node_type:'TextEmbeddingNode',config:{bind_model:'embed'}});
w.upsertModel(models,catalog,'embed',{...models.models[0],model_id:'renamed'});
assert.equal(models.pipeline[0].config.bind_model,'renamed');
assert.equal(models.models[0].model_path,'embed.onnx');
assert.deepEqual(models.deployment,{io:{io_binding:'doc_qa.operator.v1'}});
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
                pipeline = copy.deepcopy(case["pipeline"])
                _, catalog = PipelineCliTest.command(self, "catalog")
                bindings = {b["biz_name"]: b["binding_id"] for b in catalog["io_bindings"]}
                neutral_biz = pipeline.pop("biz_name")
                pipeline["deployment"] = {"io": {"io_binding": bindings.get(neutral_biz, "unknown.binding")}}
                if case["primary_code"] == "UNKNOWN_BIZ":
                    case = {**case, "primary_code": "UNKNOWN_IO_BINDING",
                            "primary_path": "/deployment/io/io_binding", "required_codes": ["UNKNOWN_IO_BINDING"]}
                elif case["primary_code"] == "UNKNOWN_FIELD" and case["primary_path"].count("/") == 1:
                    # The external envelope rejects unknown root fields before Core.
                    case = {**case, "primary_code": "DEPLOYMENT_ERROR",
                            "required_codes": ["DEPLOYMENT_ERROR"]}
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
                # Preparation errors have an explicit empty plan envelope;
                # diagnostics remain identical to validate and the HTTP API.
                if "plan" not in cli_val:
                    self.assertEqual(cli_plan.pop("plan"), {"layers": [], "topological_order": []})
                self.assertEqual(cli_plan, cli_val)


class SelectionVerificationTest(unittest.TestCase):
    def test_studio_selection_uses_selected_asset_directory_and_host_root(self):
        service = SHOW.WorkbenchService(ROOT / "configs")
        with mock.patch.object(SHOW.SELECTION, "inspect_selection", return_value={"ok": True}) as inspect:
            self.assertTrue(service.verify_selection({"models": []}, model_root="custom-assets")["ok"])
            self.assertEqual(inspect.call_args.args[2], ROOT / "custom-assets")
            self.assertEqual(inspect.call_args.kwargs["pipeline_root"], ROOT)
        with self.assertRaises(SHOW.StudioError):
            service.verify_selection({"models": []}, model_root="../outside-assets")

    def test_run_conf_preserves_binding_override_without_inference(self):
        selection = SHOW.SELECTION
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for binding in ("keyword_match.operator.v1", "new_contract.operator.v1"):
                pipeline = {"deployment": {"io": {"io_binding": binding}}, "models": []}
                conf = selection.build_run_conf(pipeline, {}, "pipeline.json", root, root)
                self.assertEqual(conf, {"pipe_path": "pipeline.json"})
                self.assertEqual(pipeline["deployment"]["io"]["io_binding"], binding)
                for explicit in ("custom.operator.v3", "", None, 42):
                    with self.subTest(explicit=explicit):
                        io_overrides(pipeline)["io_binding"] = explicit
                        selection.build_run_conf(pipeline, {}, "pipeline.json", root, root)
                        self.assertEqual(pipeline["deployment"]["io"]["io_binding"], explicit)

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

    def test_asset_paths_use_host_root_and_sidecars_use_model_directory(self):
        selection = SHOW.SELECTION
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / "models"
            nested = assets / "nested"
            nested.mkdir(parents=True)
            (nested / "weights.bin").write_bytes(b"weights")
            (nested / "vocab.txt").write_bytes(b"vocabulary")
            model = {"model_id": "m", "model_type": "bge_embedding", "backend": "onnxruntime",
                     "model_path": "models/nested/weights.bin", "model_config": {"tokenizer_file": "vocab.txt"}}
            template = copy.deepcopy(model)
            template["model_path"] = "nested/weights.bin"
            template["model_config"]["tokenizer_file"] = "nested/vocab.txt"
            manifest = {"schema_version": 1, "selections": [{"id": "nested", "model": template,
                "paths": {"/model_path": "nested/weights.bin", "/model_config/tokenizer_file": "nested/vocab.txt"},
                "files": ["nested/weights.bin", "nested/vocab.txt"]}],
                "artifacts": {name: {"sha256": selection.file_digest(assets / name)}
                              for name in ("nested/weights.bin", "nested/vocab.txt")}}
            if not shutil.which("node"):
                self.skipTest("Node.js is required to exercise the asset-selection helper")
            script = """
import {assetModel} from './tools/pipeline_studio/web/workbench.js';
import fs from 'node:fs';
const asset = JSON.parse(fs.readFileSync(0, 'utf8'));
process.stdout.write(JSON.stringify(assetModel(asset, 'models')));
"""
            generated = subprocess.run([shutil.which("node"), "--input-type=module", "-e", script],
                                       input=json.dumps(manifest["selections"][0]), cwd=ROOT,
                                       text=True, capture_output=True, check=True)
            model = json.loads(generated.stdout)
            self.assertEqual(model["model_path"], "models/nested/weights.bin")
            self.assertEqual(model["model_config"]["tokenizer_file"], "vocab.txt")
            pipeline = {"models": [model]}
            self.assertEqual(selection.verify_assets(pipeline, assets, manifest, pipeline_root=root)[0]["status"], "verified")
            original = copy.deepcopy(pipeline)
            selection.build_run_conf(pipeline, {}, "pipeline.json", assets, root)
            selection.build_run_conf(pipeline, {}, "pipeline.json", assets, root)
            self.assertEqual(pipeline, original, "saving must not prefix model paths again")
            model["model_config"]["tokenizer_file"] = "../vocab.txt"
            self.assertEqual(selection.verify_assets(pipeline, assets, manifest, pipeline_root=root)[0]["status"], "unregistered")
            (assets / "vocab.txt").write_bytes(b"vocabulary")
            escaped_manifest = copy.deepcopy(manifest)
            choice = escaped_manifest["selections"][0]
            choice["model"]["model_config"]["tokenizer_file"] = "vocab.txt"
            choice["paths"]["/model_config/tokenizer_file"] = "vocab.txt"
            choice["files"] = ["nested/weights.bin", "vocab.txt"]
            escaped_manifest["artifacts"]["vocab.txt"] = {"sha256": selection.file_digest(assets / "vocab.txt")}
            self.assertEqual(selection.verify_assets(pipeline, assets, escaped_manifest, pipeline_root=root)[0]["status"], "unregistered",
                             "a relative sidecar must stay inside the model directory")
            model["model_config"]["tokenizer_file"] = str(assets / "vocab.txt")
            self.assertEqual(selection.verify_assets(pipeline, assets, escaped_manifest, pipeline_root=root)[0]["status"], "verified")

    def test_native_build_variant_and_real_effects_are_bound_to_selection(self):
        selection = SHOW.SELECTION
        tool = Path(os.environ.get("LLM_EDGEFLOW_SELECTION_TOOL", ROOT / "build/alg_pipeline_tool"))
        demo = SHOW.DEMO_BINARY
        pipeline = json.loads((ROOT / "configs/pipeline_keyword_match_rules.json").read_text())
        spec = ROOT / "tests/fixtures/effects/keyword_exact.json"
        conf = ROOT / "configs/pipeline_keyword_match_rules.conf"
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
        existing["id"] = "b"
        existing.pop("depends_on", None)
        existing["inputs"] = {"text": "text"}  # Explicit input uses the default output key.
        source = {
            "id": "a", "node_type": "TextTemplateNode", "depends_on": [],
            "inputs": {"primary": "input_sentences"}, "outputs": {},
            "config": {"template": "{{primary}}"},
        }
        consumer = copy.deepcopy(existing)
        consumer.update(id="c")
        consumer.update(inputs={"text": "input_sentences"},
                        outputs={"matches": "other_matches"})
        pipe["pipeline"] = [source, existing, consumer]
        self.assertTrue(self.service.validate(pipe)["ok"])
        code, result = self.run_edit({
            "schema_version": 1, "pipeline": pipe, "require_valid": True,
            "operation": {"kind": "connect",
                          "source": {"node_id": "a", "port": "text"},
                          "target": {"node_id": "c", "port": "text"}},
        })
        self.assertEqual(code, 0, result)
        self.assertTrue(result["validation"]["ok"])
        a, b, c = result["pipeline"]["pipeline"]
        self.assertEqual(a.get("outputs", {}).get("text", "text"), "text")
        self.assertEqual(b, existing)
        self.assertEqual(c["inputs"]["text"], "text")
        self.assertNotIn("depends_on", c)

    def test_authoring_inferred_cycle_is_rejected_by_native_validation(self):
        pipe = copy.deepcopy(self.keyword)
        rule = pipe["pipeline"][0]
        rule["inputs"]["text"] = "b_text"
        pipe["pipeline"] = [
            {"id": "a", "node_type": "TextTemplateNode",
             "inputs": {"primary": "input_sentences"}, "outputs": {"text": "a_text"}},
            {"id": "b", "node_type": "TextTemplateNode",
             "inputs": {"primary": "a_text"}, "outputs": {"text": "b_text"}},
            rule,
        ]
        self.assertTrue(self.service.validate(pipe)["ok"])
        operations = [
            {"kind": "connect", "source": {"node_id": "b", "port": "text"},
             "target": {"node_id": "a", "port": "primary"}},
            {"kind": "add_dependency", "node_id": "a", "depends_on_id": "b"},
        ]
        for operation in operations:
            for require_valid in (False, True):
                with self.subTest(operation=operation["kind"], require_valid=require_valid):
                    code, result = self.run_edit({
                        "schema_version": 1, "pipeline": pipe,
                        "require_valid": require_valid, "operation": operation,
                    })
                    self.assertEqual(code, 1 if require_valid else 0, result)
                    self.assertEqual(result["ok"], not require_valid)
                    self.assertEqual("pipeline" in result, not require_valid)
                    self.assertFalse(result["validation"]["ok"])
                    self.assertIn("DAG_CYCLE", [d["code"] for d in result["validation"]["diagnostics"]])


    def test_authoring_disconnect_rejects_unknown_and_reversed_endpoints(self):
        pipe = copy.deepcopy(self.keyword)
        pipe["pipeline"][0]["id"] = "c"
        pipe["pipeline"].insert(0, {
            "id": "a", "node_type": "TextTemplateNode", "depends_on": [],
            "inputs": {"primary": "input_sentences"}, "outputs": {},
            "config": {"template": "{{primary}}"},
        })
        cases = [
            ({"node_id": "a", "port": "input_sentences"}, {"node_id": "c", "port": "text"}),
            ({"node_id": "$ingress", "port": "missing"}, {"node_id": "c", "port": "text"}),
            ({"node_id": "$egress", "port": "rule_matches"}, {"node_id": "c", "port": "text"}),
            ({"node_id": "c", "port": "matches"}, {"node_id": "$egress", "port": "missing"}),
            ({"node_id": "c", "port": "matches"}, {"node_id": "$ingress", "port": "input_sentences"}),
            ({"node_id": "$ingress", "port": "input_sentences"}, {"node_id": "c", "port": "missing"}),
        ]
        for source, target in cases:
            with self.subTest(source=source, target=target):
                self.assert_edit_rejected({
                    "schema_version": 1, "pipeline": pipe,
                    "operation": {"kind": "disconnect", "source": source, "target": target},
                }, 0)

    def test_authoring_rejects_ambiguous_duplicate_node_targets(self):
        pipe = copy.deepcopy(self.keyword)
        pipe["pipeline"][0]["id"] = "b"
        pipe["pipeline"].append(copy.deepcopy(pipe["pipeline"][0]))
        operations = [
            {"kind": "rename_node", "node_id": "b", "new_id": "bb"},
            {"kind": "remove_node", "node_id": "b"},
            {"kind": "connect", "source": {"node_id": "$ingress", "port": "input_sentences"},
             "target": {"node_id": "b", "port": "text"}},
            {"kind": "connect", "source": {"node_id": "b", "port": "matches"},
             "target": {"node_id": "$egress", "port": "rule_matches"}},
        ]
        for operation in operations:
            with self.subTest(operation=operation):
                self.assert_edit_rejected({"schema_version": 1, "pipeline": pipe,
                                           "operation": operation}, 0)

    def test_authoring_rejects_invalid_request_fields_without_crashing(self):
        base = {"schema_version": 1, "pipeline": self.keyword,
                "operation": {"kind": "add_node", "node_type": "TextTemplateNode"}}
        requests = [
            dict(base, requrie_valid=True), dict(base, require_valid="true"),
            dict(base, schema_version=True), dict(base, schema_version=1.0),
            dict(base, operations=None), dict(base, operation=None, operations=[base["operation"]]),
            dict(base, operations={}), dict(base, pipeline=[]),
        ]
        for request in requests:
            with self.subTest(request=request):
                self.assert_edit_rejected(request)

    def test_authoring_batch_schema_errors_report_index_and_no_candidate(self):
        invalid_operations = [
            {"kind": 42}, {"kind": "remove_node", "node_id": 42},
            {"kind": "add_node", "node_type": "TextTemplateNode", "config": []},
            {"kind": "add_node", "node_type": "TextTemplateNode", "unexpected": True},
            {"kind": "connect", "source": [], "target": {}},
            {"kind": "connect", "source": {"node_id": "$ingress", "port": "input_sentences", "unexpected": 1},
             "target": {"node_id": "added", "port": "primary"}},
            {"kind": "connect", "source": {"node_id": "$ingress", "port": 42},
             "target": {"node_id": "added", "port": "primary"}},
            {"kind": "reset_input_binding", "target": {"node_id": "added", "port": "primary"}},
            None, [],
        ]
        for operation in invalid_operations:
            with self.subTest(operation=operation):
                self.assert_edit_rejected({
                    "schema_version": 1, "pipeline": self.keyword,
                    "operations": [
                        {"kind": "add_node", "node_type": "TextTemplateNode", "id": "added"},
                        operation,
                    ],
                }, 1)

    def test_authoring_add_node_explicit_auto_id_and_collision(self):
        req = {
            "schema_version": 1,
            "pipeline": {"deployment": {"io": {"io_binding": "keyword_match.operator.v1"}}, "models": [], "pipeline": []},
            "operation": {
                "kind": "add_node",
                "node_type": "TextRuleMatchNode",
                "id": "custom_rule",
            },
        }
        code, res = self.run_edit(req)
        self.assertEqual(code, 0)
        self.assertTrue(res["ok"])
        added = res["pipeline"]["pipeline"][0]
        self.assertEqual(added["id"], "custom_rule")
        self.assertNotIn("ports", added)
        self.assertNotIn("depends_on", added)
        self.assertEqual(added["inputs"], {})

        # Duplicate ID rejection
        req2 = {
            "schema_version": 1,
            "pipeline": res["pipeline"],
            "operation": {
                "kind": "add_node",
                "node_type": "TextRuleMatchNode",
                "id": "custom_rule",
            },
        }
        code2, res2 = self.run_edit(req2)
        self.assertEqual(code2, 1)
        self.assertFalse(res2["ok"])
        self.assertIn("DUPLICATE_NODE_ID", res2["diagnostics"][0]["message"])

        # Auto ID allocation
        req3 = {
            "schema_version": 1,
            "pipeline": res["pipeline"],
            "operation": {
                "kind": "add_node",
                "node_type": "TextRuleMatchNode",
            },
        }
        code3, res3 = self.run_edit(req3)
        self.assertEqual(code3, 0)
        self.assertTrue(res3["ok"])
        self.assertNotEqual(res3["pipeline"]["pipeline"][1]["id"], "custom_rule")

        # Unknown node type
        req4 = {
            "schema_version": 1,
            "pipeline": res["pipeline"],
            "operation": {"kind": "add_node", "node_type": "NonExistentNode"},
        }
        code4, res4 = self.run_edit(req4)
        self.assertEqual(code4, 1)
        self.assertFalse(res4["ok"])
        self.assertIn("UNKNOWN_NODE_TYPE", res4["diagnostics"][0]["message"])

    def test_authoring_remove_node_detaches_inputs_and_dependencies(self):
        pipe = {
            "deployment": {"io": {"io_binding": "keyword_match.operator.v1"}},
            "models": [],
            "pipeline": [
                {
                    "id": "template",
                    "node_type": "TextTemplateNode",
                    "depends_on": [],
                    "inputs": {}, "outputs": {"text": "tpl_text"},
                    "config": {"template": "{{primary}}"},
                },
                {
                    "id": "rule",
                    "node_type": "TextRuleMatchNode",
                    "depends_on": ["template"],
                    "inputs": {"text": "tpl_text"}, "outputs": {},
                    "config": {},
                },
            ],
        }
        req = {
            "schema_version": 1,
            "pipeline": pipe,
            "operation": {"kind": "remove_node", "node_id": "template"},
        }
        code, res = self.run_edit(req)
        self.assertEqual(code, 0)
        self.assertTrue(res["ok"])
        self.assertEqual(len(res["pipeline"]["pipeline"]), 1)
        rule_node = res["pipeline"]["pipeline"][0]
        self.assertEqual(rule_node["id"], "rule")
        # depends_on detached
        self.assertEqual(rule_node["depends_on"], [])
        # Required input is absent after its producer is removed.
        self.assertNotIn("text", rule_node["inputs"])

    def test_authoring_rename_node_syncs_dependencies_and_preserves_keys(self):
        pipe = {
            "deployment": {"io": {"io_binding": "keyword_match.operator.v1"}},
            "models": [],
            "pipeline": [
                {
                    "id": "template",
                    "node_type": "TextTemplateNode",
                    "depends_on": [],
                    "inputs": {}, "outputs": {"text": "tpl_text"},
                    "config": {"template": "{{primary}}"},
                },
                {
                    "id": "rule",
                    "node_type": "TextRuleMatchNode",
                    "depends_on": ["template"],
                    "inputs": {"text": "tpl_text"}, "outputs": {},
                    "config": {},
                },
            ],
        }
        req = {
            "schema_version": 1,
            "pipeline": pipe,
            "operation": {
                "kind": "rename_node",
                "node_id": "template",
                "new_id": "new_template",
            },
        }
        code, res = self.run_edit(req)
        self.assertEqual(code, 0)
        self.assertTrue(res["ok"])
        tpl = res["pipeline"]["pipeline"][0]
        rule = res["pipeline"]["pipeline"][1]
        self.assertEqual(tpl["id"], "new_template")
        # Data keys are preserved
        self.assertEqual(tpl["outputs"]["text"], "tpl_text")
        # depends_on updated
        self.assertEqual(rule["depends_on"], ["new_template"])

        # Duplicate ID rejection
        req_dup = {
            "schema_version": 1,
            "pipeline": pipe,
            "operation": {"kind": "rename_node", "node_id": "template", "new_id": "rule"},
        }
        code_dup, res_dup = self.run_edit(req_dup)
        self.assertEqual(code_dup, 1)
        self.assertIn("DUPLICATE_NODE_ID", res_dup["diagnostics"][0]["message"])

    def test_authoring_connect_and_cycle_and_redundant_dependencies(self):
        pipe = {
            "deployment": {"io": {"io_binding": "keyword_match.operator.v1"}},
            "models": [],
            "pipeline": [
                {
                    "id": "template",
                    "node_type": "TextTemplateNode",
                    "depends_on": [],
                    "inputs": {}, "outputs": {},
                    "config": {"template": "{{primary}}"},
                },
                {
                    "id": "rule",
                    "node_type": "TextRuleMatchNode",
                    "depends_on": [],
                    "inputs": {}, "outputs": {},
                    "config": {},
                },
            ],
        }
        # Ingress to node
        req1 = {
            "schema_version": 1,
            "pipeline": pipe,
            "operation": {
                "kind": "connect",
                "source": {"node_id": "$ingress", "port": "input_sentences"},
                "target": {"node_id": "template", "port": "primary"},
            },
        }
        code1, res1 = self.run_edit(req1)
        self.assertEqual(code1, 0)
        self.assertEqual(
            res1["pipeline"]["pipeline"][0]["inputs"]["primary"],
            "input_sentences",
        )
        self.assertEqual(res1["pipeline"]["pipeline"][0]["depends_on"], [])

        # Node to node
        req2 = {
            "schema_version": 1,
            "pipeline": res1["pipeline"],
            "operation": {
                "kind": "connect",
                "source": {"node_id": "template", "port": "text"},
                "target": {"node_id": "rule", "port": "text"},
            },
        }
        code2, res2 = self.run_edit(req2)
        self.assertEqual(code2, 0)
        rule = res2["pipeline"]["pipeline"][1]
        tpl_key = res2["pipeline"]["pipeline"][0]["outputs"].get("text", "text")
        self.assertEqual(tpl_key, "text", "connecting preserves the effective default output key")
        self.assertEqual(rule["inputs"]["text"], tpl_key)
        self.assertEqual(rule["depends_on"], [])

        # Cycle detection: connecting rule to template creates cycle
        req_cycle = {
            "schema_version": 1,
            "pipeline": res2["pipeline"],
            "operation": {
                "kind": "connect",
                "source": {"node_id": "rule", "port": "matches"},
                "target": {"node_id": "template", "port": "primary"},
            },
        }
        code_cycle, res_cycle = self.run_edit(req_cycle)
        self.assertEqual(code_cycle, 1)
        self.assertFalse(res_cycle["ok"])

    def test_authoring_connect_and_disconnect_biz_egress(self):
        pipe = {
            "deployment": {"io": {"io_binding": "keyword_match.operator.v1"}},
            "models": [],
            "pipeline": [
                {
                    "id": "rule",
                    "node_type": "TextRuleMatchNode",
                    "depends_on": [],
                    "inputs": {"text": "input_sentences"},
                    "outputs": {"matches": "rule_internal_out"},
                    "config": {},
                },
                {
                    "id": "audit",
                    "node_type": "TextRuleMatchNode",
                    "depends_on": ["rule"],
                    "inputs": {"text": "rule_internal_out"}, "outputs": {},
                    "config": {},
                },
            ],
        }
        # Connect rule.matches to $egress rule_matches
        req_egress = {
            "schema_version": 1,
            "pipeline": pipe,
            "operation": {
                "kind": "connect",
                "source": {"node_id": "rule", "port": "matches"},
                "target": {"node_id": "$egress", "port": "rule_matches"},
            },
        }
        code_egress, res_egress = self.run_edit(req_egress)
        self.assertEqual(code_egress, 0)
        rule_out = res_egress["pipeline"]["pipeline"][0]["outputs"]["matches"]
        audit_in = res_egress["pipeline"]["pipeline"][1]["inputs"]["text"]
        self.assertEqual(rule_out, "rule_matches")
        self.assertEqual(audit_in, "rule_matches")

        # Disconnect rule.matches from $egress rule_matches
        req_disc = {
            "schema_version": 1,
            "pipeline": res_egress["pipeline"],
            "operation": {
                "kind": "disconnect",
                "source": {"node_id": "rule", "port": "matches"},
                "target": {"node_id": "$egress", "port": "rule_matches"},
            },
        }
        code_disc, res_disc = self.run_edit(req_disc)
        self.assertEqual(code_disc, 0)
        rule_out2 = res_disc["pipeline"]["pipeline"][0]["outputs"]["matches"]
        audit_in2 = res_disc["pipeline"]["pipeline"][1]["inputs"]["text"]
        self.assertNotEqual(rule_out2, "rule_matches")
        self.assertEqual(rule_out2, audit_in2)

    def test_authoring_disconnect_preserves_extra_order_and_removes_input(self):
        pipe = {
            "deployment": {"io": {"io_binding": "keyword_match.operator.v1"}},
            "models": [],
            "pipeline": [
                {
                    "id": "tpl",
                    "node_type": "TextTemplateNode",
                    "depends_on": [],
                    "inputs": {}, "outputs": {"text": "tpl_out"},
                    "config": {"template": "{{primary}}"},
                },
                {
                    "id": "rule",
                    "node_type": "TextRuleMatchNode",
                    "depends_on": ["tpl"],
                    "inputs": {"text": "tpl_out"}, "outputs": {},
                    "config": {},
                },
            ],
        }
        # Disconnecting data port preserves execution dependency (Regression Case 1)
        req = {
            "schema_version": 1,
            "pipeline": pipe,
            "operation": {
                "kind": "disconnect",
                "source": {"node_id": "tpl", "port": "text"},
                "target": {"node_id": "rule", "port": "text"},
            },
        }
        code, res = self.run_edit(req)
        self.assertEqual(code, 0)
        rule = res["pipeline"]["pipeline"][1]
        self.assertEqual(rule["depends_on"], ["tpl"])
        self.assertNotIn("text", rule["inputs"])

        # remove_dependency removes execution dependency
        req_rm_dep = {
            "schema_version": 1,
            "pipeline": res["pipeline"],
            "operation": {
                "kind": "remove_dependency",
                "node_id": "rule",
                "depends_on_id": "tpl",
            },
        }
        code_rm_dep, res_rm_dep = self.run_edit(req_rm_dep)
        self.assertEqual(code_rm_dep, 0)
        self.assertEqual(res_rm_dep["pipeline"]["pipeline"][1]["depends_on"], [])

    def test_authoring_batch_operations_and_atomic_rollback(self):
        pipe = {"deployment": {"io": {"io_binding": "keyword_match.operator.v1"}}, "models": [], "pipeline": []}
        req_valid = {
            "schema_version": 1,
            "pipeline": pipe,
            "operations": [
                {"kind": "add_node", "node_type": "TextTemplateNode", "id": "t1", "config": {"template": "{{primary}}"}},
                {"kind": "add_node", "node_type": "TextRuleMatchNode", "id": "r1", "config": {"categories": {"K": ["v"]}}},
                {"kind": "connect", "source": {"node_id": "$ingress", "port": "input_sentences"}, "target": {"node_id": "t1", "port": "primary"}},
                {"kind": "connect", "source": {"node_id": "t1", "port": "text"}, "target": {"node_id": "r1", "port": "text"}},
                {"kind": "connect", "source": {"node_id": "r1", "port": "matches"}, "target": {"node_id": "$egress", "port": "rule_matches"}},
            ],
            "require_valid": True,
        }
        code_valid, res_valid = self.run_edit(req_valid)
        self.assertEqual(code_valid, 0)
        self.assertTrue(res_valid["ok"])
        self.assertTrue(res_valid["validation"]["ok"])
        self.assertEqual(len(res_valid["pipeline"]["pipeline"]), 2)

        # Failure mid-batch discards changes
        req_fail = {
            "schema_version": 1,
            "pipeline": pipe,
            "operations": [
                {"kind": "add_node", "node_type": "TextTemplateNode", "id": "t1"},
                {"kind": "add_node", "node_type": "InvalidNodeType"},
                {"kind": "add_node", "node_type": "TextRuleMatchNode", "id": "r1"},
            ],
        }
        code_fail, res_fail = self.run_edit(req_fail)
        self.assertEqual(code_fail, 1)
        self.assertFalse(res_fail["ok"])
        self.assertEqual(res_fail["failed_operation_index"], 1)
        self.assertNotIn("pipeline", res_fail)

        # Batch limit > 128
        req_oversize = {
            "schema_version": 1,
            "pipeline": pipe,
            "operations": [{"kind": "add_node", "node_type": "TextTemplateNode"}] * 129,
        }
        code_oversize, res_oversize = self.run_edit(req_oversize)
        self.assertEqual(code_oversize, 1)
        self.assertFalse(res_oversize["ok"])

    def test_authoring_regression_case_2_key_allocation_no_collision(self):
        pipe = {"deployment": {"io": {"io_binding": "keyword_match.operator.v1"}}, "models": [], "pipeline": []}
        req = {
            "schema_version": 1,
            "pipeline": pipe,
            "operations": [
                {"kind": "add_node", "node_type": "TextRuleMatchNode", "id": "foo"},
                {"kind": "rename_node", "node_id": "foo", "new_id": "bar"},
                {"kind": "add_node", "node_type": "TextRuleMatchNode", "id": "foo"},
            ],
        }
        code, res = self.run_edit(req)
        self.assertEqual(code, 0)
        self.assertTrue(res["ok"])
        bar_node = res["pipeline"]["pipeline"][0]
        foo_node = res["pipeline"]["pipeline"][1]
        self.assertEqual(bar_node["id"], "bar")
        self.assertEqual(foo_node["id"], "foo")
        bar_key = bar_node["outputs"]["matches"]
        foo_key = foo_node["outputs"]["matches"]
        self.assertNotEqual(bar_key, foo_key, "Allocated keys must not collide with retained keys")


    def test_studio_authoring_preview_endpoint(self):
        pipe = {"deployment": {"io": {"io_binding": "keyword_match.operator.v1"}}, "models": [], "pipeline": []}
        res = self.service.preview_authoring(
            pipe,
            operation={"kind": "add_node", "node_type": "TextRuleMatchNode", "id": "n1"},
            expected_revision=SHOW.revision_for(json.dumps(pipe, sort_keys=True).encode()),
            tool_fingerprint=self.service.get_tool_fingerprint(),
        )
        self.assertTrue(res["ok"])
        self.assertIn("revision", res)
        self.assertIn("tool_fingerprint", res)
        self.assertEqual(res["pipeline"]["pipeline"][0]["id"], "n1")

        # Stale revision conflict
        with self.assertRaises(SHOW.StudioError) as ctx:
            self.service.preview_authoring(
                pipe,
                operation={"kind": "add_node", "node_type": "TextRuleMatchNode"},
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
                            self.keyword, operation={"kind": "add_node", "node_type": "TextTemplateNode"},
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
        self.assertEqual(summary["biz_name"], "keyword_match_v1")
        self.assertIn("tools", summary)
        self.assertIn("pipeline_snapshot", summary)
        self.assertIn("status", summary)
        # Verify no user file was written
        self.assertFalse((self.configs / "pipeline_preflight_leak.json").exists())

    def associated_doc_qa(self):
        pipeline = json.loads((ROOT / "configs" / "pipeline_doc_qa_cpu.json").read_text())
        conf = json.loads((ROOT / "configs" / "pipeline_doc_qa_cpu.conf").read_text())
        pipeline_path = self.configs / "pipeline_associated.json"
        conf_path = self.configs / "pipeline_associated.conf"
        conf["pipe_path"] = pipeline_path.name
        for model in pipeline.get("models", []):
            model["model_path"] = "models/deployed_" + model["model_id"]
        output_override(pipeline, "doc_out")["capacities"] = {"answer_text": 2047}
        pipeline_path.write_text(json.dumps(pipeline))
        conf_path.write_text(json.dumps(conf))
        self.service.associate_deployment(pipeline_path.name, conf_path.name)
        return pipeline, conf, pipeline_path, conf_path

    def test_associated_preflight_run_and_save_share_candidate(self):
        pipeline, original_conf, path, conf_path = self.associated_doc_qa()
        pipeline["models"][0]["model_path"] = "models/selected_A.onnx"
        model_id = pipeline["models"][0]["model_id"]
        expected_conf = {"pipe_path": path.name}
        # Spy on real native resolution so this checks exactly what preflight resolves.
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
        self.assertEqual(preview["configuration"]["model_paths"][0]["resolved"], str(ROOT / "models/selected_A.onnx"))
        self.assertEqual(preview["configuration"]["model_paths"][0]["source"], "pipeline.models.model_path")
        with mock.patch.object(SHOW.threading, "Thread") as thread:
            self.service.start_run(pipeline, "doc_qa_cpu", filename=path.name)
            args = thread.call_args.kwargs["args"]
            run_profile, run_conf = args[2], args[3]
            self.assertEqual(args[1]["models"], pipeline["models"])
            self.assertNotIn("model_paths", args[1]["deployment"])
        self.assertNotIn("biz", run_profile)
        self.service.save_pipeline(
            path.name, pipeline, SHOW.revision_for(path.read_bytes()),
        )
        saved_conf = json.loads(conf_path.read_text())
        self.assertEqual(saved_conf, expected_conf)
        saved_pipe = json.loads(path.read_text())
        self.assertEqual(saved_pipe["models"][0]["model_path"], "models/selected_A.onnx")
        self.assertNotIn("model_paths", saved_pipe["deployment"])
        self.assertEqual(run_conf, expected_conf)
        self.assertEqual(resolved_candidates[0], {"pipe_path": "pipeline.json"})

    def test_associated_raw_model_edit_is_the_effective_path(self):
        pipeline, conf, path, _ = self.associated_doc_qa()
        pipeline["models"][0]["model_path"] = "models/raw_edit.onnx"
        _, candidate = self.service.deployment_candidate(pipeline, filename=path.name)
        self.assertEqual(pipeline["models"][0]["model_path"], "models/raw_edit.onnx")
        self.assertNotIn("model_paths", pipeline["deployment"])
        self.assertEqual(candidate, {"pipe_path": path.name})
        self.assertEqual(pipeline["deployment"]["io"]["out_mem"]["doc_out"]["capacities"]["answer_text"], 2047)

    def test_associated_new_model_does_not_invent_deployment_override(self):
        pipeline, conf, path, _ = self.associated_doc_qa()
        new_model = copy.deepcopy(pipeline["models"][0])
        new_model.update(model_id="new_model", model_path="new.onnx")
        pipeline["models"].append(new_model)
        _, candidate = self.service.deployment_candidate(pipeline, filename=path.name)
        self.assertNotIn("model_paths", pipeline["deployment"])
        self.assertEqual(pipeline["models"][-1]["model_path"], "new.onnx")

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

    def test_studio_deployment_associate_and_model_path_update(self):
        # Create pipeline in configs
        pipe_path = self.configs / "pipeline_doc_qa_assoc.json"
        doc_qa_pipe = json.loads((ROOT / "configs" / "pipeline_doc_qa_cpu.json").read_text())
        output_override(doc_qa_pipe, "doc_out")["capacities"] = {"answer_text": 2047}
        pipe_path.write_text(json.dumps(doc_qa_pipe, indent=2))

        # Create conf in configs pointing to this pipeline
        conf_path = self.configs / "pipeline_doc_qa_assoc.conf"
        doc_qa_conf = {"pipe_path": pipe_path.name}
        conf_path.write_text(json.dumps(doc_qa_conf, indent=2))

        # Associate
        assoc_res = self.service.associate_deployment(
            "pipeline_doc_qa_assoc.json", "pipeline_doc_qa_assoc.conf"
        )
        self.assertTrue(assoc_res["ok"])
        self.assertEqual(assoc_res["conf_name"], "pipeline_doc_qa_assoc.conf")
        self.assertIn("pipeline_doc_qa_assoc.conf", self.service.save_targets(pipe_path))

        # Now update pipeline models and save
        modified_pipe = copy.deepcopy(doc_qa_pipe)
        # Update model_path of first model
        modified_pipe["models"][0]["model_path"] = "models/new_embed_model.onnx"
        pipe_raw = pipe_path.read_bytes()
        pipe_rev = SHOW.revision_for(pipe_raw)

        save_res = self.service.save_pipeline(
            "pipeline_doc_qa_assoc.json", modified_pipe, pipe_rev, save_as=False,
        )
        self.assertTrue(save_res["ok"])

        # Verify the model entry owns the effective path
        updated_pipe = json.loads(pipe_path.read_text())
        self.assertEqual(
            updated_pipe["models"][0]["model_path"],
            "models/new_embed_model.onnx",
        )
        # Verify non-model conf settings preserved
        self.assertEqual(updated_pipe["deployment"]["io"]["out_mem"],
                         {"doc_out": {"capacities": {"answer_text": 2047}}})
        updated_conf = json.loads(conf_path.read_text())
        self.assertEqual(updated_conf, {"pipe_path": pipe_path.name})

    def test_authoring_oversized_payload_rejection_4mib(self):
        pipe = {"deployment": {"io": {"io_binding": "keyword_match.operator.v1"}}, "models": [], "pipeline": []}
        large_comment = "x" * (4 * 1024 * 1024 + 100)
        req = {
            "schema_version": 1,
            "pipeline": pipe,
            "operation": {"kind": "add_node", "node_type": "TextRuleMatchNode"},
            "comment": large_comment,
        }
        code, res = self.run_edit(req)
        self.assertEqual(code, 1)
        self.assertFalse(res["ok"])
        self.assertIn("REQUEST_TOO_LARGE", res["diagnostics"][0]["message"])

    def test_authoring_reserved_node_ids_rejected(self):
        pipe = {"deployment": {"io": {"io_binding": "keyword_match.operator.v1"}}, "models": [], "pipeline": []}
        req_add_ingress = {
            "schema_version": 1,
            "pipeline": pipe,
            "operation": {"kind": "add_node", "node_type": "TextRuleMatchNode", "id": "$ingress"},
        }
        code1, res1 = self.run_edit(req_add_ingress)
        self.assertEqual(code1, 1)
        self.assertFalse(res1["ok"])
        self.assertIn("RESERVED_NODE_ID", res1["diagnostics"][0]["message"])

        pipe2 = {
            "deployment": {"io": {"io_binding": "keyword_match.operator.v1"}},
            "models": [],
            "pipeline": [
                {"id": "node_a", "node_type": "TextRuleMatchNode", "depends_on": [], "inputs": {}, "outputs": {}, "config": {}}
            ],
        }
        req_rename_egress = {
            "schema_version": 1,
            "pipeline": pipe2,
            "operation": {"kind": "rename_node", "node_id": "node_a", "new_id": "$egress"},
        }
        code2, res2 = self.run_edit(req_rename_egress)
        self.assertEqual(code2, 1)
        self.assertFalse(res2["ok"])
        self.assertIn("RESERVED_NODE_ID", res2["diagnostics"][0]["message"])

    def test_authoring_disconnect_requires_explicit_binding(self):
        # A same-name output plus explicit order must not create an input binding.
        pipe = {
            "deployment": {"io": {"io_binding": "keyword_match.operator.v1"}},
            "models": [],
            "pipeline": [
                {
                    "id": "tpl",
                    "node_type": "TextTemplateNode",
                    "depends_on": [],
                    "inputs": {}, "outputs": {"text": "text"},
                    "config": {"template": "{{primary}}"},
                },
                {
                    "id": "rule",
                    "node_type": "TextRuleMatchNode",
                    "depends_on": ["tpl"],
                    "inputs": {}, "outputs": {},
                    "config": {},
                },
            ],
        }
        req_disc = {
            "schema_version": 1,
            "pipeline": pipe,
            "operation": {
                "kind": "disconnect",
                "source": {"node_id": "tpl", "port": "text"},
                "target": {"node_id": "rule", "port": "text"},
            },
        }
        code, res = self.run_edit(req_disc)
        self.assertEqual(code, 1)
        self.assertFalse(res["ok"])
        self.assertIn("连线不存在", res["diagnostics"][0]["message"])

        # Ingress disconnect
        pipe_ing = {
            "deployment": {"io": {"io_binding": "keyword_match.operator.v1"}},
            "models": [],
            "pipeline": [
                {
                    "id": "rule",
                    "node_type": "TextRuleMatchNode",
                    "depends_on": [],
                    "inputs": {"text": "input_sentences"}, "outputs": {},
                    "config": {},
                }
            ],
        }
        req_ing_disc = {
            "schema_version": 1,
            "pipeline": pipe_ing,
            "operation": {
                "kind": "disconnect",
                "source": {"node_id": "$ingress", "port": "input_sentences"},
                "target": {"node_id": "rule", "port": "text"},
            },
        }
        code_ing, res_ing = self.run_edit(req_ing_disc)
        self.assertEqual(code_ing, 0)
        self.assertTrue(res_ing["ok"])
        self.assertNotIn("text", res_ing["pipeline"]["pipeline"][0]["inputs"])

    def test_authoring_disconnect_egress_syncs_explicit_consumers(self):
        pipe = {
            "deployment": {"io": {"io_binding": "keyword_match.operator.v1"}},
            "models": [],
            "pipeline": [
                {
                    "id": "rule",
                    "node_type": "TextRuleMatchNode",
                    "depends_on": [],
                    "inputs": {"text": "input_sentences"},
                    "outputs": {"matches": "rule_matches"},
                    "config": {},
                },
                {
                    "id": "consumer",
                    "node_type": "TextRuleMatchNode",
                    "depends_on": ["rule"],
                    "inputs": {"text": "rule_matches"},
                    "outputs": {},
                    "config": {},
                },
            ],
        }
        req = {
            "schema_version": 1,
            "pipeline": pipe,
            "operation": {
                "kind": "disconnect",
                "source": {"node_id": "rule", "port": "matches"},
                "target": {"node_id": "$egress", "port": "rule_matches"},
            },
        }
        code, res = self.run_edit(req)
        self.assertEqual(code, 0)
        self.assertTrue(res["ok"])
        rule = res["pipeline"]["pipeline"][0]
        consumer = res["pipeline"]["pipeline"][1]
        new_key = rule["outputs"]["matches"]
        self.assertNotEqual(new_key, "rule_matches")
        self.assertEqual(consumer["inputs"]["text"], new_key)

    def test_authoring_remove_node_handles_consumers_and_egress(self):
        pipe = {
            "deployment": {"io": {"io_binding": "keyword_match.operator.v1"}},
            "models": [],
            "pipeline": [
                {
                    "id": "tpl",
                    "node_type": "TextTemplateNode",
                    "depends_on": [],
                    "inputs": {}, "outputs": {"text": "tpl_out"},
                    "config": {"template": "{{primary}}"},
                },
                {
                    "id": "rule",
                    "node_type": "TextRuleMatchNode",
                    "depends_on": ["tpl"],
                    "inputs": {"text": "tpl_out"}, "outputs": {"matches": "rule_matches"},
                    "config": {},
                },
            ],
        }
        req = {
            "schema_version": 1,
            "pipeline": pipe,
            "operation": {"kind": "remove_node", "node_id": "rule"},
        }
        code, res = self.run_edit(req)
        self.assertEqual(code, 0)
        self.assertTrue(res["ok"])
        change = res["changes"][0]
        self.assertIn("$egress", change["affected_nodes"])

    def test_authoring_add_dependency_avoids_redundant_indirect_ancestor(self):
        pipe = {
            "deployment": {"io": {"io_binding": "keyword_match.operator.v1"}},
            "models": [],
            "pipeline": [
                {"id": "a", "node_type": "TextRuleMatchNode", "depends_on": [], "inputs": {}, "outputs": {}, "config": {}},
                {"id": "b", "node_type": "TextRuleMatchNode", "depends_on": ["a"], "inputs": {}, "outputs": {}, "config": {}},
                {"id": "c", "node_type": "TextRuleMatchNode", "depends_on": ["b"], "inputs": {}, "outputs": {}, "config": {}},
            ],
        }
        # Adding c depends on a: a is already an indirect ancestor of c via b
        req = {
            "schema_version": 1,
            "pipeline": pipe,
            "operation": {"kind": "add_dependency", "node_id": "c", "depends_on_id": "a"},
        }
        code, res = self.run_edit(req)
        self.assertEqual(code, 0)
        self.assertTrue(res["ok"])
        c_node = res["pipeline"]["pipeline"][2]
        self.assertEqual(c_node["depends_on"], ["b"])



class PipelineJsonSchemaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.builds = []
        tools = (PIPELINE_TOOL, Path(os.environ.get(
            "LLM_EDGEFLOW_SELECTION_TOOL", ROOT / "build/alg_pipeline_tool")))
        for tool in dict.fromkeys(tools):
            artifacts = {}
            for command in ("catalog", "export-schema"):
                result = subprocess.run(
                    [str(tool), command], cwd=ROOT, text=True,
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
                self.assertNotIn("ok", schema)
                repeated = subprocess.run(
                    [str(tool), "export-schema"], cwd=ROOT, text=True,
                    capture_output=True, timeout=30, check=True)
                self.assertEqual(repeated.stdout, artifacts["export-schema_raw"])
                catalog = subprocess.run(
                    [str(tool), "catalog"], cwd=ROOT, text=True,
                    capture_output=True, timeout=30, check=True)
                self.assertEqual(catalog.stdout, artifacts["catalog_raw"])

    def test_export_rejects_arguments(self):
        for arguments in (("--stdin",), ("pipeline.json",), ("--output", "schema.json")):
            with self.subTest(arguments=arguments):
                result = subprocess.run(
                    [str(PIPELINE_TOOL), "export-schema", *arguments],
                    cwd=ROOT, text=True, capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    def test_each_build_exposes_exactly_its_registered_choices(self):
        for tool, artifacts in self.builds:
            with self.subTest(tool=tool):
                catalog = artifacts["catalog"]
                properties = artifacts["export-schema"]["properties"]
                nodes = properties["pipeline"]["items"]["oneOf"]
                self.assertCountEqual(
                    [node["properties"]["node_type"]["const"] for node in nodes],
                    [node["node_type"] for node in catalog["nodes"]])
                self.assertNotIn("biz_name", properties)
                self.assertIn("deployment", artifacts["export-schema"]["required"])
                self.assertNotIn("execution_mode", properties)
                self.assertEqual(properties["max_parallel_workers"]["minimum"], 1)
                self.assertEqual(properties["max_parallel_workers"]["maximum"], 64)
                model = properties["models"]["items"]["properties"]
                self.assertNotIn("capability", model)
                for choices, definitions, key in (
                    (model["model_type"], catalog["models"], "model_type"),
                    (model["backend"], catalog["backends"], "backend_type"),
                ):
                    if definitions:
                        self.assertCountEqual(choices["enum"], [d[key] for d in definitions])
                    else:
                        self.assertIs(choices, False)

    def assert_config_matches_catalog(self, config, fields):
        self.assertEqual(set(config["properties"]), {f["name"] for f in fields})
        self.assertCountEqual(config["required"], [f["name"] for f in fields if f["required"]])
        self.assertFalse(config["additionalProperties"])
        for field in fields:
            actual = config["properties"][field["name"]]
            self.assertEqual(actual["type"], field["type"])
            for key in ("default", "minimum", "maximum", "enum"):
                self.assertEqual(key in actual, key in field)
                if key in field:
                    self.assertEqual(actual[key], field[key])
            if "semantic" in field:
                self.assertEqual(actual["description"], field["semantic"])

    def test_node_selection_carries_config_and_port_hints(self):
        for tool, artifacts in self.builds:
            branches = artifacts["export-schema"]["properties"]["pipeline"]["items"]["oneOf"]
            by_type = {b["properties"]["node_type"]["const"]: b for b in branches}
            for definition in artifacts["catalog"]["nodes"]:
                with self.subTest(tool=tool, node=definition["node_type"]):
                    branch = by_type[definition["node_type"]]
                    self.assertIn("node_type", branch["required"])
                    self.assertNotIn("depends_on", branch["required"])
                    self.assertNotIn("ports", branch["properties"])
                    properties = branch["properties"]
                    self.assert_config_matches_catalog(properties["config"], definition["config_fields"])
                    self.assertEqual("config" in branch["required"],
                                     any(f["required"] for f in definition["config_fields"]))
                    for direction in ("inputs", "outputs"):
                        ports = properties[direction]
                        self.assertEqual(set(ports["properties"]),
                                         {p["key"] for p in definition[direction]})
                        self.assertEqual(ports["required"],
                                         [p["key"] for p in definition[direction] if direction == "inputs" and p["required"]])
                        for port in definition[direction]:
                            hint = ports["properties"][port["key"]]
                            self.assertEqual(hint["type"], "string")
                            self.assertIn(port["type_id"], hint["description"])

    def test_model_and_backend_selection_preserves_field_metadata(self):
        for tool, artifacts in self.builds:
            model = artifacts["export-schema"]["properties"]["models"]["items"]
            branches = model.get("allOf", [])
            for section, selector, config_key in (
                ("models", "model_type", "model_config"),
                ("backends", "backend", "backend_config"),
            ):
                definition_key = "model_type" if section == "models" else "backend_type"
                selected = {b["if"]["properties"][selector]["const"]: b["then"]
                            for b in branches if selector in b["if"]["properties"]}
                for definition in artifacts["catalog"][section]:
                    with self.subTest(tool=tool, definition=definition[definition_key]):
                        body = selected[definition[definition_key]]
                        self.assert_config_matches_catalog(
                            body["properties"][config_key], definition["config_fields"])
                        self.assertEqual(config_key in body.get("required", []),
                                         any(f["required"] for f in definition["config_fields"]))
                        if section == "models":
                            self.assertNotIn("capability", body["properties"])

    def test_complete_deployment_envelope_is_available(self):
        for tool, artifacts in self.builds:
            with self.subTest(tool=tool):
                deployment = artifacts["export-schema"]["properties"]["deployment"]
                self.assertEqual(set(deployment["properties"]), {"io"})
                self.assertEqual(deployment["required"], ["io"])
                io = deployment["properties"]["io"]
                self.assertEqual(io["required"], ["io_binding"])
                self.assertCountEqual(io["properties"]["io_binding"]["enum"],
                                      [b["binding_id"] for b in artifacts["catalog"]["io_bindings"]])
                allocation = io["properties"]["out_mem"]["additionalProperties"]
                self.assertEqual(set(allocation["properties"]),
                                 {"allocator", "params", "meta_num",
                                  "metadata_type_id", "capacities"})
                self.assertEqual(allocation["required"], [])
                self.assertFalse(allocation["additionalProperties"])
                # Array and scalar params are interpreted by the allocator.
                params = allocation["properties"]["params"]
                self.assertNotIn("type", params)
                self.assertNotIn("enum", params)
                capacity = allocation["properties"]["capacities"]["additionalProperties"]
                self.assertEqual(capacity["type"], "integer")
                self.assertEqual(capacity["minimum"], 1)

    def test_editor_associates_schema_without_changing_pipeline_documents(self):
        settings = json.loads((ROOT / "edgeflow.code-workspace").read_text())["settings"]
        associations = settings["json.schemas"]
        import fnmatch
        representative = "/configs/pipeline_keyword_match_rules.json"
        self.assertTrue(any(
            a.get("url") and any(fnmatch.fnmatchcase(representative, pattern)
                                 for pattern in a["fileMatch"])
            for a in associations))
        paths = list((ROOT / "configs").glob("pipeline_*.json"))
        paths += list((ROOT / "demo/fixtures").rglob("pipeline_*.json"))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(path=path):
                self.assertNotIn("$schema", json.loads(path.read_text()))


if __name__ == "__main__":
    unittest.main()
