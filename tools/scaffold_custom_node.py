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


def find_template(rel_path):
    if "LLM_EDGEFLOW_REPO_ROOT" in os.environ:
        cand = Path(os.environ["LLM_EDGEFLOW_REPO_ROOT"]) / rel_path
        if cand.exists():
            return cand
    return Path(__file__).resolve().parents[1] / rel_path


STARTER_LLM_TEMPLATE = find_template("dev_support/node_authoring/starter_llm_node.cpp")
STARTER_CONTROL_TEMPLATE = find_template("dev_support/node_authoring/starter_control_node.cpp")


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


def get_item_type_for_batch(batch):
    # 不维护 payload 名称的平行表，而是从实际批类型推导。
    return f"decltype({batch}::value_type{{}}.data)"


def render_map_node(name, description, in_port, out_port):
    in_name, in_type, in_card, in_prov = in_port
    out_name, out_type, out_card, out_prov = out_port
    return f"""#include <string>

#include "nodes/authoring.h"

namespace llm_edgeflow {{
namespace custom_nodes {{
namespace {{
// 逐条转换入门模板：每条输入得到一条输出，来源编号由框架保留。
static std::string Transform(const std::string& input) {{
  // TODO: 替换为你的领域逻辑。
  return input;
}}

struct Inputs {{
  const {in_type}* input = nullptr;
}};

NodeResult<{out_type}> Run(const Inputs& inputs) {{
  return MapPayloads(*inputs.input, &Transform);
}}

auto Spec() {{
  return MakeNodeSpec(
      InputsOf<Inputs>{{Required({cpp_string(in_name)}, &Inputs::input)}},
      PreservedOutput<{out_type}>({cpp_string(out_name)}, {cpp_string(in_name)}),
      &Run)
      .Description({cpp_string(description)});
}}

REGISTER_FUNCTION_NODE({name}, Spec());

}}  // namespace
}}  // namespace custom_nodes
}}  // namespace llm_edgeflow
"""


def render_llm_starter(name, description, in_name, out_name):
    source = STARTER_LLM_TEMPLATE.read_text(encoding="utf-8")
    source = source.replace("starter_llm", name)
    literals = {
        '"input"': cpp_string(in_name),
        '"output"': cpp_string(out_name),
        '"LLM authoring starter"': cpp_string(description),
    }
    return re.sub(r'"input"|"output"|"LLM authoring starter"',
                  lambda match: literals[match.group()], source)


def render_embedding_node(name, description, in_name, out_name):
    return f"""#include "nodes/authoring.h"

namespace llm_edgeflow {{
namespace custom_nodes {{
namespace {{
struct Inputs {{
  const TextBatch* texts = nullptr;
}};

struct Models {{
  EmbeddingCall encoder;
}};

static NodeResult<EmbeddingBatch> Run(const Inputs& input, const Models& models) {{
  // TODO: 按需添加领域预处理或后处理。
  return models.encoder.Embed(*input.texts);
}}

auto Spec() {{
  return MakeNodeSpec(
      InputsOf<Inputs>({{Required({cpp_string(in_name)}, &Inputs::texts)}}),
      PreservedOutput<EmbeddingBatch>({cpp_string(out_name)}, {cpp_string(in_name)}),
      ModelsOf<Models>({{Model("encoder", "bind_model", &Models::encoder)}}),
      &Run)
      .Description({cpp_string(description)});
}}

REGISTER_FUNCTION_NODE({name}, Spec());

}}  // namespace
}}  // namespace custom_nodes
}}  // namespace llm_edgeflow
"""


def render_node(name, description, kind, capability, in_port, out_port, control_id=None):
    in_name, in_type, in_card, in_prov = in_port
    out_name, out_type, out_card, out_prov = out_port
    preserved = (in_card, in_prov, out_card, out_prov) == ("1:1", "preserve", "1:1", "preserve")
    if control_id is not None:
        if not 1000 <= control_id <= 2147483647:
            raise ValueError("--control-id must be an unused custom ID in 1000..2147483647")
        if (kind != "compute" or in_type != "TextBatch" or out_type != "TextBatch"
                or (in_card, in_prov, out_card, out_prov) != ("1:1", "preserve", "1:1", "preserve")):
            raise ValueError("Control starter requires --kind compute and TextBatch 1:1 preserve ports; copy its Control fragment for other Node kinds")
        source = STARTER_CONTROL_TEMPLATE.read_text(encoding="utf-8")
        source = source.replace("starter_control", name)
        source = re.sub(r"kUpdatePrefix = \d+;", f"kUpdatePrefix = {control_id};", source)
        literals = {'"input"': cpp_string(in_name), '"output"': cpp_string(out_name),
                    '"Control authoring starter"': cpp_string(description)}
        return re.sub(r'"input"|"output"|"Control authoring starter"',
                      lambda match: literals[match.group()], source)
    if kind == "model":
        signature = CAPABILITY_MAP.get(capability)
        if not signature:
            raise ValueError("A model capability is required")
        if (in_type, out_type) != signature[1:3]:
            raise ValueError(f"{capability} requires {signature[1]} -> {signature[2]}; customize conversions in C++")
        if not preserved:
            raise ValueError("Model templates require 1:1 preserve ports; customize batch changes in C++")
        if capability == "llm":
            return render_llm_starter(name, description, in_name, out_name)
        if capability == "embedding":
            return render_embedding_node(name, description, in_name, out_name)
    elif kind != "compute":
        raise ValueError("kind must be compute or model")
    elif preserved and in_type == out_type == "TextBatch":
        return render_map_node(name, description, in_port, out_port)

    input_flow = ("" if (in_card, in_prov) == ("1:1", "preserve") else
                  f", PortFlow{{{cpp_string(in_card)}, {cpp_string(in_prov)}}}")
    output = (f"PreservedOutput<{out_type}>({cpp_string(out_name)}, {cpp_string(in_name)})"
              if preserved else
              f"ProducedBatch<{out_type}>({cpp_string(out_name)}, PortFlow{{{cpp_string(out_card)}, {cpp_string(out_prov)}}})")
    models = ""
    run_models = ""
    model_binding = ""
    if kind == "model":
        call, _, _, method = CAPABILITY_MAP[capability]
        models = f"struct Models {{ {call} model; }};\n"
        run_models = ", const Models& models"
        model_binding = '\n      ModelsOf<Models>{Model("model", "bind_model", &Models::model)},'
        options = ", TranscribeOptions{}" if capability == "asr" else ""
        processing = f"  return models.model.{method}(*inputs.items{options});"
    elif preserved and in_type == out_type:
        processing = f"  return NodeResult<{out_type}>::Success(*inputs.items);"
    else:
        processing = f'''  (void)inputs;
  // TODO: 实现声明的转换和来源策略。
  // 不要把默认 payload 当作成功的业务结果发布。
  return NodeResult<{out_type}>::Failure(
      NodeErrorKind::kBusinessError, "domain transformation is not implemented", -8102);'''
    return f'''#include "nodes/authoring.h"

namespace llm_edgeflow {{
namespace custom_nodes {{
namespace {{
struct Inputs {{ const {in_type}* items = nullptr; }};
{models}
static NodeResult<{out_type}> Run(const Inputs& inputs{run_models}) {{
{processing}
}}

auto Spec() {{
  return MakeNodeSpec(
      InputsOf<Inputs>{{Required({cpp_string(in_name)}, &Inputs::items{input_flow})}},
      {output},{model_binding}
      &Run)
      .Description({cpp_string(description)});
}}

REGISTER_FUNCTION_NODE({name}, Spec());
}}  // namespace
}}  // namespace custom_nodes
}}  // namespace llm_edgeflow
'''


def render_model_node(name, description, capability, in_port, out_port):
    return render_node(name, description, "model", capability, in_port, out_port)


def render_standalone_test(name, description, kind, capability, in_port, out_port, control_id=None):
    in_name, in_type, in_card, in_prov = in_port
    out_name, out_type, out_card, out_prov = out_port
    preserved = (in_card, in_prov, out_card, out_prov) == ("1:1", "preserve", "1:1", "preserve")
    inputs = {
        "TextBatch": 'std::string("example")',
        "AudioPcmBatch": "AudioPcmPayload({0.25f, -0.5f}, 16000)",
        "ImageFrameBatch": "ImageFrame{1, 1, 3, {1, 2, 3}}",
        "QueryCandidatesBatch": 'QueryCandidatePair{"query", "candidate"}',
        "Int32Batch": "int32_t{-7}",
    }
    input_payload = inputs.get(in_type, f"{get_item_type_for_batch(in_type)}{{}}")
    fixture = ""
    setup = ""
    params = "nlohmann::json::object()"
    checks = {
        "TextBatch": 'EXPECT_EQ(item.data, "example");',
        "AudioPcmBatch": "EXPECT_EQ(item.data.pcm_data, (std::vector<float>{0.25f, -0.5f}));\n  EXPECT_EQ(item.data.sample_rate, 16000);",
        "ImageFrameBatch": "EXPECT_EQ(item.data.width, 1);\n  EXPECT_EQ(item.data.height, 1);\n  EXPECT_EQ(item.data.stride, 3u);\n  EXPECT_EQ(item.data.data, (std::vector<uint8_t>{1, 2, 3}));",
        "QueryCandidatesBatch": 'EXPECT_EQ(item.data.query, "query");\n  EXPECT_EQ(item.data.candidate, "candidate");',
        "Int32Batch": "EXPECT_EQ(item.data, -7);",
        "EmbeddingBatch": "EXPECT_TRUE(item.data.empty());",
        "ScoreBatch": "EXPECT_FLOAT_EQ(item.data, 0.0f);",
        "OcrDocumentBatch": "EXPECT_TRUE(item.data.combined_text.empty());\n  EXPECT_TRUE(item.data.boxes.empty());",
        "RankedTextBatch": "EXPECT_TRUE(item.data.text.empty());\n  EXPECT_FLOAT_EQ(item.data.score, 0.0f);\n  EXPECT_EQ(item.data.rank, 0);\n  EXPECT_EQ(item.data.original_sub_id, 0u);\n  EXPECT_EQ(item.data.metadata, nlohmann::json::object());",
        "RuleMatchBatch": "EXPECT_EQ(item.data.is_hit, 0);\n  EXPECT_EQ(item.data.status_code, 0);\n  EXPECT_TRUE(item.data.category.empty());\n  EXPECT_TRUE(item.data.matches.empty());\n  EXPECT_EQ(item.data.slots, nlohmann::json::object());",
        "StructuredDocumentBatch": "EXPECT_TRUE(item.data.json_payload.empty());\n  EXPECT_EQ(item.data.structured_data, nlohmann::json::object());\n  EXPECT_FALSE(item.data.is_valid);\n  EXPECT_EQ(item.data.parse_status, JsonParseStatus::kOk);\n  EXPECT_TRUE(item.data.diagnostic.empty());",
    }
    payload_checks = checks.get(out_type, "// Add domain payload assertions for this custom batch type.")
    if kind == "model":
        _, model_input, model_output, method = CAPABILITY_MAP[capability]
        interface = {"llm": "ILlmModel", "embedding": "IEmbeddingModel", "asr": "IAsrModel",
                     "ocr": "IOcrModel", "rerank": "IRerankModel"}[capability]
        options = {"llm": ", const GenerateOptions&", "asr": ", const TranscribeOptions&"}.get(capability, "")
        language = ('  bool SupportsLanguage(std::string_view language) const noexcept override {\n'
                    '    return language == "zh";\n  }\n') if capability == "asr" else ""
        returned = {"llm": 'std::string("fixture text")', "embedding": "std::vector<float>{3, 4}",
                    "asr": 'std::string("fixture audio")', "ocr": 'OcrDocumentItem{{}, "fixture ocr"}',
                    "rerank": "0.75f"}[capability]
        payload_checks = {
            "llm": 'EXPECT_EQ(item.data, "fixture text");',
            "embedding": "EXPECT_EQ(item.data, (std::vector<float>{3, 4}));",
            "asr": 'EXPECT_EQ(item.data, "fixture audio");',
            "ocr": 'EXPECT_EQ(item.data.combined_text, "fixture ocr");\n  EXPECT_TRUE(item.data.boxes.empty());',
            "rerank": "EXPECT_FLOAT_EQ(item.data, 0.75f);",
        }[capability]
        fixture = f'''class RecordingModel final : public {interface} {{
 public:
  const std::string& ImplName() const noexcept override {{
    static const std::string value = {cpp_string("scaffold_" + name + "_fixture")};
    return value;
  }}
  const std::string& ModelType() const noexcept override {{
    static const std::string value = {cpp_string(capability)};
    return value;
  }}
  InferenceConcurrency Concurrency() const noexcept override {{
    return InferenceConcurrency::kConcurrent;
  }}
{language}  int {method}(const {model_input}& inputs{options}, {model_output}* outputs,
               std::string*) noexcept override {{
    ++calls;
    if (!outputs || inputs.size() != 1) return -1;
    req_id = inputs.front().req_id;
    sub_id = inputs.front().sub_id;
    try {{
      outputs->clear();
      outputs->emplace_back(req_id, sub_id, {returned});
      return 0;
    }} catch (...) {{
      outputs->clear();
      return -1;
    }}
  }}
  int calls = 0;
  uint32_t req_id = 0;
  uint32_t sub_id = 0;
}};

'''
        setup = '''  auto model = std::make_shared<RecordingModel>();
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "fixture", model));
'''
        params = 'nlohmann::json{{"bind_model", "fixture"}}'
    if control_id is not None:
        params = 'nlohmann::json{{"prefix", "before:"}}'
        payload_checks = 'EXPECT_EQ(item.data, "before:example");'
    succeeds = kind == "model" or (preserved and in_type == out_type)
    if succeeds:
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
        execution = '''  EXPECT_EQ(probe.node->Process(&context), -8102);
  EXPECT_FALSE(context.Has("result"));
'''
    if kind == "model":
        execution += '''  EXPECT_EQ(model->calls, 1);
  EXPECT_EQ(model->req_id, 7u);
  EXPECT_EQ(model->sub_id, 2u);
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
#include "engine/model_interface.h"
#include "tests/support/node_probe.h"

namespace llm_edgeflow::scaffold_{name}_test {{
{fixture}TEST(ScaffoldGeneratedNodeTest, {name}_ExecutesDeclaredContract) {{
  const auto definition = NodeRegistry::Instance().Find({cpp_string(name)});
  ASSERT_TRUE(definition);
  EXPECT_EQ(definition->category, "custom");
  EXPECT_EQ(definition->description, {cpp_string(description)});
  ASSERT_EQ(definition->inputs.size(), 1u);
  EXPECT_EQ(definition->inputs.front().logical_name, {cpp_string(in_name)});
  EXPECT_EQ(definition->inputs.front().cardinality, {cpp_string(in_card)});
  EXPECT_EQ(definition->inputs.front().provenance_policy, {cpp_string(in_prov)});
  EXPECT_EQ(definition->inputs.front().type_id, MakeBlackboardKey<{in_type}>("input").type_id);
  ASSERT_EQ(definition->outputs.size(), 1u);
  EXPECT_EQ(definition->outputs.front().logical_name, {cpp_string(out_name)});
  EXPECT_EQ(definition->outputs.front().cardinality, {cpp_string(out_card)});
  EXPECT_EQ(definition->outputs.front().provenance_policy, {cpp_string(out_prov)});
  EXPECT_EQ(definition->outputs.front().type_id, MakeBlackboardKey<{out_type}>("result").type_id);
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
    parser.add_argument("-k", "--kind", choices=["compute", "model"], default="compute")
    parser.add_argument("-m", "--model-capability", choices=list(CAPABILITY_MAP))
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
    if args.kind == "compute" and args.model_capability:
        parser.error("--model-capability requires --kind model")
    capability = (args.model_capability or "llm") if args.kind != "compute" else None
    signature = CAPABILITY_MAP.get(capability, (None, "TextBatch", "TextBatch"))
    try:
        in_port = parse_port_spec(args.in_port or f"input:{signature[1]}", "input")
        out_port = parse_port_spec(args.out_port or f"output:{signature[2]}", "output")
        content = render_node(name, args.description or f"Custom algorithm node {name}.",
                              args.kind, capability, in_port, out_port, args.control_id)

        target = root / args.output_dir / (name + "_node.cpp")
        test_filename = f"test_{name}_node.cpp"
        test_target = root / "tests/unit/nodes" / test_filename

        plan = ChangePlan()
        if args.force and target.exists():
            plan.add_modification(target, target.read_text(encoding="utf-8"), content)
        else:
            plan.add_new_file(target, content)
        if args.write_test:
            test_content = render_standalone_test(name, args.description or f"Custom algorithm node {name}.",
                                                  args.kind, capability, in_port, out_port, args.control_id)
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

        if args.control_id is not None:
            print("Next: edit ApplyPrefix for business logic and Parameters/Field for config and Control validation. Walkthrough: doc/dev_guide/first_control.md")
        if args.kind == "model" and capability == "llm":
            print("Next: edit BuildPrompt and FormatAnswer. Walkthrough: doc/dev_guide/first_custom_node.md")

    except (ValueError, OSError, RuntimeError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
