#!/usr/bin/env python3
"""Generate custom capability nodes using the existing Node interfaces.

This is an authoring aid, not a second catalog or Pipeline validator. Rebuild and
query alg_pipeline_tool to discover registrations and validate actual wiring.
"""

import argparse
import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import sys
import tempfile


# 编译期能力签名；可用性仍以 Catalog 为准。
CAPABILITY_MAP = {
    "llm": ("LlmCall", "TextBatch", "TextBatch", "Generate"),
    "embedding": ("EmbeddingCall", "TextBatch", "EmbeddingBatch", "Embed"),
    "asr": ("AsrCall", "AudioPcmBatch", "TextBatch", "Transcribe"),
    "ocr": ("OcrCall", "ImageFrameBatch", "OcrDocumentBatch", "Recognize"),
    "rerank": ("RerankCall", "QueryCandidatesBatch", "ScoreBatch", "Score"),
}


def cpp_string(value):
    return json.dumps(value, ensure_ascii=False)


def parse_port_spec(spec, default_role="input"):
    """解析 name:Batch[:1:1[:preserve]]，保留 cardinality 的两侧。"""
    parts = [part.strip() for part in spec.split(":")]
    if len(parts) not in (2, 4, 5):
        raise ValueError(f"Invalid {default_role} port: expected name:Batch[:1:1[:preserve]]")
    name, batch = parts[:2]
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        raise ValueError("Port name must be an identifier")
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", batch):
        raise ValueError("Batch type must be an unqualified C++ type identifier")
    cardinality = ":".join(parts[2:4]) if len(parts) >= 4 else "1:1"
    if not re.fullmatch(r"[1N]:[1N]", cardinality):
        raise ValueError("Cardinality must be 1:1, 1:N, N:1 or N:N")
    provenance = parts[4] if len(parts) == 5 else "preserve"
    if not re.fullmatch(r"[a-z_]+", provenance):
        raise ValueError("Provenance policy must be an identifier")
    return name, batch, cardinality, provenance


def payload_type(batch):
    # 文本直接写出类型；其余从实际批类型推导，不维护 payload 名称的平行表。
    return "std::string" if batch == "TextBatch" else f"decltype({batch}::value_type::data)"


def is_preserved(in_port, out_port):
    return (in_port[2], in_port[3], out_port[2], out_port[3]) == ("1:1", "preserve", "1:1", "preserve")


def check_request(in_port, out_port, capability, control_id):
    in_type, out_type = in_port[1], out_port[1]
    preserved = is_preserved(in_port, out_port)
    if control_id is not None:
        if not 1000 <= control_id <= 2147483647:
            raise ValueError("--control-id must be an unused custom ID in 1000..2147483647")
        if capability or not (preserved and in_type == out_type == "TextBatch"):
            raise ValueError("--control-id requires TextBatch 1:1 preserve ports without "
                             "--model-capability; copy its Control fragment for other Nodes")
    if capability:
        _, model_in, model_out, _ = CAPABILITY_MAP[capability]
        if (in_type, out_type) != (model_in, model_out):
            raise ValueError(f"{capability} requires {model_in} -> {model_out}; customize conversions in C++")
        if not preserved:
            raise ValueError("Model calls require 1:1 preserve ports; customize batch changes in C++")


def render_node(name, description, in_port, out_port, capability=None, control_id=None):
    """所有 Node 共用一个结构：Inputs、可选的 Params 与 Models、处理函数、Run、Spec。

    带模型只是多声明 Models 并在 Run 中调用；Control 只是多声明 Params 与命令。
    """
    check_request(in_port, out_port, capability, control_id)
    in_name, in_type, in_card, in_prov = in_port
    out_name, out_type, out_card, out_prov = out_port
    preserved = is_preserved(in_port, out_port)
    declarations = [f"struct Inputs {{\n  const {in_type}* input = nullptr;\n}};\n"]
    spec_extra = []
    controls = ""
    if capability == "llm":
        declarations.append("struct Models {\n  LlmCall model;\n};\n")
        logic = """// 从这里开始：调用模型前整理每条文本，调用后整理每条回答。
std::string BuildPrompt(const std::string& text) { return text; }

std::string FormatAnswer(const std::string& text) { return text; }

// 逐条构造提示词，整批调用一次模型，再逐条整理回答；来源编号由框架保留。
// 生成参数（max_tokens、temperature 等）来自节点配置。
NodeResult<TextBatch> Run(const Inputs& inputs, const GenerateOptions& options,
                          const Models& models) {
  auto answers =
      models.model.Generate(MapPayloads(*inputs.input, &BuildPrompt), options);
  if (!answers.ok()) return answers;
  return MapPayloads(answers.value(), &FormatAnswer);
}
"""
        spec_extra = ["GenerateParameters()",
                      'ModelsOf<Models>{Model("model", "bind_model", &Models::model)}']
    elif capability:
        call, _, _, method = CAPABILITY_MAP[capability]
        declarations.append(f"struct Models {{\n  {call} model;\n}};\n")
        options = ", TranscribeOptions{}" if capability == "asr" else ""
        logic = f"""// 整批调用一次模型，来源编号由框架保留；需要时在调用前后加入领域处理。
NodeResult<{out_type}> Run(const Inputs& inputs, const Models& models) {{
  return models.model.{method}(*inputs.input{options});
}}
"""
        spec_extra = ['ModelsOf<Models>{Model("model", "bind_model", &Models::model)}']
    elif control_id is not None:
        declarations.append("struct Params {\n  std::string prefix;\n};\n")
        declarations.append("// 根据当前 Catalog 选择一个稳定且未被占用的自定义 ID。\n"
                            f"inline constexpr int kUpdatePrefix = {control_id};\n")
        logic = """// 从这里开始：逐条处理。prefix 来自节点配置，Control 可在运行时替换它。
std::string Transform(const std::string& text, const Params& params) {
  return params.prefix + text;
}

NodeResult<TextBatch> Run(const Inputs& inputs, const Params& params) {
  return MapPayloads(*inputs.input, [&params](const std::string& text) {
    return Transform(text, params);
  });
}
"""
        spec_extra = ["""Parameters<Params>(
                 {
                     Field("prefix", &Params::prefix)
                         .Default("")
                         .Description(
                             "Text prepended to each input; at most 64 UTF-8 "
                             "bytes. Control replaces this initial value."),
                 })
                 .Validate([](const Params& params, std::string* diagnostic) {
                   if (params.prefix.size() > 64) {
                     if (diagnostic) {
                       *diagnostic = "prefix exceeds 64 UTF-8 bytes";
                     }
                     return false;
                   }
                   return true;
                 })"""]
        controls = """
      .WithControls({
          ReplaceFields(kUpdatePrefix, "set_prefix", {"prefix"},
                        "Replace the text prefix (at most 64 bytes)"),
      })"""
    elif preserved and in_type == out_type:
        item, value = payload_type(in_type), "text"
        if item != "std::string":
            declarations.insert(0, f"using Item = {item};\n")
            item, value = "Item", "item"
        logic = f"""// 从这里开始：逐条处理。每条输入得到一条输出，来源编号由框架保留。
{item} Transform(const {item}& {value}) {{ return {value}; }}

NodeResult<{out_type}> Run(const Inputs& inputs) {{
  return MapPayloads(*inputs.input, &Transform);
}}
"""
    else:
        logic = f"""// 从这里开始：实现声明的转换和来源策略。
// 不要把默认 payload 当作成功的业务结果发布。
NodeResult<{out_type}> Run(const Inputs& inputs) {{
  (void)inputs;
  return NodeResult<{out_type}>::Failure(
      "domain transformation is not implemented", error::kBusinessError);
}}
"""
    input_flow = ("" if (in_card, in_prov) == ("1:1", "preserve") else
                  f", PortFlow{{{cpp_string(in_card)}, {cpp_string(in_prov)}}}")
    output = (f"PreservedOutput<{out_type}>({cpp_string(out_name)}, {cpp_string(in_name)})"
              if preserved else
              f"ProducedBatch<{out_type}>({cpp_string(out_name)}, "
              f"PortFlow{{{cpp_string(out_card)}, {cpp_string(out_prov)}}})")
    spec_args = [f"InputsOf<Inputs>{{Required({cpp_string(in_name)}, &Inputs::input{input_flow})}}",
                 output, *spec_extra, "&Run"]
    headers = "#include <string>\n\n" if "std::string" in logic else ""
    structs = "\n".join(declarations)
    if spec_extra:
        arguments = "\n             " + ",\n             ".join(spec_args)
    else:
        arguments = f"{spec_args[0]},\n                      {spec_args[1]}, &Run"
    return f"""{headers}#include "nodes/authoring.h"

namespace llm_edgeflow {{
namespace custom_nodes {{
namespace {{
{structs}
{logic}
auto Spec() {{
  return MakeNodeSpec({arguments})
      .Description({cpp_string(description)}){controls};
}}

REGISTER_FUNCTION_NODE({name}, Spec());

}}  // namespace
}}  // namespace custom_nodes
}}  // namespace llm_edgeflow
"""


# 输入样例和对应的断言；未列出的类型使用默认构造的 payload。
SAMPLE_INPUTS = {
    "TextBatch": 'std::string("example")',
    "AudioPcmBatch": "AudioPcmPayload({0.25f, -0.5f}, 16000)",
    "ImageFrameBatch": "ImageFrame{1, 1, 3, {1, 2, 3}}",
    "QueryCandidatesBatch": 'QueryCandidatePair{"query", "candidate"}',
    "Int32Batch": "int32_t{-7}",
}
SAMPLE_CHECKS = {
    "TextBatch": 'EXPECT_EQ(item.data, "example");',
    "AudioPcmBatch": "EXPECT_EQ(item.data.pcm_data, (std::vector<float>{0.25f, -0.5f}));\n  EXPECT_EQ(item.data.sample_rate, 16000);",
    "ImageFrameBatch": "EXPECT_EQ(item.data.width, 1);\n  EXPECT_EQ(item.data.height, 1);\n  EXPECT_EQ(item.data.stride, 3u);\n  EXPECT_EQ(item.data.data, (std::vector<uint8_t>{1, 2, 3}));",
    "QueryCandidatesBatch": 'EXPECT_EQ(item.data.query, "query");\n  EXPECT_EQ(item.data.candidate, "candidate");',
    "Int32Batch": "EXPECT_EQ(item.data, -7);",
    "EmbeddingBatch": "EXPECT_TRUE(item.data.empty());",
    "ScoreBatch": "EXPECT_FLOAT_EQ(item.data, 0.0f);",
    "OcrDocumentBatch": "EXPECT_TRUE(item.data.combined_text.empty());\n  EXPECT_TRUE(item.data.boxes.empty());",
    "RankedTextBatch": "EXPECT_TRUE(item.data.text.empty());\n  EXPECT_FLOAT_EQ(item.data.score, 0.0f);\n  EXPECT_EQ(item.data.rank, 0);\n  EXPECT_EQ(item.data.original_sub_id, 0u);",
    "RuleMatchBatch": "EXPECT_EQ(item.data.is_hit, 0);\n  EXPECT_TRUE(item.data.category.empty());\n  EXPECT_TRUE(item.data.matches.empty());\n  EXPECT_EQ(item.data.slots, nlohmann::json::object());",
    "StructuredDocumentBatch": "EXPECT_TRUE(item.data.json_payload.empty());\n  EXPECT_EQ(item.data.structured_data, nlohmann::json::object());\n  EXPECT_FALSE(item.data.is_valid);\n  EXPECT_EQ(item.data.parse_status, JsonParseStatus::kOk);\n  EXPECT_TRUE(item.data.diagnostic.empty());",
}
# tests/support/recording_models.h 中的替身返回这些固定回答。
MODEL_REPLIES = {
    "llm": ('std::string("fixture text")', 'EXPECT_EQ(item.data, "fixture text");'),
    "embedding": ("std::vector<float>{3, 4}", "EXPECT_EQ(item.data, (std::vector<float>{3, 4}));"),
    "asr": ('std::string("fixture audio")', 'EXPECT_EQ(item.data, "fixture audio");'),
    "ocr": ('OcrDocumentItem{{}, "fixture ocr"}',
            'EXPECT_EQ(item.data.combined_text, "fixture ocr");\n  EXPECT_TRUE(item.data.boxes.empty());'),
    "rerank": ("0.75f", "EXPECT_FLOAT_EQ(item.data, 0.75f);"),
}


def render_standalone_test(name, in_port, out_port, capability=None, control_id=None):
    """生成的测试执行真实注册的 Node；端口声明由 Init 绑定和类型化读写检查。"""
    in_name, in_type, _, _ = in_port
    out_name, out_type, _, _ = out_port
    input_payload = SAMPLE_INPUTS.get(in_type, f"{payload_type(in_type)}{{}}")
    payload_checks = SAMPLE_CHECKS.get(out_type, "// Add domain payload assertions for this custom batch type.")
    setup = ""
    params = "nlohmann::json::object()"
    support = ""
    if capability:
        reply, payload_checks = MODEL_REPLIES[capability]
        support = '#include "tests/support/recording_models.h"\n'
        setup = f'''  auto model = std::make_shared<test_support::Recording{capability.capitalize()}>();
  model->replies = {{{{{reply}}}}};
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "fixture", model));
'''
        params = 'nlohmann::json{{"bind_model", "fixture"}}'
    if control_id is not None:
        params = 'nlohmann::json{{"prefix", "before:"}}'
        payload_checks = 'EXPECT_EQ(item.data, "before:example");'
    if capability or (is_preserved(in_port, out_port) and in_type == out_type):
        execution = f'''  ASSERT_EQ(probe.node->Process(&context), 0) << context.GetErrorMessage();
  const auto* result = context.Read<{out_type}>("result");
  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->size(), 1u);
  const auto& item = result->front();
  EXPECT_EQ(item.req_id, 7u);
  EXPECT_EQ(item.sub_id, 2u);
  {payload_checks}
'''
    else:
        execution = '''  EXPECT_EQ(probe.node->Process(&context), error::kBusinessError);
  EXPECT_FALSE(context.Has("result"));
'''
    if capability:
        execution += '''  ASSERT_EQ(model->calls.size(), 1u);
  EXPECT_EQ(model->calls.front().front().req_id, 7u);
  EXPECT_EQ(model->calls.front().front().sub_id, 2u);
'''
    if control_id is not None:
        execution += f'''  ASSERT_EQ(probe.node->Control({control_id}, R"({{"prefix":"after:"}})").status,
            NodeControlStatus::kHandled);
  const auto verify_after = [&] {{
    AlgContext next;
    EXPECT_TRUE(next.Publish("input", inputs));
    EXPECT_EQ(probe.node->Process(&next), 0);
    const auto* updated = next.Read<{out_type}>("result");
    ASSERT_NE(updated, nullptr);
    ASSERT_EQ(updated->size(), 1u);
    EXPECT_EQ(updated->front().req_id, 7u);
    EXPECT_EQ(updated->front().sub_id, 2u);
    EXPECT_EQ(updated->front().data, "after:example");
  }};
  verify_after();
  const auto invalid = nlohmann::json{{{{"prefix", std::string(65, 'x')}}}}.dump();
  EXPECT_EQ(probe.node->Control({control_id}, invalid).status, NodeControlStatus::kFailed);
  verify_after();
'''
    return f'''#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "tests/support/node_probe.h"
{support}
namespace llm_edgeflow::scaffold_{name}_test {{
TEST(ScaffoldGeneratedNodeTest, {name}_ExecutesDeclaredContract) {{
  SessionContext session;
{setup}  test_support::NodeProbe probe;
  ASSERT_TRUE(probe.Init({cpp_string(name)}, {params}, session,
                         {{{{{cpp_string(in_name)}, "input"}}}},
                         {{{{{cpp_string(out_name)}, "result"}}}})) << probe.diagnostic;
  {in_type} inputs;
  inputs.emplace_back(7, 2, {input_payload});
  AlgContext context;
  ASSERT_TRUE(context.Publish("input", inputs));
{execution}}}
}}  // namespace llm_edgeflow::scaffold_{name}_test
'''


class ChangePlan:
    """发布时不覆盖目标；冲突时保留被挪走的编辑。

    目录锁使生成器串行执行。检查内容前先把现有文件挪开，再以不替换语义链接新版本。
    并发编辑者先创建目标时以其为准；被挪走的内容会在不覆盖该编辑者的前提下恢复，
    或保留在明确报告的路径上。
    """
    def __init__(self):
        self.new_files = {}
        self.modified_files = {}
        self.created_paths = []
        self.modified_paths = []
        self.backups = {}

    def add_new_file(self, path: Path, content: str):
        path = path.absolute()
        if path.exists() or path.is_symlink():
            raise ValueError(f"Target already exists: {path}")
        if path in self.new_files or path in self.modified_files:
            raise ValueError(f"Duplicate transaction target: {path}")
        self.new_files[path] = content

    def add_modification(self, path: Path, original: str, new_content: str):
        path = path.absolute()
        if path.is_symlink():
            raise ValueError(f"Registration file must not be a symbolic link: {path}")
        if path in self.new_files or path in self.modified_files:
            raise ValueError(f"Duplicate transaction target: {path}")
        if original != new_content:
            self.modified_files[path] = (original, new_content)

    @staticmethod
    def _stage(path, content):
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=f".{path.name}.",
                                         suffix=".edgeflow-tmp", delete=False) as stream:
            staged = Path(stream.name)
            try:
                stream.write(content.encode("utf-8"))
                stream.flush()
                os.fsync(stream.fileno())
            except BaseException:
                staged.unlink(missing_ok=True)
                raise
        return staged

    @staticmethod
    def _restore(displaced, path):
        # link() 遇到已存在的目标 (含符号链接) 会原子地拒绝。
        try:
            os.link(displaced, path, follow_symlinks=False)
        except FileExistsError:
            raise RuntimeError(f"Concurrent edit preserved at {path}; displaced version retained at {displaced}")
        displaced.unlink()

    @contextlib.contextmanager
    def _locks(self):
        with contextlib.ExitStack() as stack:
            parents = sorted({p.parent for p in (*self.new_files, *self.modified_files)})
            for parent in parents:
                parent.mkdir(parents=True, exist_ok=True)
                fd = os.open(parent, os.O_RDONLY)
                stack.callback(os.close, fd)
                fcntl.flock(fd, fcntl.LOCK_EX)
            yield

    def commit(self):
        with self._locks():
            try:
                for path, content in self.new_files.items():
                    staged = self._stage(path, content)
                    try:
                        os.chmod(staged, 0o644)
                        os.link(staged, path)  # 绝不替换并发新建的文件。
                        self.created_paths.append((path, content))
                    finally:
                        staged.unlink(missing_ok=True)
                for path, (original, content) in self.modified_files.items():
                    staged = self._stage(path, content)
                    backup = self._stage(path, "")
                    try:
                        # 原子地捕获当前实际文件；检查捕获到的版本，
                        # 而不是发布前可能变化的路径名。
                        os.replace(path, backup)
                        self.backups[path] = backup
                        if backup.is_symlink() or backup.read_text(encoding="utf-8") != original:
                            raise RuntimeError(f"Concurrent edit detected in {path}")
                        os.chmod(staged, backup.stat().st_mode & 0o777)
                        os.link(staged, path)
                        self.modified_paths.append((path, original, content))
                    finally:
                        staged.unlink(missing_ok=True)
                        if path not in self.backups:
                            backup.unlink(missing_ok=True)
                # 通过指向被挪走 inode 的已打开描述符检测编辑。
                for path, backup in self.backups.items():
                    if backup.read_text(encoding="utf-8") != self.modified_files[path][0]:
                        raise RuntimeError(f"Concurrent edit detected in displaced file {backup}")
            except BaseException:
                self.rollback()
                raise
            for backup in self.backups.values():
                backup.unlink(missing_ok=True)
            self.backups.clear()

    def _remove_ours(self, path, expected):
        displaced = self._stage(path, "")
        discard = False
        try:
            try:
                os.replace(path, displaced)
            except FileNotFoundError:
                discard = True
                return
            if displaced.is_symlink() or displaced.read_text(encoding="utf-8") != expected:
                self._restore(displaced, path)
                raise RuntimeError(f"Preserved user-modified file during rollback: {path}")
            discard = True
        finally:
            if discard:
                displaced.unlink(missing_ok=True)

    def rollback(self):
        errors = []
        for path, content in reversed(self.created_paths):
            try:
                self._remove_ours(path, content)
            except Exception as error:
                errors.append(str(error))
        for path, original, content in reversed(self.modified_paths):
            try:
                self._remove_ours(path, content)
                if path not in self.backups:
                    self.backups[path] = self._stage(path, original)
            except Exception as error:
                errors.append(str(error))
        for path, backup in list(self.backups.items()):
            try:
                self._restore(backup, path)
                del self.backups[path]
            except Exception as error:
                errors.append(str(error))
        self.created_paths.clear()
        self.modified_paths.clear()
        if errors:
            sys.stderr.write("Rollback conflicts (files retained):\n" + "\n".join(errors) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("node_name", nargs="?")
    parser.add_argument("-m", "--model-capability", choices=list(CAPABILITY_MAP),
                        help="Declare one model of this capability; omit it for a Node without models")
    parser.add_argument("-d", "--description", default="")
    parser.add_argument("-i", "--in-port", help="name:Batch[:1:1[:preserve]]")
    parser.add_argument("-o", "--out-port", help="name:Batch[:1:1[:preserve]]")
    parser.add_argument("--output-dir", default="src/custom_nodes")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("-f", "--force", action="store_true")
    parser.add_argument("--write-test", action="store_true", help="Write an automatically discovered test in tests/unit/nodes/test_<node_name>_node.cpp")
    parser.add_argument("--control-id", type=int, help="Generate the text-prefix Control starter using an unused custom command ID (>=1000)")
    parser.add_argument("--self-test", action="store_true", help="Run the generator's Python tests")
    args = parser.parse_args()
    root = Path(os.environ.get("LLM_EDGEFLOW_REPO_ROOT", Path(__file__).resolve().parent.parent))
    if args.self_test:
        import subprocess
        return subprocess.call([sys.executable, str(root / "tests/tooling/test_scaffold_custom_node.py")])
    if not args.node_name or not re.fullmatch(r"[a-z][a-z0-9]*(?:_[a-z0-9]+)*", args.node_name):
        parser.error("node_name must be a snake_case identifier")

    if args.write_test and args.force:
        parser.error("--write-test rejects --force to prevent multi-file overwrite; remove existing files explicitly")
    if args.write_test and args.output_dir != "src/custom_nodes":
        parser.error("--write-test requires the standard source directory src/custom_nodes")

    name = args.node_name
    capability = args.model_capability
    signature = CAPABILITY_MAP.get(capability, (None, "TextBatch", "TextBatch"))
    try:
        in_port = parse_port_spec(args.in_port or f"input:{signature[1]}", "input")
        out_port = parse_port_spec(args.out_port or f"output:{signature[2]}", "output")
        content = render_node(name, args.description or f"Custom algorithm node {name}.",
                              in_port, out_port, capability, args.control_id)

        target = root / args.output_dir / (name + "_node.cpp")
        test_filename = f"test_{name}_node.cpp"
        test_target = root / "tests/unit/nodes" / test_filename

        plan = ChangePlan()
        if args.force and target.exists():
            plan.add_modification(target, target.read_text(encoding="utf-8"), content)
        else:
            plan.add_new_file(target, content)
        if args.write_test:
            test_content = render_standalone_test(name, in_port, out_port, capability, args.control_id)
            plan.add_new_file(test_target, test_content)

        if args.dry_run:
            if args.write_test:
                print(f"--- {target} (new file) ---")
            print(content)
            if args.write_test:
                print(f"--- {test_target} (new test file) ---")
                print(test_content)
                print("Tests in tests/unit/nodes/test_*.cpp are discovered automatically.")
            return 0

        plan.commit()

        print(f"Created {target}")
        collected_dirs = ("src/custom_nodes", "src/common_nodes")
        if any(target.resolve().is_relative_to((root / d).resolve()) for d in collected_dirs):
            print("Sources under src/custom_nodes/ and src/common_nodes/ are compiled automatically on the next build.")
        else:
            print(f"Note: {target} is outside src/custom_nodes/ and src/common_nodes/, so the build does not collect it.")
        if args.write_test:
            print(f"Created {test_target}")
            print("Tests in tests/unit/nodes/test_*.cpp are discovered automatically.")
            print("Next steps:")
            print("  Build command: cmake --build build --target edgeflow_test_nodes_runner")
            print(f"  Test filter: ScaffoldGeneratedNodeTest.{name}_*")
            print("  Run command: (cd build && ctest -R ScaffoldGeneratedNodeTest)")
            print(f"  (or: ./build/edgeflow_test_nodes_runner --gtest_filter=\"ScaffoldGeneratedNodeTest.{name}_*\")")

        walkthrough = "first_control.md" if args.control_id is not None else "first_custom_node.md"
        print(f"Next: implement the algorithm in Run and any helpers it calls. Walkthrough: doc/dev_guide/{walkthrough}")

    except (ValueError, OSError, RuntimeError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
