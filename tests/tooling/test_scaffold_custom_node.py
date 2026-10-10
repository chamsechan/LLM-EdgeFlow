#!/usr/bin/env python3
"""生成器 CLI/文件契约测试。生成的 C++ 也会在 Node runner 中编译。"""
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
SCRIPT = ROOT / "tools/scaffold_custom_node.py"
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
                ["--kind", "model", "-m", "asr", "--in-port", "i:TextBatch"],
                ["--kind", "model", "--out-port", "o:TextBatch:1:N:generate_sub_id"],
                ["-m", "llm"],
                ["--in-port", "input:TextBatch:1"],
            ]:
                result = self.run_cli("rejected", "--output-dir", temp, *args)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertEqual(list(Path(temp).iterdir()), [])

    def test_cli_rejects_invalid_identifiers_before_writing(self):
        with tempfile.TemporaryDirectory() as temp:
            for name in ("", "123", "_bad", "UPPER", "badUpper", "bad-name",
                         "bad.name", "bad__name", "bad_", "../outside", "bad;name"):
                with self.subTest(name=name):
                    result = self.run_cli(name, "--output-dir", temp, "--write-test")
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertIn("node_name must be a snake_case identifier", result.stderr)
                    self.assertEqual(list(Path(temp).iterdir()), [])

    def test_overwrite_and_dry_run(self):
        with tempfile.TemporaryDirectory() as temp:
            args = ["example", "--output-dir", temp]
            dry = self.run_cli(*args, "--dry-run")
            self.assertEqual(dry.returncode, 0, dry.stderr)
            self.assertEqual(list(Path(temp).iterdir()), [])
            result = self.run_cli(*args)
            self.assertEqual(result.returncode, 0, result.stderr)
            # 只有被收集的源码目录才能声明自动编译。
            self.assertIn("the build does not collect it", result.stdout)
            self.assertNotIn("compiled automatically", result.stdout)
            node = Path(temp) / "example_node.cpp"
            node.write_text("user changes")
            self.assertNotEqual(self.run_cli(*args).returncode, 0)
            self.assertEqual(node.read_text(), "user changes")
            self.assertEqual(self.run_cli(*args, "--force").returncode, 0)

    def test_cpp_string_escaping(self):
        result = self.run_cli("example", "--description", '引号 " and \\ newline\n', "--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('.Description("引号 \\" and \\\\ newline\\n");', result.stdout)

    def test_llm_starter_preserves_custom_ports(self):
        result = self.run_cli("swapped", "--kind", "model", "-m", "llm",
                              "--in-port", "output:TextBatch", "--out-port", "input:TextBatch",
                              "--description", 'starter_llm "input"\n', "--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Required("output", &Inputs::input)', result.stdout)
        self.assertIn('PreservedOutput<TextBatch>("input", "output")', result.stdout)

    def test_control_starter_and_business_test_generation(self):
        result = self.run_cli("prefix", "--control-id", "12345", "--dry-run",
                              "--in-port", "source:TextBatch", "--out-port", "result:TextBatch",
                              "--write-test")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('kUpdatePrefix = 12345;', result.stdout)
        self.assertIn('harness.CustomInput("source"', result.stdout)
        self.assertIn('result.Output<TextBatch>("result")', result.stdout)
        self.assertIn('ControlChangesOutputAndPreservesOnFailure', result.stdout)

    def test_control_starter_rejects_invalid_ids_and_non_text_signatures(self):
        with tempfile.TemporaryDirectory() as temp:
            for args in [["--control-id", "0"], ["--control-id", "999"],
                         ["--control-id", "2147483648"],
                         ["--control-id", "1001", "--kind", "model"],
                         ["--control-id", "1001", "--out-port", "out:Int32Batch"]]:
                result = self.run_cli("invalid", "--output-dir", temp, *args)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(list(Path(temp).iterdir()), [])

    def test_write_test_creates_source_and_test_file(self):
        with tempfile.TemporaryDirectory() as temp:
            env = self._setup_mock_repo(temp)
            result = self.run_cli("awesome_feature", "--write-test", env=env)
            self.assertEqual(result.returncode, 0, result.stderr)

            # 检查在 src/custom_nodes/awesome_feature_node.cpp 生成的自定义 Node 源文件
            source_file = Path(temp) / "src" / "custom_nodes" / "awesome_feature_node.cpp"
            self.assertTrue(source_file.exists())

            # 检查在 tests/unit/nodes/test_awesome_feature_node.cpp 生成的测试文件
            test_file = Path(temp) / "tests" / "unit" / "nodes" / "test_awesome_feature_node.cpp"
            self.assertTrue(test_file.exists())
            test_content = test_file.read_text(encoding="utf-8")
            self.assertIn("TEST(CustomNodeCatalogTest, awesome_feature_RegistrationAndInstantiation)", test_content)
            self.assertIn("TEST(CustomNodeCatalogTest, awesome_feature_MapPreservesInputData)", test_content)

            # 检查输出报告
            self.assertIn(f"Created {source_file}", result.stdout)
            self.assertIn(f"Created {test_file}", result.stdout)
            self.assertIn("compiled automatically", result.stdout)
            self.assertIn("Tests in tests/unit/nodes/test_*.cpp are discovered automatically.", result.stdout)

            self.assertEqual(list(Path(temp).rglob("CMakeLists.txt")), [])
            self.assertEqual(list((Path(temp) / "tests").glob("*.cmake")), [])
            self.assertIn("cmake --build build --target edgeflow_test_nodes_runner", result.stdout)
            self.assertIn("CustomNodeCatalogTest.awesome_feature_*", result.stdout)
            self.assertIn("(cd build && ctest -R CommonNodesTest)", result.stdout)

    def test_write_test_dry_run_does_not_create_files(self):
        with tempfile.TemporaryDirectory() as temp:
            env = self._setup_mock_repo(temp)

            result = self.run_cli(
                "dry_run", "--write-test", "--dry-run", env=env
            )
            self.assertEqual(result.returncode, 0, result.stderr)

            # 检查预览输出
            source_file = Path(temp) / "src" / "custom_nodes" / "dry_run_node.cpp"
            test_file = Path(temp) / "tests" / "unit" / "nodes" / "test_dry_run_node.cpp"
            self.assertIn(f"--- {source_file} (new file) ---", result.stdout)
            self.assertIn(f"--- {test_file} (new test file) ---", result.stdout)
            self.assertIn("TEST(CustomNodeCatalogTest, dry_run_", result.stdout)
            self.assertIn("Tests in tests/unit/nodes/test_*.cpp are discovered automatically.", result.stdout)
            self.assertNotIn("+  test_dry_run_node.cpp", result.stdout)

            # 验证没有创建或修改任何文件
            self.assertFalse(source_file.exists())
            self.assertFalse(test_file.exists())
            self.assertEqual(list(Path(temp).rglob("CMakeLists.txt")), [])
            self.assertEqual(list((Path(temp) / "tests").glob("*.cmake")), [])

    def test_write_test_rejects_incompatible_flags_and_custom_output_dir(self):
        with tempfile.TemporaryDirectory() as temp:
            env = self._setup_mock_repo(temp)
            custom_dir = Path(temp) / "custom_dir"
            custom_dir.mkdir()

            cases = [
                (["--force"], "--write-test rejects --force to prevent multi-file overwrite"),
                (["--output-dir", str(custom_dir)], "--write-test requires the standard source directory src/custom_nodes"),
            ]

            for extra_flags, expected_error in cases:
                result = self.run_cli(
                    "rejected", "--write-test", *extra_flags, env=env
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected_error, result.stderr)

            # 确保所有目录中的文件都未被改动
            source_file = Path(temp) / "src" / "custom_nodes" / "rejected_node.cpp"
            test_file = Path(temp) / "tests" / "unit" / "nodes" / "test_rejected_node.cpp"
            self.assertFalse(source_file.exists())
            self.assertFalse(test_file.exists())
            self.assertEqual(list(custom_dir.iterdir()), [])

    def test_write_test_atomic_rollback_on_conflict(self):
        with tempfile.TemporaryDirectory() as temp:
            env = self._setup_mock_repo(temp)

            # 情形 1：目标源文件已存在
            conflict_source = Path(temp) / "src" / "custom_nodes" / "conflict_source_node.cpp"
            conflict_source.write_text("existing custom node source", encoding="utf-8")
            res_source_conflict = self.run_cli(
                "conflict_source", "--write-test", env=env
            )
            self.assertNotEqual(res_source_conflict.returncode, 0)
            self.assertIn("Target already exists", res_source_conflict.stderr)
            self.assertEqual(conflict_source.read_text(encoding="utf-8"), "existing custom node source")
            self.assertFalse((Path(temp) / "tests" / "unit" / "nodes" / "test_conflict_source_node.cpp").exists())
            self.assertEqual(list(Path(temp).rglob("CMakeLists.txt")), [])
            self.assertEqual(list((Path(temp) / "tests").glob("*.cmake")), [])

            # 情形 2：目标测试文件已存在
            conflict_test = Path(temp) / "tests" / "unit" / "nodes" / "test_conflict_test_node.cpp"
            conflict_test.write_text("existing custom node test", encoding="utf-8")
            res_test_conflict = self.run_cli(
                "conflict_test", "--write-test", env=env
            )
            self.assertNotEqual(res_test_conflict.returncode, 0)
            self.assertIn("Target already exists", res_test_conflict.stderr)
            self.assertEqual(conflict_test.read_text(encoding="utf-8"), "existing custom node test")
            self.assertFalse((Path(temp) / "src" / "custom_nodes" / "conflict_test_node.cpp").exists())
            self.assertEqual(list(Path(temp).rglob("CMakeLists.txt")), [])
            self.assertEqual(list((Path(temp) / "tests").glob("*.cmake")), [])

    def test_change_plan_rollback_preserves_concurrent_edit(self):
        with tempfile.TemporaryDirectory() as temp:
            env = self._setup_mock_repo(temp)
            # 即使没有源码清单，通用修改仍保持事务性。
            plan = SCAFFOLD.ChangePlan()
            new_file = Path(temp) / "src" / "custom_nodes" / "rollback_probe.cpp"
            new_test = Path(temp) / "tests" / "unit" / "nodes" / "test_rollback_probe.cpp"
            mod_target = Path(temp) / "settings.json"
            mod_orig = "original settings"
            mod_target.write_text(mod_orig, encoding="utf-8")
            plan.add_new_file(new_file, "// rollback probe content")
            plan.add_new_file(new_test, "// rollback test probe content")
            plan.add_modification(mod_target, mod_orig, mod_orig + "\n# Modified\n")
            # 在提交前模拟对 mod_target 的并发修改
            mod_target.write_text(mod_orig + "\n# Concurrent user edit\n", encoding="utf-8")
            with self.assertRaises(RuntimeError):
                plan.commit()
            # 验证 new_file 已被删除，且 mod_target 未被覆盖
            self.assertFalse(new_file.exists())
            self.assertFalse(new_test.exists())
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

    def test_input_cardinality_generates_domain_stub(self):
        result = self.run_cli("selected", "--in-port", "input:TextBatch:1:N:generate_sub_id",
                              "--dry-run", "--write-test")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("domain transformation is not implemented", result.stdout)
        self.assertIn("UnimplementedDomainLogicFailsCleanly", result.stdout)

    def test_embedding_starter_preserves_custom_ports(self):
        result = self.run_cli("encoder", "--kind", "model",
                              "-m", "embedding", "--in-port", "texts:TextBatch",
                              "--out-port", "vectors:EmbeddingBatch", "--dry-run", "--write-test")
        self.assertEqual(result.returncode, 0, result.stderr)
        for expected in ['Required("texts", &Inputs::texts)',
                         'PreservedOutput<EmbeddingBatch>("vectors", "texts")',
                         'Model("encoder", "bind_model", &Models::encoder)',
                         'models.encoder.Embed(*input.texts)',
                         'encoder_ControlledExecutionAndModelFailure']:
            self.assertIn(expected, result.stdout)


if __name__ == "__main__":
    unittest.main()
