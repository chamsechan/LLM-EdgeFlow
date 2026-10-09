#!/usr/bin/env python3
"""仅用于构建的测试夹具：调用公开 CLI，并编译其原样输出。"""
from pathlib import Path
import os
import json
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
output = Path(sys.argv[1])
output.parent.mkdir(parents=True, exist_ok=True)
cases = [("ScaffoldComputeNode", ["--kind", "compute"]),
         ("ScaffoldConversionNode", ["--out-port", "output:Int32Batch"]),
         ("ScaffoldSplitStubNode", ["--out-port", "output:TextBatch:1:N:generate_sub_id"])]
for cap in ("llm", "embedding", "asr", "ocr", "rerank"):
    cases.append((f"ScaffoldModel{cap.capitalize()}Node", ["--kind", "model", "-m", cap]))
cases.append(("ScaffoldTutorialLlmNode", ["--kind", "model", "-m", "llm"]))
cases.append(("ScaffoldControlNode", ["--control-id", "2000000042"]))


def apply_documented_text_functions(code):
    """编译教程中讲解的两个业务函数体原文。"""
    guide = (root / "doc/dev_guide/first_custom_node.md").read_text(encoding="utf-8")
    for function in ("BuildPrompt", "FormatAnswer"):
        snippet = re.search(r"<!-- starter-example:" + function + r" -->\s*```cpp\n(.*?)\n```",
                            guide, re.DOTALL)
        if not snippet:
            raise RuntimeError(f"Missing documented {function} exercise")
        pattern = (r"(static std::string " + function
                   + r"\(const std::string& text\)\s*\{)\s*return text;\s*\}")
        body = "\n".join("    " + line for line in snippet.group(1).splitlines())
        code, count = re.subn(pattern, lambda match: match.group(1) + "\n" + body + "\n  }", code)
        if count != 1:
            raise RuntimeError(f"Starter no longer has exactly one editable {function}")
    return code


with output.open("w", encoding="utf-8") as stream:
    standalone_cases = [
        ("ScaffoldWrittenTextNode", ["--kind", "compute"]),
        ("ScaffoldWrittenAudioNode", ["--kind", "compute", "--in-port", "input:AudioPcmBatch",
                                      "--out-port", "output:AudioPcmBatch"]),
        ("ScaffoldWrittenControlNode", ["--control-id", "2000000043"]),
    ]
    standalone_cases.append(("ScaffoldWrittenLlmNode", ["--kind", "model", "-m", "llm"]))
    standalone_cases.append(("ScaffoldWrittenMapNode", ["--kind", "compute"]))
    standalone_cases.extend(cases)
    with tempfile.TemporaryDirectory(prefix="edgeflow-written-fixtures-") as directory:
        fixture_root = Path(directory)
        (fixture_root / "src/custom_nodes").mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, LLM_EDGEFLOW_REPO_ROOT=str(fixture_root))
        for name, options in standalone_cases:
            subprocess.run([sys.executable, str(root / "tools/scaffold_custom_node.py"),
                            name, *options, "--write-test"],
                           env=env, text=True, capture_output=True, check=True)
        for source in sorted((fixture_root / "src/custom_nodes").glob("*.cpp")):
            code = source.read_text(encoding="utf-8")
            if source.name == "scaffold_tutorial_llm_node.cpp":
                code = apply_documented_text_functions(code)
            # 在合并后的源码中保留各源文件匿名命名空间的隔离性。
            code = code.replace("namespace {", f"namespace {source.stem} {{", 1)
            stream.write(code)
        for test in sorted((fixture_root / "tests/unit/nodes").glob("test_*.cpp")):
            code = test.read_text(encoding="utf-8")
            if test.name == "test_scaffold_tutorial_llm_node.cpp":
                code = code.replace('mock_answer:', 'mock_answer:实体抽取：\\n')
            stream.write(code)

# 让教程中可运行的 Control 部署继续接受原生校验。
# 只把教程 Node 类型替换为隔离生成的等价类型。
guide = (root / "doc/dev_guide/first_control.md").read_text(encoding="utf-8")
json_blocks = re.findall(r"```json\n(.*?)\n```", guide, re.DOTALL)
if len(json_blocks) < 2:
    raise RuntimeError("Control walkthrough must include pipeline and conf JSON")
tutorial_dir = output.parent / "control_tutorial"
tutorial_dir.mkdir(parents=True, exist_ok=True)
pipeline = json.loads(json_blocks[0])
if pipeline["pipeline"][0]["node_type"] != "PrefixControlNode":
    raise RuntimeError("Control walkthrough node no longer matches the generated fixture")
pipeline["pipeline"][0]["node_type"] = "ScaffoldControlNode"
(tutorial_dir / "pipeline.json").write_text(json.dumps(pipeline), encoding="utf-8")
(tutorial_dir / "pipeline.conf").write_text(json_blocks[1], encoding="utf-8")
with output.open("a", encoding="utf-8") as stream:
    stream.write('''
#include <filesystem>

#include "adapter/deployment_io_config.h"
#include "adapter/io_plan_resolver.h"

namespace llm_edgeflow {
TEST(CustomNodeCatalogTest,
     ControlTutorialDeploymentUsesCurrentNativeContracts) {
''')
    stream.write("  const std::filesystem::path directory = " + json.dumps(str(tutorial_dir.resolve())) + ";\n")
    stream.write('''  DeploymentIoConfig parsed;
  std::string error;
  ASSERT_TRUE(DeploymentIoConfig::ReadFromFile(
      (directory / "pipeline.conf").string(), &parsed, &error))
      << error;
  EXPECT_EQ(std::filesystem::path(parsed.resolved_pipe_path),
            std::filesystem::canonical(directory / "pipeline.json"));
  std::unique_ptr<ValidatedIoPlan> plan;
  ASSERT_EQ(IoPlanResolver::ResolveFromConfig(parsed, &plan,
                                              &error),
            0)
      << error;
  ASSERT_NE(plan, nullptr);
  ASSERT_EQ(plan->inputs.size(), 1U);
  ASSERT_EQ(plan->outputs.size(), 1U);
  ASSERT_NE(plan->inputs[0].converter, nullptr);
  ASSERT_NE(plan->outputs[0].converter, nullptr);
  EXPECT_EQ(plan->inputs[0].converter->type, "keyword_in");
  EXPECT_EQ(plan->inputs[0].converter->name, "keyword_match");
  EXPECT_EQ(plan->outputs[0].converter->type, "keyword_out");
  EXPECT_EQ(plan->outputs[0].converter->name, "keyword_match");
}
}  // namespace llm_edgeflow
''')
