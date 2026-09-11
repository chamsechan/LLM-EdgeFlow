#!/usr/bin/env python3
"""Recipe contracts with the real native Catalog, Validator and Demo.

Only incremental-build and focused-test boundaries are stubbed when exercising
their failure decisions. This suite never starts a competing CMake build.
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
TOOL = Path(os.environ.get("LLM_EDGEFLOW_PIPELINE_TOOL", ROOT / "build/alg_pipeline_tool_test"))
DEMO = Path(os.environ.get("LLM_EDGEFLOW_DEMO_BINARY", ROOT / "build/alg_demo"))
SPEC = importlib.util.spec_from_file_location("dev_recipe", ROOT / "scripts/dev_recipe.py")
RECIPE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RECIPE)


class DevRecipeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="edgeflow-recipe-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for relative in ("configs", "demo/fixtures", "data", "tests/fixtures"):
            shutil.copytree(ROOT / relative, self.root / relative)
        for relative in ("demo/profiles.json", "src/custom_nodes/CMakeLists.txt",
                         "cmake_ext/CustomNodeTests.cmake", "models/asset_manifest.json"):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        self.build = self.root / "build"
        self.build.mkdir()
        self.tool = self.build / TOOL.name
        self.demo = self.build / "alg_demo"
        self.link_binary(TOOL, self.tool)
        self.link_binary(DEMO, self.demo)
        for library in DEMO.parent.glob("libcompany_alg_sdk.*"):
            self.link_binary(library.resolve(), self.build / library.name)
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}\nLLM_EDGEFLOW_SHARDED_TEST_RUNNERS:BOOL=ON\n")
        self.target = self.root / "configs/pipeline_recipe_contract.json"

    @staticmethod
    def link_binary(source, target):
        if not source.is_file():
            raise AssertionError(f"Build the recipe test prerequisite: {source}")
        try:
            os.link(source.resolve(), target)
        except OSError:
            shutil.copy2(source, target)

    def prepare(self, kind="prompt-config", profile="keyword_match_rules", **options):
        function = RECIPE.prepare_prompt_config if kind == "prompt-config" else RECIPE.prepare_text_llm_node
        arguments = dict(name="RecipeContractNode", profile_name=profile, tool_path=self.tool,
                         build_dir=self.build, pipeline_target=self.target, root=self.root)
        arguments.update(options)
        return function(**arguments)

    def verify(self, kind="prompt-config", **options):
        arguments = dict(recipe=kind, pipeline_path=self.target, tool_path=self.tool,
                         build_dir=self.build, effects_path=self.target.with_name(self.target.stem + "_effects.json"),
                         model_root=self.root, demo_path=self.demo,
                         manifest_path=self.root / "tests/fixtures/asset_manifest_test.json", root=self.root)
        arguments.update(options)
        return RECIPE.verify_recipe(**arguments)

    def assert_prepare_rejected_without_writes(self, **options):
        def files():
            return {str(p.relative_to(self.root)): p.read_bytes() for p in self.root.rglob("*")
                    if p.is_file() and self.build not in p.parents}
        before = files()
        try:
            report = self.prepare(**options)
        except (ValueError, RuntimeError):
            pass
        else:
            self.assertFalse(report["ok"], report)
            self.assertIsNotNone(report["failed_step"])
        self.assertEqual(before, files(), "Rejected preparation must not publish or alter files")

    def test_list_recipes_has_stable_machine_report(self):
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            self.assertEqual(RECIPE.list_recipes(True), 0)
        report = json.loads(stream.getvalue())
        self.assertEqual(report["schema_version"], 1)
        self.assertTrue(report["ok"])
        self.assertEqual(set(report["recipes"]), {"prompt-config", "text-llm-node"})

    def test_keyword_prepare_validate_and_real_demo_evaluation(self):
        report = self.prepare()
        self.assertTrue(report["ok"], report)
        effects = json.loads(self.target.with_name(self.target.stem + "_effects.json").read_text())
        self.assertEqual([sample["request_id"] for sample in effects["samples"]], [20001, 20002, 20003, 20004])
        self.assertEqual(effects["samples"][0]["expected"], {"/output/is_hit": True})
        self.assertTrue((self.target.parent / effects["dataset"]).is_file())
        result = self.verify()
        self.assertTrue(result["ok"], result)
        self.assertIn("evaluate", result["completed_steps"])

    def test_entity_prompt_uses_pinned_assets_and_real_business_expectation(self):
        report = self.prepare(profile="entity_extract_custom_mock")
        self.assertTrue(report["ok"], report)
        conf = json.loads(self.target.with_suffix(".conf").read_text())
        self.assertEqual(conf["data"]["model_paths"]["entity_llm"],
                         "demo/fixtures/mock/artifacts/neutral-llm.fixture")
        command = next(item["argv"] for item in report["next_commands"] if "verify" in item["argv"])
        self.assertIn("--manifest", command)
        self.assertEqual(Path(command[command.index("--model-root") + 1]), self.root)
        result = self.verify()
        self.assertTrue(result["ok"], result)

    def test_text_preparation_remaps_native_llm_ports_and_keeps_upstream_prompt(self):
        report = self.prepare(kind="text-llm-node", profile="entity_extract_mock")
        self.assertTrue(report["ok"], report)
        document = json.loads(self.target.read_text())
        generated = next(node for node in document["pipeline"] if node["node_type"] == "RecipeContractNode")
        self.assertEqual(generated["ports"]["inputs"], {"input": "prompt_text"})
        self.assertEqual(generated["ports"]["outputs"], {"output": "llm_raw_answer"})
        self.assertEqual(generated["depends_on"], ["node_0_TextTemplateNode"])
        self.assertEqual(generated["config"], {"bind_model": "entity_llm"})
        self.assertTrue((self.root / "src/custom_nodes/recipe_contract_node.cpp").is_file())
        self.assertTrue((self.root / "tests/unit/nodes/test_recipe_contract_node.cpp").is_file())
        self.assertEqual((self.root / "src/custom_nodes/CMakeLists.txt").read_text().count("recipe_contract_node.cpp"), 1)

    def test_native_invalid_profile_and_unavailable_llm_rejected_before_writing(self):
        self.assert_prepare_rejected_without_writes(kind="text-llm-node", profile="keyword_match_rules")
        self.assert_prepare_rejected_without_writes(kind="text-llm-node", profile="entity_extract_mock", name="invalid-node")
        source = self.root / "configs/pipeline_keyword_match_rules.json"
        invalid = json.loads(source.read_text())
        invalid["pipeline"][0]["node_type"] = "MissingRegisteredNode"
        source.write_text(json.dumps(invalid))
        self.assert_prepare_rejected_without_writes()

    def test_tool_from_different_build_is_rejected(self):
        self.assert_prepare_rejected_without_writes(tool_path=TOOL)

    def test_external_destination_preserves_deployment_and_dataset_paths(self):
        with tempfile.TemporaryDirectory(prefix="edgeflow-recipe-output-") as directory:
            target = Path(directory) / "pipeline_external.json"
            report = self.prepare(pipeline_target=target)
            self.assertTrue(report["ok"], report)
            verified = self.verify(pipeline_path=target,
                                   effects_path=target.with_name(target.stem + "_effects.json"))
            self.assertTrue(verified["ok"], verified)

    def test_multi_output_rejected_before_generation(self):
        conf_path = self.root / "configs/pipeline_keyword_match_rules.conf"
        conf = json.loads(conf_path.read_text())
        conf["data"]["outputs"] = [{"name": "only_one_slot"}]
        conf_path.write_text(json.dumps(conf))
        self.assert_prepare_rejected_without_writes()

    def test_unlabelled_or_duplicate_effects_rejected_before_generation(self):
        source = self.root / "tests/fixtures/effects/keyword_exact.json"
        labelled = json.loads(source.read_text())
        for invalid in (
            dict(labelled, samples=[{"request_id": 20001, "expected": {"/status": 0}}]),
            dict(labelled, samples=[labelled["samples"][0], labelled["samples"][0]]),
            dict(labelled, biz_name="entity_extract_v1"),
        ):
            with self.subTest(spec=invalid):
                source.write_text(json.dumps(invalid))
                self.assert_prepare_rejected_without_writes(effects_path=source)

    def test_repeated_prepare_preserves_user_edit_and_registration_count(self):
        self.assertTrue(self.prepare(kind="text-llm-node", profile="entity_extract_mock")["ok"])
        source = self.root / "src/custom_nodes/recipe_contract_node.cpp"
        source.write_text(source.read_text() + "\n// Developer's business implementation\n")
        self.assert_prepare_rejected_without_writes(kind="text-llm-node", profile="entity_extract_mock")

    def test_missing_effects_or_demo_cannot_report_evaluation_complete(self):
        self.assertTrue(self.prepare()["ok"])
        for options in ({"effects_path": self.root / "missing.json"}, {"demo_path": None},
                        {"demo_path": self.build / "missing_demo"}):
            with self.subTest(options=options):
                result = self.verify(**options)
                self.assertFalse(result["ok"], result)
                self.assertNotIn("evaluate", result["completed_steps"])

    def test_wrong_business_expectation_and_tampered_assets_fail_real_evaluation(self):
        self.assertTrue(self.prepare(profile="entity_extract_mock")["ok"])
        effects_path = self.target.with_name(self.target.stem + "_effects.json")
        effects = json.loads(effects_path.read_text())
        effects["samples"][0]["expected"] = {"/output/entities/nouns": ["incorrect result"]}
        effects_path.write_text(json.dumps(effects))
        self.assertFalse(self.verify()["ok"])
        fixture = self.root / "demo/fixtures/mock/artifacts/neutral-llm.fixture"
        fixture.write_text("changed asset")
        self.assertFalse(self.verify()["ok"])

    def test_node_build_targets_selected_tool_and_can_create_missing_demo(self):
        self.assertTrue(self.prepare(profile="entity_extract_custom_mock")["ok"])
        self.demo.unlink()
        real_run = subprocess.run
        build_commands = []

        def execute(command, *args, **kwargs):
            if command[:2] == ["cmake", "--build"]:
                build_commands.append(command)
                self.link_binary(DEMO, self.demo)
                return subprocess.CompletedProcess(command, 0, "", "")
            if Path(command[0]).name in ("edgeflow_test_nodes_runner", "test_common_nodes"):
                listing = "CustomNodeCatalogTest.\n  PromptGuidedLlmNode_BusinessExample\n"
                output = listing if "--gtest_list_tests" in command else "[  PASSED  ] 1 test.\n"
                return subprocess.CompletedProcess(command, 0, output, "")
            return real_run(command, *args, **kwargs)

        with mock.patch.object(subprocess, "run", side_effect=execute):
            report = self.verify(kind="text-llm-node", name="PromptGuidedLlmNode")
        self.assertTrue(report["ok"], report)
        self.assertTrue(build_commands, "A missing Demo must be built, not rejected before build")
        self.assertIn(self.tool.name, build_commands[0])
        self.assertIn("alg_demo", build_commands[0])

    def test_zero_matched_node_tests_fail_without_evaluation(self):
        self.assertTrue(self.prepare(profile="entity_extract_custom_mock")["ok"])
        real_run = subprocess.run

        def execute(command, *args, **kwargs):
            if command[:2] == ["cmake", "--build"]:
                return subprocess.CompletedProcess(command, 0, "", "")
            if Path(command[0]).name in ("edgeflow_test_nodes_runner", "test_common_nodes"):
                return subprocess.CompletedProcess(command, 0, "[  PASSED  ] 0 tests.\n", "")
            return real_run(command, *args, **kwargs)

        with mock.patch.object(subprocess, "run", side_effect=execute):
            report = self.verify(kind="text-llm-node", name="PromptGuidedLlmNode")
        self.assertFalse(report["ok"], report)
        self.assertNotIn("evaluate", report["completed_steps"])

    def test_node_name_required_for_verification(self):
        self.assertTrue(self.prepare()["ok"])
        result = self.verify(kind="text-llm-node", name=None)
        self.assertFalse(result["ok"], result)
        self.assertNotIn("focused_test", result["completed_steps"])

    def test_manifest_matches_fixture_and_rejects_tampering(self):
        pipeline = json.loads((self.root / "demo/fixtures/mock/pipeline_entity_extract.json").read_text())
        manifest = json.loads((self.root / "tests/fixtures/asset_manifest_test.json").read_text())
        rows = RECIPE.VERIFY_SELECTION.verify_assets(pipeline, self.root, manifest)
        self.assertEqual(rows[0]["status"], "verified")
        (self.root / "demo/fixtures/mock/artifacts/neutral-llm.fixture").write_bytes(b"tampered")
        rows = RECIPE.VERIFY_SELECTION.verify_assets(pipeline, self.root, manifest)
        self.assertEqual(rows[0]["files"][0]["status"], "hash_mismatch")


if __name__ == "__main__":
    unittest.main()
