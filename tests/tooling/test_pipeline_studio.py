#!/usr/bin/env python3
"""API and filesystem boundary tests for the local Pipeline Studio."""

import importlib.util
import json
import os
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
        if hasattr(self.service, "tool_fingerprint"):
            try:
                fp = self.service.tool_fingerprint
                return fp() if callable(fp) else fp
            except Exception:
                pass
        if hasattr(SHOW, "tool_fingerprint"):
            try:
                fp = SHOW.tool_fingerprint
                return fp() if callable(fp) else fp
            except Exception:
                pass
        return "valid_tool_fingerprint"

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

        bad_patch = [{"op": "test", "path": "/biz_name", "value": "mismatched_biz"}]
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

    def test_saved_pair_runs_the_selected_pipeline_with_explicit_arguments(self):
        self.keyword["pipeline"][0]["config"]["categories"] = {"SAVED_RULE": ["VIP"]}
        filename = f"pipeline_saved_{uuid.uuid4().hex}.json"
        saved = self.service.save_solution(filename, self.keyword, "keyword_match_rules")
        self.assertEqual(json.loads((self.configs / filename).read_text()), self.keyword)
        conf = json.loads((self.configs / saved["conf_filename"]).read_text())
        self.assertEqual(conf["data"]["pipe_path"], str((self.configs / filename).relative_to(ROOT)))
        self.assertEqual(conf["data"]["model_paths"], {})
        command = shlex.split(saved["command"])
        self.assertEqual(command[:3], ["cd", str(ROOT), "&&"])
        self.assertIn("--no-default-control", command)
        self.assertFalse(Path(command[command.index("--config") + 1]).is_absolute())
        output = Path(command[command.index("--output-dir") + 1])
        try:
            process = subprocess.run(command[3:], cwd=command[1], text=True, capture_output=True, timeout=30)
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            records = [json.loads(line) for line in (output / "keyword_match/results.jsonl").read_text().splitlines()]
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

    def test_conf_rebuilds_selected_model_paths_and_honors_explicit_root(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        selected = pipeline["models"][0]["model_path"]
        saved = self.service.save_solution("pipeline_fixture.json", pipeline, "entity_extract_mock", ".")
        self.assertEqual(saved["conf"]["data"]["model_paths"], {"entity_llm": selected})
        pipeline["models"][0]["model_path"] = "replacement.gguf"
        saved = self.service.save_solution("pipeline_replaced.json", pipeline, "entity_extract_mock", "models")
        self.assertEqual(saved["conf"]["data"]["model_paths"], {"entity_llm": "models/replacement.gguf"})
        self.assertEqual(json.loads((self.configs / "pipeline_replaced.json").read_text()), pipeline)

    def test_ordinary_save_updates_managed_model_paths_and_node_parameters(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        saved = self.service.save_solution("pipeline_paired.json", pipeline, "entity_extract_mock", "models")
        pipeline["models"][0]["model_path"] = "replacement.gguf"
        pipeline["models"][0]["model_id"] = "replacement_model"
        pipeline["pipeline"][1]["config"]["bind_model"] = "replacement_model"
        pipeline["pipeline"][1]["config"]["max_tokens"] = 17
        updated = self.service.save_pipeline(saved["filename"], pipeline, saved["revision"])
        self.assertEqual(updated["command"], saved["command"])
        self.assertEqual(updated["conf"]["data"]["model_paths"], {"replacement_model": "models/replacement.gguf"})
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

    def test_restarted_service_rejects_model_changes_shadowed_by_unmanaged_conf(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        saved = self.service.save_solution("pipeline_restart.json", pipeline, "entity_extract_mock", ".")
        restarted = SHOW.WorkbenchService(self.configs)
        pipeline["pipeline"][1]["config"]["max_tokens"] = 17
        updated = restarted.save_pipeline(saved["filename"], pipeline, saved["revision"])
        paths = [self.configs / saved["filename"], self.configs / saved["conf_filename"]]
        before = {path: path.read_bytes() for path in paths}
        pipeline["models"][0]["model_path"] = "replacement.gguf"
        with self.assertRaises(SHOW.StudioError) as error:
            restarted.save_pipeline(saved["filename"], pipeline, updated["revision"])
        self.assertEqual(error.exception.code, "DEPLOYMENT_CONFLICT")
        self.assertIn("pipeline_restart.conf", str(error.exception))
        self.assertEqual({path: path.read_bytes() for path in paths}, before)
        self.assertFalse(restarted.generated_solutions)

    def test_draft_uses_the_same_selected_paths_under_project_root_and_cleans_up(self):
        pipeline = json.loads((ROOT / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        observed = {}
        original_popen = SHOW.subprocess.Popen
        def inspect_launch(args, **kwargs):
            if "--config" in args:
                conf_path = ROOT / args[args.index("--config") + 1]
                observed["directory"] = conf_path.parent
                observed["conf"] = json.loads(conf_path.read_text())
            return original_popen(args, **kwargs)
        with mock.patch.object(SHOW.subprocess, "Popen", side_effect=inspect_launch):
            started = self.service.start_run(pipeline, "entity_extract_mock", ".")
            for _ in range(200):
                job = self.service.run_status(started["job_id"])["job"]
                if job["status"] in ("completed", "failed", "cancelled") and "directory" in observed and not observed["directory"].exists():
                    break
                time.sleep(0.05)
        self.assertEqual(job["status"], "completed", job)
        self.assertEqual(observed["conf"]["data"]["model_paths"], {"entity_llm": pipeline["models"][0]["model_path"]})
        self.assertTrue(observed["conf"]["data"]["pipe_path"].startswith("build/"))
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
        profile, mem_que = self.service.profile_inputs(self.keyword, "keyword_match_rules")
        mem_que["capacities"]["match_result_json"] = 0
        with mock.patch.object(self.service, "profile_inputs", return_value=(profile, mem_que)):
            with self.assertRaises(SHOW.StudioError) as error:
                self.service.save_solution("pipeline_invalid_pool.json", self.keyword, "keyword_match_rules")
        self.assertEqual(error.exception.code, "DEPLOYMENT_VALIDATION_FAILED")
        self.assertIn("match_result_json", str(error.exception))
        self.assertEqual(list(self.configs.iterdir()), [])


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

    def test_init_raw_can_be_saved_and_validated_without_unwrapping(self):
        args = ["init", "--biz", "keyword_match_v1", "--profile", "keyword_match_rules"]
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
            [str(PIPELINE_TOOL), "init", "--raw", "-b", "keyword_match_v1", "--empty"],
            text=True, capture_output=True, cwd=ROOT, check=False,
        )
        self.assertEqual(empty.returncode, 0, empty.stderr)
        self.assertEqual(json.loads(empty.stdout), {
            "biz_name": "keyword_match_v1", "models": [], "pipeline": []
        })

    def test_init_rejects_invalid_options(self):
        for options in (
            ["--profile", "keyword_match_rules", "--empty"],
            ["--profile"], ["--profile", "--raw"], ["--unknown"],
            ["--raw", "--raw"], ["--empty", "--empty"],
            ["--biz", "keyword_match_v1"],
            ["--profile", "keyword_match_rules", "--profile", "keyword_match_rules"],
        ):
            with self.subTest(options=options):
                process = subprocess.run(
                    [str(PIPELINE_TOOL), "init", "--biz", "keyword_match_v1", *options],
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
        self.assertEqual(configuration["conf_path"], str(conf_path))
        self.assertEqual(configuration["model_paths"], [{
            "model_id": "entity_llm", "source": "conf.data.model_paths",
            "resolved": str(ROOT / "models/qwen_0_6b_npu.bin"),
        }])
        llm_config = configuration["effective_pipeline"]["pipeline"][1]["config"]
        self.assertEqual(llm_config["max_tokens"], 64)
        self.assertEqual(llm_config["top_p"], 0.9, "omitted defaults must come from the native validated plan")
        conf = json.loads(conf_path.read_text())
        with tempfile.TemporaryDirectory(prefix="resolve-conf-", dir=ROOT / "build") as directory:
            changed = Path(directory) / "pipeline.conf"
            conf["data"].pop("model_paths")
            changed.write_text(json.dumps(conf))
            code, direct = self.command("resolve-conf", str(changed.relative_to(ROOT)), "--root", str(ROOT))
            self.assertEqual(code, 0, direct)
            self.assertEqual(direct["configuration"]["model_paths"][0]["source"], "pipeline.models.model_path")
            conf["data"]["mem_que"]["capacities"]["entities_json"] = 0
            changed.write_text(json.dumps(conf))
            code, rejected = self.command("resolve-conf", str(changed.relative_to(ROOT)), "--root", str(ROOT))
            self.assertEqual(code, 1)
            self.assertFalse(rejected["ok"])
            self.assertEqual(rejected["diagnostics"][0]["code"], "DEPLOYMENT_CONFIG")
            self.assertIn("entities_json", rejected["diagnostics"][0]["message"])

    def test_native_viewer_preserves_explicit_dag_dependencies(self):
        process = subprocess.run(
            [str(ALG_SHOW), str(ROOT / "configs" / "pipeline_doc_qa_default.json")],
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

    def test_startup_endpoints_load_rerank_pipeline_and_refresh_saved_list(self):
        path = ROOT / "configs/pipeline_doc_qa_rerank_cpu.json"
        pipeline = json.loads(path.read_text())
        managed = Path(self.temporary.name) / path.name
        managed.write_text(json.dumps(pipeline))
        self.service.initial_document = self.service.open_pipeline(path.name)
        for endpoint in ("/catalog", "/profiles", "/pipelines", "/assets", "/initial",
                         "/catalog?biz=smart_doc_qa_v1"):
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
    await api("/catalog?biz=smart_doc_qa_v1");
    assert.equal(requested, base + "api/v1/catalog?biz=smart_doc_qa_v1");
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
    def test_actual_fix_handler_history_and_stale_response_protection(self):
        process = subprocess.run(
            [shutil.which("node"), "--experimental-vm-modules",
             str(Path(__file__).with_name("studio_fix_workflow_test.mjs"))],
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

if __name__ == "__main__":
    unittest.main()
