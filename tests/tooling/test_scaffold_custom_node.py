#!/usr/bin/env python3
"""Generator CLI/file contracts. Generated C++ also compiles in the Node runner."""
import importlib.util
import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/scaffold_custom_node.py"
SPEC = importlib.util.spec_from_file_location("scaffold", SCRIPT)
SCAFFOLD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SCAFFOLD)


class ScaffoldCustomNodeTest(unittest.TestCase):
    def run_cli(self, *args, env=None):
        return subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True, env=env)

    def _setup_mock_repo(self, temp):
        temp_root = Path(temp)
        custom_nodes_dir = temp_root / "src" / "custom_nodes"
        custom_nodes_dir.mkdir(parents=True, exist_ok=True)
        cmake_src = (ROOT / "src/custom_nodes/CMakeLists.txt").read_text(encoding="utf-8")
        (custom_nodes_dir / "CMakeLists.txt").write_text(cmake_src, encoding="utf-8")

        cmake_ext_dir = temp_root / "cmake_ext"
        cmake_ext_dir.mkdir(parents=True, exist_ok=True)
        cmake_test = (ROOT / "cmake_ext/CustomNodeTests.cmake").read_text(encoding="utf-8")
        (cmake_ext_dir / "CustomNodeTests.cmake").write_text(cmake_test, encoding="utf-8")

        (temp_root / "tests" / "unit" / "nodes").mkdir(parents=True, exist_ok=True)
        return dict(os.environ, LLM_EDGEFLOW_REPO_ROOT=str(temp_root))

    def test_port_cardinality_is_not_split_into_provenance(self):
        for spec, expected in [
            ("input:TextBatch", ("input", "TextBatch", "1:1", "preserve")),
            ("input:TextBatch:1:1:preserve", ("input", "TextBatch", "1:1", "preserve")),
            ("chunks:TextBatch:1:N:generate_sub_id", ("chunks", "TextBatch", "1:N", "generate_sub_id")),
            ("context:TextBatch:N:1:aggregate", ("context", "TextBatch", "N:1", "aggregate")),
        ]:
            self.assertEqual(SCAFFOLD.parse_port_spec(spec), expected)
        for spec in ["", "input", "input:TextBatch:1", "i:TextBatch:1:1:preserve:extra", 'bad"name:TextBatch']:
            with self.assertRaises(ValueError):
                SCAFFOLD.parse_port_spec(spec)

    def test_cli_rejects_unsupported_signatures_before_writing(self):
        with tempfile.TemporaryDirectory() as temp:
            for args in [
                ["--kind", "unary_inference", "-m", "ocr"],
                ["--kind", "model", "-m", "asr", "--in-port", "i:TextBatch"],
                ["--kind", "model", "--out-port", "o:TextBatch:1:N:generate_sub_id"],
                ["-m", "llm"],
                ["--in-port", "input:TextBatch:1"],
            ]:
                result = self.run_cli("RejectedNode", "--output-dir", temp, *args)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertEqual(list(Path(temp).iterdir()), [])

    def test_file_registration_overwrite_and_dry_run(self):
        with tempfile.TemporaryDirectory() as temp:
            cmake = Path(temp) / "CMakeLists.txt"
            original_cmake = (ROOT / "src/custom_nodes/CMakeLists.txt").read_text()
            cmake.write_text(original_cmake)
            args = ["ExampleNode", "--output-dir", temp, "--add-to-cmake", "--generate-test"]
            dry = self.run_cli(*args, "--dry-run")
            self.assertEqual(dry.returncode, 0, dry.stderr)
            self.assertEqual(len(list(Path(temp).iterdir())), 1)
            result = self.run_cli(*args)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("TEST(CustomNodeCatalogTest", result.stdout)
            node = Path(temp) / "example_node.cpp"
            node.write_text("user changes")
            self.assertNotEqual(self.run_cli(*args).returncode, 0)
            self.assertEqual(node.read_text(), "user changes")
            self.assertEqual(self.run_cli(*args, "--force").returncode, 0)
            self.assertEqual(cmake.read_text().count("example_node.cpp"), 1)
            self.assertEqual(cmake.read_text().replace("  example_node.cpp\n", ""), original_cmake)

    def test_bad_cmake_does_not_leave_partial_source(self):
        with tempfile.TemporaryDirectory() as temp:
            result = self.run_cli("ExampleNode", "--output-dir", temp, "--add-to-cmake")
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((Path(temp) / "example_node.cpp").exists())

    def test_cpp_string_escaping_and_stateless_default(self):
        result = self.run_cli("Example", "--description", '引号 " and \\ newline\n', "--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('def.description = "引号 \\" and \\\\ newline\\n";', result.stdout)
        self.assertIn("def.parallel_safe = false", result.stdout)
        self.assertNotIn('"adapter/', result.stdout)

    def test_llm_starter_is_the_actual_generator_template(self):
        source = SCAFFOLD.STARTER_LLM_TEMPLATE.read_text(encoding="utf-8")
        generated = SCAFFOLD.render_model_node(
            "StarterLlmNode", "LLM authoring starter", "llm",
            ("input", "TextBatch", "1:1", "preserve"),
            ("output", "TextBatch", "1:1", "preserve"))
        self.assertEqual(generated, source)
        result = self.run_cli("SwappedNode", "--kind", "model", "-m", "llm",
                              "--in-port", "output:TextBatch", "--out-port", "input:TextBatch",
                              "--description", 'StarterLlmNode "input"\n', "--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('kInput = MakeBlackboardKey<TextBatch>("output");', result.stdout)
        self.assertIn('kOutput = MakeBlackboardKey<TextBatch>("input");', result.stdout)
        self.assertIn('MakeCustomModelNodeDefinition<SwappedNode>', result.stdout)
        self.assertIn('class SwappedNode final', result.stdout)

    def test_control_starter_and_business_test_generation(self):
        generated = SCAFFOLD.render_node(
            "StarterControlNode", "Control authoring starter", "compute", None,
            ("input", "TextBatch", "1:1", "preserve"),
            ("output", "TextBatch", "1:1", "preserve"), 1001)
        self.assertEqual(generated, SCAFFOLD.STARTER_CONTROL_TEMPLATE.read_text())
        result = self.run_cli("PrefixNode", "--control-id", "12345", "--dry-run",
                              "--in-port", "source:TextBatch", "--out-port", "result:TextBatch",
                              "--generate-test")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('kUpdatePrefix = 12345;', result.stdout)
        self.assertIn('ctx.Publish("source"', result.stdout)
        self.assertIn('ctx.Read<TextBatch>("result")', result.stdout)
        self.assertIn('ControlChangesOutputAndPreservesOnFailure', result.stdout)

    def test_control_starter_rejects_invalid_ids_and_non_text_signatures(self):
        with tempfile.TemporaryDirectory() as temp:
            for args in [["--control-id", "0"], ["--control-id", "999"],
                         ["--control-id", "2147483648"],
                         ["--control-id", "1001", "--kind", "model"],
                         ["--control-id", "1001", "--out-port", "out:Int32Batch"]]:
                result = self.run_cli("InvalidNode", "--output-dir", temp, *args)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(list(Path(temp).iterdir()), [])

    def test_write_test_creates_source_and_test_file(self):
        with tempfile.TemporaryDirectory() as temp:
            env = self._setup_mock_repo(temp)
            result = self.run_cli("AwesomeFeatureNode", "--write-test", env=env)
            self.assertEqual(result.returncode, 0, result.stderr)

            # Check custom node source generated under src/custom_nodes/awesome_feature_node.cpp
            source_file = Path(temp) / "src" / "custom_nodes" / "awesome_feature_node.cpp"
            self.assertTrue(source_file.exists())
            source_content = source_file.read_text(encoding="utf-8")
            self.assertIn("class AwesomeFeatureNode final", source_content)
            self.assertIn("AwesomeFeatureNode::kNodeType", source_content)

            # Check test file generated under tests/unit/nodes/test_awesome_feature_node.cpp
            test_file = Path(temp) / "tests" / "unit" / "nodes" / "test_awesome_feature_node.cpp"
            self.assertTrue(test_file.exists())
            test_content = test_file.read_text(encoding="utf-8")
            self.assertIn("TEST(CustomNodeCatalogTest, AwesomeFeatureNode_RegistrationAndInstantiation)", test_content)
            self.assertIn("TEST(CustomNodeCatalogTest, AwesomeFeatureNode_BusinessExample)", test_content)

            # Output reporting
            self.assertIn(f"Created {source_file}", result.stdout)
            self.assertIn(f"Created {test_file}", result.stdout)
            self.assertIn("Pending registrations:", result.stdout)
            self.assertIn("awesome_feature_node.cpp", result.stdout)
            self.assertIn("test_awesome_feature_node.cpp", result.stdout)

            # Without --add-to-cmake, CMake files should NOT be modified
            cmake_file = Path(temp) / "src" / "custom_nodes" / "CMakeLists.txt"
            test_cmake_file = Path(temp) / "cmake_ext" / "CustomNodeTests.cmake"
            self.assertNotIn("awesome_feature_node.cpp", cmake_file.read_text(encoding="utf-8"))
            self.assertNotIn("test_awesome_feature_node.cpp", test_cmake_file.read_text(encoding="utf-8"))

    def test_write_test_add_to_cmake_registers_both_files(self):
        with tempfile.TemporaryDirectory() as temp:
            env = self._setup_mock_repo(temp)
            result = self.run_cli("AwesomeFeatureNode", "--write-test", "--add-to-cmake", env=env)
            self.assertEqual(result.returncode, 0, result.stderr)

            # Verify files were created
            source_file = Path(temp) / "src" / "custom_nodes" / "awesome_feature_node.cpp"
            test_file = Path(temp) / "tests" / "unit" / "nodes" / "test_awesome_feature_node.cpp"
            self.assertTrue(source_file.exists())
            self.assertTrue(test_file.exists())

            # Verify source registered in src/custom_nodes/CMakeLists.txt
            cmake_file = Path(temp) / "src" / "custom_nodes" / "CMakeLists.txt"
            cmake_content = cmake_file.read_text(encoding="utf-8")
            self.assertIn("awesome_feature_node.cpp", cmake_content)
            self.assertRegex(
                cmake_content,
                r"target_sources\(edgeflow_capability_nodes_objects\s+PRIVATE[\s\S]*awesome_feature_node\.cpp",
            )

            # Verify test registered in cmake_ext/CustomNodeTests.cmake
            test_cmake_file = Path(temp) / "cmake_ext" / "CustomNodeTests.cmake"
            test_cmake_content = test_cmake_file.read_text(encoding="utf-8")
            self.assertIn("test_awesome_feature_node.cpp", test_cmake_content)
            self.assertIn(
                '  "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_awesome_feature_node.cpp"',
                test_cmake_content,
            )

            # Verify stdout messages and next commands for default sharded runner
            self.assertIn(f"Registered awesome_feature_node.cpp in {cmake_file}", result.stdout)
            self.assertIn(f"Registered test_awesome_feature_node.cpp in {test_cmake_file}", result.stdout)
            self.assertIn("Next steps:", result.stdout)
            self.assertIn("cmake --build build --target edgeflow_test_nodes_runner", result.stdout)
            self.assertIn("CustomNodeCatalogTest.AwesomeFeatureNode_*", result.stdout)
            self.assertIn("ctest --test-dir build -R CommonNodesTest", result.stdout)
            self.assertIn('./build/edgeflow_test_nodes_runner --gtest_filter="CustomNodeCatalogTest.AwesomeFeatureNode_*"', result.stdout)

            # Test individual runner mode if CMakeCache specifies it
            cache_file = Path(temp) / "build" / "CMakeCache.txt"
            cache_file.parent.mkdir(parents=True, exist_ok=True)
            cache_file.write_text("LLM_EDGEFLOW_SHARDED_TEST_RUNNERS:BOOL=OFF\n", encoding="utf-8")
            result_ind = self.run_cli("IndividualNode", "--write-test", "--add-to-cmake", env=env)
            self.assertEqual(result_ind.returncode, 0, result_ind.stderr)
            self.assertIn("cmake --build build --target test_common_nodes", result_ind.stdout)
            self.assertIn("CustomNodeCatalogTest.IndividualNode_*", result_ind.stdout)
            self.assertIn('./build/test_common_nodes --gtest_filter="CustomNodeCatalogTest.IndividualNode_*"', result_ind.stdout)

    def test_write_test_dry_run_does_not_create_files(self):
        with tempfile.TemporaryDirectory() as temp:
            env = self._setup_mock_repo(temp)
            cmake_file = Path(temp) / "src" / "custom_nodes" / "CMakeLists.txt"
            test_cmake_file = Path(temp) / "cmake_ext" / "CustomNodeTests.cmake"
            orig_cmake = cmake_file.read_text(encoding="utf-8")
            orig_test_cmake = test_cmake_file.read_text(encoding="utf-8")

            result = self.run_cli(
                "DryRunNode", "--write-test", "--dry-run", "--add-to-cmake", env=env
            )
            self.assertEqual(result.returncode, 0, result.stderr)

            # Check preview output
            source_file = Path(temp) / "src" / "custom_nodes" / "dry_run_node.cpp"
            test_file = Path(temp) / "tests" / "unit" / "nodes" / "test_dry_run_node.cpp"
            self.assertIn(f"--- {source_file} (new file) ---", result.stdout)
            self.assertIn("class DryRunNode final", result.stdout)
            self.assertIn(f"--- {test_file} (new test file) ---", result.stdout)
            self.assertIn("TEST(CustomNodeCatalogTest, DryRunNode_", result.stdout)
            self.assertIn(f"--- {cmake_file} registration ---", result.stdout)
            self.assertIn("+  dry_run_node.cpp", result.stdout)
            self.assertIn(f"--- {test_cmake_file} registration ---", result.stdout)
            self.assertIn("+  test_dry_run_node.cpp", result.stdout)

            # Verify no files were created or modified
            self.assertFalse(source_file.exists())
            self.assertFalse(test_file.exists())
            self.assertEqual(cmake_file.read_text(encoding="utf-8"), orig_cmake)
            self.assertEqual(test_cmake_file.read_text(encoding="utf-8"), orig_test_cmake)

    def test_write_test_rejects_incompatible_flags_and_custom_output_dir(self):
        with tempfile.TemporaryDirectory() as temp:
            env = self._setup_mock_repo(temp)
            custom_dir = Path(temp) / "custom_dir"
            custom_dir.mkdir()

            cases = [
                (["--generate-test"], "--write-test and --generate-test cannot be used together"),
                (["--force"], "--write-test rejects --force to prevent multi-file overwrite"),
                (["--output-dir", str(custom_dir)], "--write-test requires the standard source directory src/custom_nodes"),
            ]

            for extra_flags, expected_error in cases:
                result = self.run_cli(
                    "RejectedNode", "--write-test", *extra_flags, env=env
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected_error, result.stderr)

            # Ensure no files were touched in any directory
            source_file = Path(temp) / "src" / "custom_nodes" / "rejected_node.cpp"
            test_file = Path(temp) / "tests" / "unit" / "nodes" / "test_rejected_node.cpp"
            self.assertFalse(source_file.exists())
            self.assertFalse(test_file.exists())
            self.assertEqual(list(custom_dir.iterdir()), [])

    def test_write_test_atomic_rollback_on_conflict(self):
        with tempfile.TemporaryDirectory() as temp:
            env = self._setup_mock_repo(temp)
            cmake_file = Path(temp) / "src" / "custom_nodes" / "CMakeLists.txt"
            test_cmake_file = Path(temp) / "cmake_ext" / "CustomNodeTests.cmake"
            orig_cmake = cmake_file.read_text(encoding="utf-8")
            orig_test_cmake = test_cmake_file.read_text(encoding="utf-8")

            # Case 1: Target source file already exists
            conflict_source = Path(temp) / "src" / "custom_nodes" / "conflict_source_node.cpp"
            conflict_source.write_text("existing custom node source", encoding="utf-8")
            res_source_conflict = self.run_cli(
                "ConflictSourceNode", "--write-test", "--add-to-cmake", env=env
            )
            self.assertNotEqual(res_source_conflict.returncode, 0)
            self.assertIn("Target already exists", res_source_conflict.stderr)
            self.assertEqual(conflict_source.read_text(encoding="utf-8"), "existing custom node source")
            self.assertFalse((Path(temp) / "tests" / "unit" / "nodes" / "test_conflict_source_node.cpp").exists())
            self.assertEqual(cmake_file.read_text(encoding="utf-8"), orig_cmake)
            self.assertEqual(test_cmake_file.read_text(encoding="utf-8"), orig_test_cmake)

            # Case 2: Target test file already exists
            conflict_test = Path(temp) / "tests" / "unit" / "nodes" / "test_conflict_test_node.cpp"
            conflict_test.write_text("existing custom node test", encoding="utf-8")
            res_test_conflict = self.run_cli(
                "ConflictTestNode", "--write-test", "--add-to-cmake", env=env
            )
            self.assertNotEqual(res_test_conflict.returncode, 0)
            self.assertIn("Target already exists", res_test_conflict.stderr)
            self.assertEqual(conflict_test.read_text(encoding="utf-8"), "existing custom node test")
            self.assertFalse((Path(temp) / "src" / "custom_nodes" / "conflict_test_node.cpp").exists())
            self.assertEqual(cmake_file.read_text(encoding="utf-8"), orig_cmake)
            self.assertEqual(test_cmake_file.read_text(encoding="utf-8"), orig_test_cmake)

            # Case 3: Registration error during CMake modification (corrupted CMakeLists.txt)
            cmake_file.write_text("# Corrupted cmake with missing target_sources block\n", encoding="utf-8")
            res_corrupt = self.run_cli("CorruptCmakeNode", "--write-test", "--add-to-cmake", env=env)
            self.assertNotEqual(res_corrupt.returncode, 0)
            self.assertIn("No edgeflow_capability_nodes_objects target_sources", res_corrupt.stderr)
            self.assertFalse((Path(temp) / "src" / "custom_nodes" / "corrupt_cmake_node.cpp").exists())
            self.assertFalse((Path(temp) / "tests" / "unit" / "nodes" / "test_corrupt_cmake_node.cpp").exists())
            self.assertEqual(test_cmake_file.read_text(encoding="utf-8"), orig_test_cmake)

            # Case 4: ChangePlan unit rollback on commit failure
            plan = SCAFFOLD.ChangePlan()
            new_file = Path(temp) / "src" / "custom_nodes" / "rollback_probe.cpp"
            mod_target = test_cmake_file
            mod_orig = mod_target.read_text(encoding="utf-8")
            plan.add_new_file(new_file, "// rollback probe content")
            plan.add_modification(mod_target, mod_orig, mod_orig + "\n# Modified\n")
            # Simulate concurrent modification to mod_target before commit
            mod_target.write_text(mod_orig + "\n# Concurrent user edit\n", encoding="utf-8")
            with self.assertRaises(RuntimeError):
                plan.commit()
            # Verify new_file was unlinked and mod_target was not clobbered
            self.assertFalse(new_file.exists())
            self.assertEqual(mod_target.read_text(encoding="utf-8"), mod_orig + "\n# Concurrent user edit\n")

    def test_atomic_install_preserves_destination_created_at_final_syscall(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            previous, target = root / "previous.cpp", root / "new.cpp"
            plan = SCAFFOLD.ChangePlan()
            plan.add_new_file(previous, "generated before conflict")
            plan.add_new_file(target, "generated source")
            real_link = os.link

            def competing_link(source, destination, *args, **kwargs):
                if Path(destination) == target:
                    target.write_text("concurrent user source", encoding="utf-8")
                return real_link(source, destination, *args, **kwargs)

            with mock.patch.object(os, "link", side_effect=competing_link):
                with self.assertRaises((OSError, ValueError, RuntimeError)):
                    plan.commit()
            self.assertEqual(target.read_text(), "concurrent user source")
            self.assertFalse(previous.exists(), "Earlier generated file must be rolled back")
            self.assertFalse(list(root.glob("*.tmp")))

    def test_preexisting_temporary_file_and_user_edited_rollback_survive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target, registration = root / "new.cpp", root / "CMakeLists.txt"
            temporary = root / "new.cpp.tmp"
            temporary.write_text("user scratch", encoding="utf-8")
            registration.write_text("original", encoding="utf-8")
            plan = SCAFFOLD.ChangePlan()
            plan.add_new_file(target, "generated")
            plan.add_modification(registration, "original", "registered")
            plan.commit()
            target.write_text("user source edit", encoding="utf-8")
            registration.write_text("user registration edit", encoding="utf-8")
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                plan.rollback()
            self.assertEqual(temporary.read_text(), "user scratch")
            self.assertEqual(target.read_text(), "user source edit")
            self.assertEqual(registration.read_text(), "user registration edit")
            self.assertIn(str(target), errors.getvalue())
            self.assertIn(str(registration), errors.getvalue())

    def test_failed_second_registration_preserves_concurrent_first_registration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first, second = root / "first.cmake", root / "second.cmake"
            first.write_text("first original", encoding="utf-8")
            second.write_text("second original", encoding="utf-8")
            plan = SCAFFOLD.ChangePlan()
            plan.add_modification(first, "first original", "first generated")
            plan.add_modification(second, "second original", "second generated")
            original_replace = os.replace

            def edit_before_second_capture(source, destination, *args, **kwargs):
                if Path(source) == second:
                    first.write_text("first user edit", encoding="utf-8")
                    second.write_text("second user edit", encoding="utf-8")
                return original_replace(source, destination, *args, **kwargs)

            with mock.patch.object(os, "replace", side_effect=edit_before_second_capture):
                with self.assertRaises((OSError, ValueError, RuntimeError)):
                    plan.commit()
            self.assertEqual(first.read_text(), "first user edit")
            self.assertEqual(second.read_text(), "second user edit")


if __name__ == "__main__":
    unittest.main()
