#!/usr/bin/env python3
"""Unit tests for scripts/scaffold_custom_node.py."""

import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCAFFOLD_SCRIPT = ROOT / "scripts" / "scaffold_custom_node.py"

SPEC = importlib.util.spec_from_file_location("scaffold_custom_node", SCAFFOLD_SCRIPT)
SCAFFOLD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SCAFFOLD)


class ScaffoldCustomNodeTest(unittest.TestCase):
    def test_to_snake_case(self):
        self.assertEqual(SCAFFOLD.to_snake_case("DomainSummaryNode"), "domain_summary_node")
        self.assertEqual(SCAFFOLD.to_snake_case("PromptGuidedLlmNode"), "prompt_guided_llm_node")
        self.assertEqual(SCAFFOLD.to_snake_case("OcrPreprocessNode"), "ocr_preprocess_node")

    def test_compute_node_rendering_and_layer_isolation(self):
        in_port = ("input", "TextBatch", "1:1", "preserve")
        out_port = ("output", "TextBatch", "1:1", "preserve")
        code = SCAFFOLD.render_compute_node(
            "SampleComputeNode", "A test compute node", in_port, out_port
        )
        self.assertIn("class SampleComputeNode final : public NodeBase", code)
        self.assertIn("BoundInput<TextBatch> in_port_;", code)
        self.assertIn("BoundOutput<TextBatch> out_port_;", code)
        self.assertIn("REGISTER_NODE_WITH_DEFINITION", code)
        self.assertIn('/*category=*/"custom"', code)
        self.assertIn("MakeTraceableItem(item.payload, item.req_id, item.sub_id)", code)

        # Architectural isolation: Layer 3 must not include Layer 1 headers
        self.assertNotIn("company_alg_interface.h", code)
        self.assertNotIn("adapter/", code)
        self.assertNotIn("operator/", code)

    def test_model_node_rendering_all_capabilities(self):
        caps = ["llm", "embedding", "asr", "ocr", "rerank"]
        for cap in caps:
            cap_info = SCAFFOLD.CAPABILITY_MAP[cap]
            in_port = (cap_info["default_in"][0], cap_info["default_in"][1], "1:1", "preserve")
            out_port = (cap_info["default_out"][0], cap_info["default_out"][1], "1:1", "preserve")
            code = SCAFFOLD.render_model_node(
                f"Sample{cap.capitalize()}Node", f"A test {cap} node", cap, in_port, out_port
            )
            self.assertIn(f"public ModelBoundNode<{cap_info['interface']}>", code)
            self.assertIn(f'/*model_capability=*/"{cap}"', code)
            self.assertIn("GetModel()", code)
            self.assertIn("REGISTER_NODE_WITH_DEFINITION", code)
            # Isolation check
            self.assertNotIn("adapter/", code)

    def test_unary_inference_node_rendering(self):
        in_port = ("audio", "AudioPcmBatch", "1:1", "preserve")
        out_port = ("text", "TextBatch", "1:1", "preserve")
        code = SCAFFOLD.render_unary_inference_node(
            "SampleAsrUnaryNode", "A test unary ASR node", "asr", in_port, out_port
        )
        self.assertIn("TraceableUnaryInferenceNode<IAsrModel, AudioPcmData, std::string>", code)
        self.assertIn("int InferBatch(const InputBatch& input, OutputBatch* output) override", code)
        self.assertIn("REGISTER_NODE_WITH_DEFINITION", code)

    def test_add_to_cmakelists(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            cmake_file = Path(temp_dir) / "CMakeLists.txt"
            initial_content = (
                "target_sources(edgeflow_layer3_node_objects PRIVATE\n"
                "  existing_node.cpp\n"
                ")\n"
            )
            cmake_file.write_text(initial_content, encoding="utf-8")

            success = SCAFFOLD.add_to_cmakelists(cmake_file, "new_test_node.cpp")
            self.assertTrue(success)

            updated = cmake_file.read_text(encoding="utf-8")
            self.assertIn("new_test_node.cpp", updated)
            self.assertIn("existing_node.cpp", updated)
            self.assertTrue(updated.strip().endswith(")"))

            # Calling again should be idempotent
            success_again = SCAFFOLD.add_to_cmakelists(cmake_file, "new_test_node.cpp")
            self.assertTrue(success_again)
            self.assertEqual(updated.count("new_test_node.cpp"), 1)

    def test_cli_dry_run_execution(self):
        res = subprocess.run(
            [
                "python3",
                str(SCAFFOLD_SCRIPT),
                "SampleCliTestNode",
                "--kind",
                "compute",
                "--dry-run",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        self.assertIn("class SampleCliTestNode final : public NodeBase", res.stdout)
        self.assertIn("REGISTER_NODE_WITH_DEFINITION", res.stdout)


if __name__ == "__main__":
    unittest.main()
