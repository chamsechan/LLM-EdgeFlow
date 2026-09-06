#!/usr/bin/env python3
"""Generator CLI/file contracts. Generated C++ also compiles in the Node runner."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/scaffold_custom_node.py"
SPEC = importlib.util.spec_from_file_location("scaffold", SCRIPT)
SCAFFOLD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SCAFFOLD)


class ScaffoldCustomNodeTest(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True)

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
            cmake.write_text("target_sources(edgeflow_layer3_node_objects PRIVATE\n  existing.cpp\n)\n")
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
            self.assertIn("existing.cpp", cmake.read_text())

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


if __name__ == "__main__":
    unittest.main()
