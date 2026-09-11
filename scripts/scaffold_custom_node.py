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


# Compile-time capability signatures; availability still comes from the Catalog.
CAPABILITY_MAP = {
    "llm": ("ILlmModel", "TextBatch", "TextBatch", "Generate(input, GenerateOptions{}, output)"),
    "embedding": ("IEmbeddingModel", "TextBatch", "EmbeddingBatch", "Embed(input, EmbeddingOptions{}, output)"),
    "asr": ("IAsrModel", "AudioPcmBatch", "TextBatch", "Transcribe(input, output)"),
    "ocr": ("IOcrModel", "ImageRefBatch", "OcrDocumentBatch", "Recognize(input, output)"),
    "rerank": ("IRerankModel", "QueryCandidatesBatch", "ScoreBatch", "Score(input, output)"),
}


def to_snake_case(name):
    s = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", s).lower()


def cpp_string(value):
    return json.dumps(value, ensure_ascii=False)


def parse_port_spec(spec, default_role="input"):
    """Parse name:Batch[:1:1[:preserve]], keeping both halves of cardinality."""
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
    # No parallel table of payload names: derive them from the actual batch.
    return f"decltype({batch}::value_type{{}}.data)"


def render_llm_starter(name, description, in_name, out_name):
    """Use the readable, compiled starter as the single LLM model template."""
    source = STARTER_LLM_TEMPLATE.read_text(encoding="utf-8")
    source = source.replace("StarterLlmNode", name)
    literals = {
        '"input"': cpp_string(in_name),
        '"output"': cpp_string(out_name),
        '"LLM authoring starter"': cpp_string(description),
    }
    # One pass: a replacement may itself contain another placeholder's text.
    return re.sub(r'"input"|"output"|"LLM authoring starter"',
                  lambda match: literals[match.group()], source)


def render_node(name, description, kind, capability, in_port, out_port, control_id=None):
    in_name, in_type, in_card, in_prov = in_port
    out_name, out_type, out_card, out_prov = out_port
    signature = CAPABILITY_MAP.get(capability)
    if control_id is not None:
        if not 1000 <= control_id <= 2147483647:
            raise ValueError("--control-id must be an unused custom ID in 1000..2147483647")
        if (kind != "compute" or in_type != "TextBatch" or out_type != "TextBatch"
                or (in_card, in_prov, out_card, out_prov) != ("1:1", "preserve", "1:1", "preserve")):
            raise ValueError("Control starter requires --kind compute and TextBatch 1:1 preserve ports; copy its Control fragment for other Node kinds")
        source = STARTER_CONTROL_TEMPLATE.read_text(encoding="utf-8")
        source = source.replace("StarterControlNode", name)
        source = re.sub(r"kUpdatePrefix = \d+;", f"kUpdatePrefix = {control_id};", source)
        literals = {'"input"': cpp_string(in_name), '"output"': cpp_string(out_name),
                    '"Control authoring starter"': cpp_string(description)}
        return re.sub(r'"input"|"output"|"Control authoring starter"',
                      lambda match: literals[match.group()], source)
    if kind != "compute":
        if not signature:
            raise ValueError("A model capability is required")
        interface, expected_in, expected_out, call = signature
        if (in_type, out_type) != (expected_in, expected_out):
            raise ValueError(f"{capability} requires {expected_in} -> {expected_out}; customize conversions in C++")
        if (in_card, in_prov, out_card, out_prov) != ("1:1", "preserve", "1:1", "preserve"):
            raise ValueError("Model templates require 1:1 preserve ports; customize batch changes in C++")
        if kind == "unary_inference" and capability == "ocr":
            raise ValueError("ImageRefBatch is a distinct container; use --kind model for OCR")
    if kind == "model" and capability == "llm":
        return render_llm_starter(name, description, in_name, out_name)
    base = "NodeBase" if kind == "compute" else f"ModelBoundNode<{signature[0]}>"
    if kind == "unary_inference":
        base = (f"TraceableUnaryInferenceNode<{signature[0]}, "
                f"{get_item_type_for_batch(in_type)}, {get_item_type_for_batch(out_type)}>")
        implementation = f"""  {name}() : Base(kNodeType, {cpp_string(in_name)}, {cpp_string(out_name)}, -8101, -8103, -8103) {{}}

 protected:
  int InferBatch(const InputBatch& input, OutputBatch* output) override {{
    // TODO: Customize request-local preprocessing / postprocessing as needed.
    return model()->{signature[3]};
  }}
"""
    else:
        init = "InitNode" if kind == "compute" else "InitModelNode"
        if kind == "compute":
            if in_type == out_type and (in_card, in_prov, out_card, out_prov) == ("1:1", "preserve", "1:1", "preserve"):
                processing = """    for (const auto& item : input) {
      // TODO: Replace identity with your domain logic. Preserve provenance.
      outputs.emplace_back(item.req_id, item.sub_id, item.data);
    }
"""
            else:
                processing = '''    (void)input;
    // TODO: Implement the declared transformation and provenance policy.
    // Do not publish default payloads as successful business results.
    return Fail(ctx, -8102, Name() + ": domain transformation is not implemented");
'''
        else:
            processing = f"""    auto* output = &outputs;
    // TODO: Build model inputs locally, then postprocess verified outputs.
    const int ret = model()->{signature[3]};
    if (ret != 0) return Fail(ctx, ret, Name() + ": model inference failed");
    if (!ValidatePreservedTraceableAlignment(input, outputs).IsAligned()) {{
      return Fail(ctx, -8103, Name() + ": output count or provenance mismatch");
    }}
"""
        implementation = f"""  {name}() : Base(kNodeType), in_port_({cpp_string(in_name)}), out_port_({cpp_string(out_name)}) {{}}

 protected:
  bool {init}(const NodeInitContext& init_ctx, const nlohmann::json&,
              SessionContext&) override {{
    BindPort(init_ctx, in_port_);
    BindPort(init_ctx, out_port_);
    return true;
  }}

  int ProcessNode(AlgContext& ctx) override {{
    const auto* inputs = in_port_.Require(ctx, -8101);
    if (!inputs) return -8101;
    {out_type} outputs;
    if (inputs->empty()) {{
      out_port_.Set(ctx, std::move(outputs));
      return 0;
    }}
    const auto& input = *inputs;
{processing}
    out_port_.Set(ctx, std::move(outputs));
    return 0;
  }}

 private:
  BoundInput<{in_type}> in_port_;
  BoundOutput<{out_type}> out_port_;
"""
    model_definition = ""
    if kind != "compute":
        model_definition = f"""  def.config_fields = {{ConfigFieldDefinition{{"bind_model", ConfigValueKind::kString, true}}}};
  def.model_capability = {cpp_string(capability)};
  def.model_config_field = "bind_model";
"""
    return f"""#include <string>
#include <string_view>
#include <utility>

#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/node_definition.h"
#include "engine/model_interface.h"
#include "nodes/model_bound_node.h"
#include "nodes/node_base.h"
#include "nodes/traceable_batch_validation.h"
#include "nodes/traceable_unary_inference_node.h"

namespace llm_edgeflow {{
namespace custom_nodes {{
namespace {{

static_assert(std::string_view(BlackboardTypeTraits<{in_type}>::TypeName()) != "Unknown",
              "Input batch needs a BlackboardTypeTraits specialization");
static_assert(std::string_view(BlackboardTypeTraits<{out_type}>::TypeName()) != "Unknown",
              "Output batch needs a BlackboardTypeTraits specialization");

class {name} final : public {base} {{
  using Base = {base};

 public:
  inline static constexpr char kNodeType[] = {cpp_string(name)};
{implementation}}};

NodeDefinition Make{name}Definition() {{
  NodeDefinition def;
  def.node_type = {name}::kNodeType;
  def.category = "custom";
  def.description = {cpp_string(description)};
  def.inputs = {{RequiredInputPort({cpp_string(in_name)},
      BlackboardKey<{in_type}>{{"", BlackboardTypeTraits<{in_type}>::TypeName()}},
      {cpp_string(in_card)}, {cpp_string(in_prov)}, "request")}};
  def.outputs = {{OutputPort({cpp_string(out_name)},
      BlackboardKey<{out_type}>{{"", BlackboardTypeTraits<{out_type}>::TypeName()}},
      {cpp_string(out_card)}, {cpp_string(out_prov)}, "request")}};
{model_definition}  // Enable only after reviewing all node-owned shared state for concurrency.
  def.parallel_safe = false;
  return def;
}}

REGISTER_NODE_WITH_DEFINITION({name}, Make{name}Definition());

}}  // namespace
}}  // namespace custom_nodes
}}  // namespace llm_edgeflow
"""


def render_compute_node(name, description, in_port, out_port):
    return render_node(name, description, "compute", None, in_port, out_port)


def render_model_node(name, description, capability, in_port, out_port):
    return render_node(name, description, "model", capability, in_port, out_port)


def render_unary_inference_node(name, description, capability, in_port, out_port):
    return render_node(name, description, "unary_inference", capability, in_port, out_port)


def render_test_stub(name):
    return f"""#include <gtest/gtest.h>
#include "core/node_registry.h"
#include "core/node_definition.h"

namespace llm_edgeflow {{
TEST(CustomNodeCatalogTest, {name}RegistrationAndInstantiation) {{
  const auto def = PipelineCatalog::FindNode({cpp_string(name)});
  ASSERT_TRUE(def.has_value());
  EXPECT_EQ(def->category, "custom");
  EXPECT_FALSE(def->inputs.empty());
  EXPECT_FALSE(def->outputs.empty());
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->Name(), {cpp_string(name)});
  // Add domain assertions, missing input, model failure and provenance coverage.
}}
}}  // namespace llm_edgeflow
"""


def render_control_test_stub(name, command_id, in_name, out_name):
    return f'''#include <gtest/gtest.h>
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {{
TEST(CustomNodeCatalogTest, {name}ControlChangesOutputAndPreservesOnFailure) {{
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  ASSERT_TRUE(InitNodeForTest(*node, {{{{"prefix", "initial:"}}}}, &session));
  const auto check_output = [&](const std::string& expected) {{
    AlgContext ctx;
    TextBatch input;
    input.emplace_back(17, 3, "sample");
    ctx.Publish({cpp_string(in_name)}, std::move(input));
    ASSERT_EQ(node->Process(&ctx), 0);
    const auto* output = ctx.Read<TextBatch>({cpp_string(out_name)});
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 1u);
    EXPECT_EQ(output->at(0).req_id, 17u);
    EXPECT_EQ(output->at(0).sub_id, 3u);
    EXPECT_EQ(output->at(0).data, expected);
  }};
  check_output("initial:sample");
  const auto update = [&](const nlohmann::json& payload) {{
    return node->Control({command_id}, payload.dump()).status;
  }};
  ASSERT_EQ(update({{{{"prefix", "new:"}}}}), NodeControlStatus::kHandled);
  check_output("new:sample");
  EXPECT_EQ(update({{{{"prefix", 12}}}}), NodeControlStatus::kFailed);
  EXPECT_EQ(update({{{{"prefix", std::string(65, 'x')}}}}), NodeControlStatus::kFailed);
  check_output("new:sample");
  // Extend these assertions with the actual business input and expected result.
}}

TEST(CustomNodeCatalogTest, {name}RejectsInvalidInitialPrefix) {{
  const auto definition = PipelineCatalog::FindNode({cpp_string(name)});
  ASSERT_TRUE(definition.has_value());
  ASSERT_TRUE(static_cast<bool>(definition->validate_config));
  std::string error;
  EXPECT_TRUE(definition->validate_config(nlohmann::json::object(), {{}}, &error));
  const nlohmann::json invalid = {{{{"prefix", std::string(65, 'x')}}}};
  EXPECT_FALSE(definition->validate_config(invalid, {{}}, &error));
  EXPECT_NE(error.find("prefix exceeds 64 UTF-8 bytes"), std::string::npos);
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  NodeInitContext init;
  init.config = &invalid;
  init.session_ctx = &session;
  init.diagnostic = &error;
  EXPECT_FALSE(node->Init(init));
  EXPECT_NE(error.find("prefix exceeds 64 UTF-8 bytes"), std::string::npos);
}}
}}  // namespace llm_edgeflow
'''


def render_standalone_test(name, description, kind, capability, in_port, out_port, control_id=None):
    in_name, in_type, in_card, in_prov = in_port
    out_name, out_type, out_card, out_prov = out_port

    def sample_value_for_type(t, index=1):
        if t == "TextBatch":
            return f'"sample_text_{index}"'
        if t == "ImageRefBatch":
            return f'"sample_image_{index}.jpg"'
        if t == "AudioPcmBatch":
            return f'AudioPcmPayload{{std::vector<float>{{0.1f, 0.2f}}, 16000}}'
        if t == "QueryCandidatesBatch":
            return f'QueryCandidatePair{{"query_{index}", "candidate_{index}"}}'
        if t == "Int32Batch":
            return f'{index * 10}'
        return "{}"

    sample_in_1 = sample_value_for_type(in_type, 1)
    sample_in_2 = sample_value_for_type(in_type, 2)
    sample_in_3 = sample_value_for_type(in_type, 3)

    def payload_check(actual, batch_type, expected, label):
        # A named value keeps aggregate commas out of GoogleTest macro arguments.
        fields = {
            "AudioPcmBatch": ("pcm_data", "sample_rate"),
            "QueryCandidatesBatch": ("query", "candidate"),
        }.get(batch_type, ())
        lines = [f"  const {get_item_type_for_batch(batch_type)} {label} = {expected};"]
        if fields:
            lines.extend(f"  EXPECT_EQ({actual}.{field}, {label}.{field});" for field in fields)
        else:
            lines.append(f"  EXPECT_EQ({actual}, {label});")
        return "\n".join(lines)

    compute_checks = [payload_check(f"output->at({i}).data", in_type, sample, f"expected_{i}")
                      for i, sample in enumerate((sample_in_1, sample_in_2, sample_in_3))]
    business_check = payload_check("output->at(0).data", in_type, sample_in_1, "expected")

    if control_id is not None:
        return f"""#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_definition.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {{

TEST(CustomNodeCatalogTest, {name}_RegistrationAndInstantiation) {{
  const auto def = PipelineCatalog::FindNode({cpp_string(name)});
  ASSERT_TRUE(def.has_value());
  EXPECT_EQ(def->category, "custom");
  EXPECT_FALSE(def->inputs.empty());
  EXPECT_FALSE(def->outputs.empty());
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->Name(), {cpp_string(name)});
}}

TEST(CustomNodeCatalogTest, {name}_ControlChangesOutputAndPreservesOnFailure) {{
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  ASSERT_TRUE(InitNodeForTest(*node, {{{{"prefix", "initial:"}}}}, &session));
  const auto check_output = [&](const std::string& expected) {{
    AlgContext ctx;
    TextBatch input;
    input.emplace_back(17, 3, "sample");
    ctx.Publish({cpp_string(in_name)}, std::move(input));
    ASSERT_EQ(node->Process(&ctx), 0);
    const auto* output = ctx.Read<TextBatch>({cpp_string(out_name)});
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 1u);
    EXPECT_EQ(output->at(0).req_id, 17u);
    EXPECT_EQ(output->at(0).sub_id, 3u);
    EXPECT_EQ(output->at(0).data, expected);
  }};
  check_output("initial:sample");
  const auto update = [&](const nlohmann::json& payload) {{
    return node->Control({control_id}, payload.dump()).status;
  }};
  ASSERT_EQ(update({{{{"prefix", "new:"}}}}), NodeControlStatus::kHandled);
  check_output("new:sample");
  EXPECT_EQ(update({{{{"prefix", 12}}}}), NodeControlStatus::kFailed);
  EXPECT_EQ(update({{{{"prefix", std::string(65, 'x')}}}}), NodeControlStatus::kFailed);
  check_output("new:sample");
}}

TEST(CustomNodeCatalogTest, {name}_RejectsInvalidInitialPrefix) {{
  const auto definition = PipelineCatalog::FindNode({cpp_string(name)});
  ASSERT_TRUE(definition.has_value());
  ASSERT_TRUE(static_cast<bool>(definition->validate_config));
  std::string error;
  EXPECT_TRUE(definition->validate_config(nlohmann::json::object(), {{}}, &error));
  const nlohmann::json invalid = {{{{"prefix", std::string(65, 'x')}}}};
  EXPECT_FALSE(definition->validate_config(invalid, {{}}, &error));
  EXPECT_NE(error.find("prefix exceeds 64 UTF-8 bytes"), std::string::npos);
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  NodeInitContext init;
  init.config = &invalid;
  init.session_ctx = &session;
  init.diagnostic = &error;
  EXPECT_FALSE(node->Init(init));
  EXPECT_NE(error.find("prefix exceeds 64 UTF-8 bytes"), std::string::npos);
}}

TEST(CustomNodeCatalogTest, {name}_MissingInputFailsWithoutPublishing) {{
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  ASSERT_TRUE(InitNodeForTest(*node, nlohmann::json::object(), &session));
  AlgContext ctx;
  EXPECT_NE(node->Process(&ctx), 0);
  EXPECT_FALSE(ctx.Has({cpp_string(out_name)}));
}}

TEST(CustomNodeCatalogTest, {name}_BusinessExample) {{
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  ASSERT_TRUE(InitNodeForTest(*node, {{{{"prefix", "biz:"}}}}, &session));
  AlgContext ctx;
  // Editable business input with independent expected output.
  TextBatch input;
  input.emplace_back(101, 1, "task_sample");
  ctx.Publish({cpp_string(in_name)}, std::move(input));
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* output = ctx.Read<TextBatch>({cpp_string(out_name)});
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 1u);
  EXPECT_EQ(output->at(0).data, "biz:task_sample");
}}

}}  // namespace llm_edgeflow
"""

    if kind == "compute":
        is_pass_through = (in_type == out_type and
                           (in_card, in_prov, out_card, out_prov) == ("1:1", "preserve", "1:1", "preserve"))
        if is_pass_through:
            return f"""#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_definition.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "core/validated_node_plan.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {{

TEST(CustomNodeCatalogTest, {name}_RegistrationAndInstantiation) {{
  const auto def = PipelineCatalog::FindNode({cpp_string(name)});
  ASSERT_TRUE(def.has_value());
  EXPECT_EQ(def->category, "custom");
  EXPECT_FALSE(def->inputs.empty());
  EXPECT_FALSE(def->outputs.empty());
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->Name(), {cpp_string(name)});
}}

TEST(CustomNodeCatalogTest, {name}_InitAndMissingInputFailsWithoutPublishing) {{
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  ValidatedNodePlan plan;
  plan.ports.push_back(ResolvedPortBinding{{
      {cpp_string(in_name)}, "actual_in_key",
      BlackboardTypeTraits<{in_type}>::TypeName(), {cpp_string(in_card)}, {cpp_string(in_prov)},
      "request", PortDirection::kInput}});
  plan.ports.push_back(ResolvedPortBinding{{
      {cpp_string(out_name)}, "actual_out_key",
      BlackboardTypeTraits<{out_type}>::TypeName(), {cpp_string(out_card)}, {cpp_string(out_prov)},
      "request", PortDirection::kOutput}});
  ASSERT_TRUE(InitNodeWithPlan(*node, nlohmann::json::object(), &session, &plan));
  AlgContext ctx;
  EXPECT_NE(node->Process(&ctx), 0);
  EXPECT_FALSE(ctx.Has("actual_out_key"));
}}

TEST(CustomNodeCatalogTest, {name}_EmptyBatchPassesThrough) {{
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  ValidatedNodePlan plan;
  plan.ports.push_back(ResolvedPortBinding{{
      {cpp_string(in_name)}, "actual_in_key",
      BlackboardTypeTraits<{in_type}>::TypeName(), {cpp_string(in_card)}, {cpp_string(in_prov)},
      "request", PortDirection::kInput}});
  plan.ports.push_back(ResolvedPortBinding{{
      {cpp_string(out_name)}, "actual_out_key",
      BlackboardTypeTraits<{out_type}>::TypeName(), {cpp_string(out_card)}, {cpp_string(out_prov)},
      "request", PortDirection::kOutput}});
  ASSERT_TRUE(InitNodeWithPlan(*node, nlohmann::json::object(), &session, &plan));
  AlgContext ctx;
  ctx.Publish("actual_in_key", {in_type}{{}});
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* output = ctx.Read<{out_type}>("actual_out_key");
  ASSERT_NE(output, nullptr);
  EXPECT_TRUE(output->empty());
}}

TEST(CustomNodeCatalogTest, {name}_PreservesBatchDataAndProvenance) {{
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  ValidatedNodePlan plan;
  plan.ports.push_back(ResolvedPortBinding{{
      {cpp_string(in_name)}, "actual_in_key",
      BlackboardTypeTraits<{in_type}>::TypeName(), {cpp_string(in_card)}, {cpp_string(in_prov)},
      "request", PortDirection::kInput}});
  plan.ports.push_back(ResolvedPortBinding{{
      {cpp_string(out_name)}, "actual_out_key",
      BlackboardTypeTraits<{out_type}>::TypeName(), {cpp_string(out_card)}, {cpp_string(out_prov)},
      "request", PortDirection::kOutput}});
  ASSERT_TRUE(InitNodeWithPlan(*node, nlohmann::json::object(), &session, &plan));
  AlgContext ctx;
  {in_type} input;
  input.emplace_back(101, 3, {sample_in_1});
  input.emplace_back(101, 19, {sample_in_2});
  input.emplace_back(205, 1, {sample_in_3});
  ctx.Publish("actual_in_key", input);
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* original_input = ctx.Read<{in_type}>("actual_in_key");
  ASSERT_NE(original_input, nullptr);
  EXPECT_EQ(original_input->size(), 3u);
  const auto* output = ctx.Read<{out_type}>("actual_out_key");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 3u);
  EXPECT_EQ(output->at(0).req_id, 101u);
  EXPECT_EQ(output->at(0).sub_id, 3u);
{compute_checks[0]}
  EXPECT_EQ(output->at(1).req_id, 101u);
  EXPECT_EQ(output->at(1).sub_id, 19u);
{compute_checks[1]}
  EXPECT_EQ(output->at(2).req_id, 205u);
  EXPECT_EQ(output->at(2).sub_id, 1u);
{compute_checks[2]}
}}

TEST(CustomNodeCatalogTest, {name}_BusinessExample) {{
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  ASSERT_TRUE(InitNodeForTest(*node, nlohmann::json::object(), &session));
  AlgContext ctx;
  {in_type} input;
  input.emplace_back(1, 0, {sample_in_1});
  ctx.Publish({cpp_string(in_name)}, input);
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* output = ctx.Read<{out_type}>({cpp_string(out_name)});
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 1u);
  EXPECT_EQ(output->at(0).req_id, 1u);
  EXPECT_EQ(output->at(0).sub_id, 0u);
{business_check}
}}

}}  // namespace llm_edgeflow
"""
        else:
            return f"""#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_definition.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {{

TEST(CustomNodeCatalogTest, {name}_RegistrationAndInstantiation) {{
  const auto def = PipelineCatalog::FindNode({cpp_string(name)});
  ASSERT_TRUE(def.has_value());
  EXPECT_EQ(def->category, "custom");
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->Name(), {cpp_string(name)});
}}

TEST(CustomNodeCatalogTest, {name}_MissingInputFailsWithoutPublishing) {{
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  ASSERT_TRUE(InitNodeForTest(*node, nlohmann::json::object(), &session));
  AlgContext ctx;
  EXPECT_NE(node->Process(&ctx), 0);
  EXPECT_FALSE(ctx.Has({cpp_string(out_name)}));
}}

TEST(CustomNodeCatalogTest, {name}_UnimplementedDomainLogicFailsCleanly) {{
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  ASSERT_TRUE(InitNodeForTest(*node, nlohmann::json::object(), &session));
  AlgContext ctx;
  {in_type} input;
  input.emplace_back(1, 0, {sample_in_1});
  ctx.Publish({cpp_string(in_name)}, std::move(input));
  EXPECT_EQ(node->Process(&ctx), -8102);
  EXPECT_FALSE(ctx.Has({cpp_string(out_name)}));
}}

}}  // namespace llm_edgeflow
"""

    # Model / Unary Inference
    mock_class = {
        "llm": "ControlledMockLlmModel",
        "embedding": "ControlledMockEmbeddingModel",
        "rerank": "ControlledMockRerankModel",
        "ocr": "ControlledMockOcrModel",
        "asr": "ControlledMockAsrModel",
    }.get(capability, "ControlledMockLlmModel")

    if capability == "llm":
        expected_output_check = f"""  EXPECT_EQ(output->at(0).req_id, 1u);
  EXPECT_EQ(output->at(0).sub_id, 0u);
  EXPECT_EQ(output->at(0).data, "mock_answer:" + std::string({sample_in_1}));"""
    elif capability == "embedding":
        expected_output_check = """  EXPECT_EQ(output->at(0).req_id, 1u);
  EXPECT_EQ(output->at(0).sub_id, 0u);
  const std::vector<float> expected = {0.1f, 0.2f, 0.3f};
  EXPECT_EQ(output->at(0).data, expected);"""
    elif capability == "rerank":
        expected_output_check = """  EXPECT_EQ(output->at(0).req_id, 1u);
  EXPECT_EQ(output->at(0).sub_id, 0u);
  EXPECT_FLOAT_EQ(output->at(0).data, 0.95f);"""
    elif capability == "ocr":
        expected_output_check = f"""  EXPECT_EQ(output->at(0).req_id, 1u);
  EXPECT_EQ(output->at(0).sub_id, 0u);
  EXPECT_EQ(output->at(0).data.combined_text, "ocr:" + std::string({sample_in_1}));
  ASSERT_EQ(output->at(0).data.boxes.size(), 1u);
  EXPECT_EQ(output->at(0).data.boxes[0].text, "ocr:" + std::string({sample_in_1}));"""
    elif capability == "asr":
        expected_output_check = """  EXPECT_EQ(output->at(0).req_id, 1u);
  EXPECT_EQ(output->at(0).sub_id, 0u);
  EXPECT_EQ(output->at(0).data, "transcribed_audio");"""
    else:
        expected_output_check = """  EXPECT_EQ(output->at(0).req_id, 1u);
  EXPECT_EQ(output->at(0).sub_id, 0u);"""

    return f"""#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_definition.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "core/validated_node_plan.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {{

TEST(CustomNodeCatalogTest, {name}_RegistrationAndInstantiation) {{
  const auto def = PipelineCatalog::FindNode({cpp_string(name)});
  ASSERT_TRUE(def.has_value());
  EXPECT_EQ(def->category, "custom");
  EXPECT_EQ(def->model_capability, {cpp_string(capability)});
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->Name(), {cpp_string(name)});
}}

TEST(CustomNodeCatalogTest, {name}_EmptyBatchPassesThrough) {{
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  auto mock_model = std::make_shared<test::{mock_class}>();
  ASSERT_TRUE(session.GetModelManager().RegisterModel(
      "test_model", mock_model, "test-v1", mock_model->ModelType(),
      mock_model->Capability(), "test_backend"));
  ASSERT_TRUE(InitNodeForTest(*node, {{{{"bind_model", "test_model"}}}}, &session));
  AlgContext ctx;
  ctx.Publish({cpp_string(in_name)}, {in_type}{{}});
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* output = ctx.Read<{out_type}>({cpp_string(out_name)});
  ASSERT_NE(output, nullptr);
  EXPECT_TRUE(output->empty());
}}

TEST(CustomNodeCatalogTest, {name}_ControlledExecutionAndModelFailure) {{
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  auto mock_model = std::make_shared<test::{mock_class}>();
  ASSERT_TRUE(session.GetModelManager().RegisterModel(
      "test_model", mock_model, "test-v1", mock_model->ModelType(),
      mock_model->Capability(), "test_backend"));
  ASSERT_TRUE(InitNodeForTest(*node, {{{{"bind_model", "test_model"}}}}, &session));

  // 1. Missing input fails without publishing output
  {{
    AlgContext ctx;
    EXPECT_NE(node->Process(&ctx), 0);
    EXPECT_FALSE(ctx.Has({cpp_string(out_name)}));
  }}

  // 2. Normal execution succeeds
  {{
    AlgContext ctx;
    {in_type} input;
    input.emplace_back(101, 1, {sample_in_1});
    input.emplace_back(101, 2, {sample_in_2});
    ctx.Publish({cpp_string(in_name)}, std::move(input));
    ASSERT_EQ(node->Process(&ctx), 0);
    const auto* output = ctx.Read<{out_type}>({cpp_string(out_name)});
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 2u);
    EXPECT_EQ(output->at(0).req_id, 101u);
    EXPECT_EQ(output->at(0).sub_id, 1u);
  }}

  // 3. Model failure fails without publishing output
  {{
    mock_model->fail_ = true;
    AlgContext ctx;
    {in_type} input;
    input.emplace_back(101, 1, {sample_in_1});
    ctx.Publish({cpp_string(in_name)}, std::move(input));
    EXPECT_NE(node->Process(&ctx), 0);
    EXPECT_FALSE(ctx.Has({cpp_string(out_name)}));
    mock_model->fail_ = false;
  }}

  // 4. Output count mismatch fails with -8103
  {{
    mock_model->return_wrong_count_ = true;
    AlgContext ctx;
    {in_type} input;
    input.emplace_back(101, 1, {sample_in_1});
    input.emplace_back(101, 2, {sample_in_2});
    ctx.Publish({cpp_string(in_name)}, std::move(input));
    EXPECT_EQ(node->Process(&ctx), -8103);
    EXPECT_FALSE(ctx.Has({cpp_string(out_name)}));
    mock_model->return_wrong_count_ = false;
  }}

  // 5. Corrupted provenance fails with -8103
  {{
    mock_model->corrupt_provenance_ = true;
    AlgContext ctx;
    {in_type} input;
    input.emplace_back(101, 1, {sample_in_1});
    ctx.Publish({cpp_string(in_name)}, std::move(input));
    EXPECT_EQ(node->Process(&ctx), -8103);
    EXPECT_FALSE(ctx.Has({cpp_string(out_name)}));
    mock_model->corrupt_provenance_ = false;
  }}
}}

TEST(CustomNodeCatalogTest, {name}_BusinessExample) {{
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  SessionContext session;
  auto mock_model = std::make_shared<test::{mock_class}>();
  ASSERT_TRUE(session.GetModelManager().RegisterModel(
      "test_model", mock_model, "test-v1", mock_model->ModelType(),
      mock_model->Capability(), "test_backend"));
  ASSERT_TRUE(InitNodeForTest(*node, {{{{"bind_model", "test_model"}}}}, &session));
  AlgContext ctx;
  {in_type} input;
  input.emplace_back(1, 0, {sample_in_1});
  ctx.Publish({cpp_string(in_name)}, std::move(input));
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* output = ctx.Read<{out_type}>({cpp_string(out_name)});
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 1u);
{expected_output_check}
}}

}}  // namespace llm_edgeflow
"""


def updated_cmakelists(cmake_path, filename):
    content = cmake_path.read_text(encoding="utf-8")
    if re.search(r"(?m)^\s*" + re.escape(filename) + r"\s*$", content):
        return content
    match = re.search(r"(target_sources\(edgeflow_capability_nodes_objects\s+PRIVATE[^)]*)(\))", content)
    if not match:
        raise ValueError(f"No edgeflow_capability_nodes_objects target_sources in {cmake_path}")
    return content[:match.end(1)].rstrip() + f"\n  {filename}\n" + content[match.start(2):]


def updated_custom_node_tests_cmake(cmake_path, filename):
    content = cmake_path.read_text(encoding="utf-8")
    entry = f'  "${{PROJECT_SOURCE_DIR}}/tests/unit/nodes/{filename}"'
    if filename in content:
        return content
    match = re.search(r"(set\(EDGEFLOW_CUSTOM_NODE_TEST_SRCS[^)]*)(\))", content)
    if not match:
        raise ValueError(f"No EDGEFLOW_CUSTOM_NODE_TEST_SRCS in {cmake_path}")
    return content[:match.end(1)].rstrip() + f"\n{entry}\n" + content[match.start(2):]


def add_to_cmakelists(cmake_path, filename):
    try:
        content = updated_cmakelists(cmake_path, filename)
    except (OSError, ValueError):
        return False
    cmake_path.write_text(content, encoding="utf-8")
    return True


class ChangePlan:
    """Publish without clobbering targets; retain displaced edits on conflict.

    Directory locks serialize generators. Existing files are moved aside before
    checking their content, then new versions are linked with no-replace semantics.
    A concurrent editor creating the destination wins; displaced content is restored
    without overwriting that editor, or retained at an explicitly reported path.
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
        # link() atomically refuses an existing destination, including symlinks.
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
                        os.link(staged, path)  # Never replace a concurrent new file.
                        self.created_paths.append((path, content))
                    finally:
                        staged.unlink(missing_ok=True)
                for path, (original, content) in self.modified_files.items():
                    staged = self._stage(path, content)
                    backup = self._stage(path, "")
                    try:
                        # Capture the actual current file atomically; check the captured
                        # version, not a pathname which may change before publication.
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
                # Detect edits through an open descriptor to the displaced inode.
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


def get_runner_info(root: Path, build_dir: Path = None):
    cache = (build_dir / "CMakeCache.txt") if build_dir else None
    if not cache or not cache.exists():
        cache = root / "build" / "CMakeCache.txt"
    if cache.exists():
        text = cache.read_text(encoding="utf-8", errors="replace")
        if "LLM_EDGEFLOW_SHARDED_TEST_RUNNERS:BOOL=OFF" in text:
            return "test_common_nodes", "test_common_nodes"
    return "edgeflow_test_nodes_runner", "edgeflow_test_nodes_runner"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("node_name", nargs="?")
    parser.add_argument("-k", "--kind", choices=["compute", "model", "unary_inference"], default="compute")
    parser.add_argument("-m", "--model-capability", choices=list(CAPABILITY_MAP))
    parser.add_argument("-d", "--description", default="")
    parser.add_argument("-i", "--in-port", help="name:Batch[:1:1[:preserve]]")
    parser.add_argument("-o", "--out-port", help="name:Batch[:1:1[:preserve]]")
    parser.add_argument("--output-dir", default="src/custom_nodes")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("-f", "--force", action="store_true")
    parser.add_argument("--add-to-cmake", action="store_true")
    parser.add_argument("--generate-test", action="store_true", help="Print a starter Google Test snippet; add it to an existing suite")
    parser.add_argument("--write-test", action="store_true", help="Write a standalone test file in tests/unit/nodes/test_<snake_name>.cpp")
    parser.add_argument("--control-id", type=int, help="Generate the text-prefix Control starter using an unused custom command ID (>=1000)")
    parser.add_argument("--self-test", action="store_true", help="Run the generator's Python tests")
    args = parser.parse_args()
    root = Path(os.environ.get("LLM_EDGEFLOW_REPO_ROOT", Path(__file__).resolve().parent.parent))
    if args.self_test:
        import subprocess
        return subprocess.call([sys.executable, str(root / "tests/tooling/test_scaffold_custom_node.py")])
    if not args.node_name or not re.fullmatch(r"[A-Z][A-Za-z0-9]*", args.node_name):
        parser.error("node_name must be a PascalCase C++ identifier")

    if args.write_test and args.generate_test:
        parser.error("--write-test and --generate-test cannot be used together; --write-test writes a standalone test file")
    if args.write_test and args.force:
        parser.error("--write-test rejects --force to prevent multi-file overwrite; remove existing files explicitly")
    if args.write_test and args.output_dir != "src/custom_nodes":
        parser.error("--write-test requires the standard source directory src/custom_nodes")

    name = args.node_name if args.node_name.endswith("Node") else args.node_name + "Node"
    if args.kind == "compute" and args.model_capability:
        parser.error("--model-capability requires --kind model or unary_inference")
    capability = (args.model_capability or "llm") if args.kind != "compute" else None
    signature = CAPABILITY_MAP.get(capability, (None, "TextBatch", "TextBatch"))
    try:
        in_port = parse_port_spec(args.in_port or f"input:{signature[1]}", "input")
        out_port = parse_port_spec(args.out_port or f"output:{signature[2]}", "output")
        content = render_node(name, args.description or f"Custom algorithm node {name}.",
                              args.kind, capability, in_port, out_port, args.control_id)

        target = root / args.output_dir / (to_snake_case(name) + ".cpp")
        test_filename = f"test_{to_snake_case(name)}.cpp"
        test_target = root / "tests/unit/nodes" / test_filename
        cmake_path = (target.parent / "CMakeLists.txt") if not args.write_test and args.output_dir != "src/custom_nodes" else (root / "src/custom_nodes/CMakeLists.txt")
        test_cmake_path = root / "cmake_ext/CustomNodeTests.cmake"

        if not args.write_test:
            # Preserve existing legacy behavior when --write-test is not given
            if not args.dry_run:
                if target.exists() and not args.force:
                    raise ValueError(f"Target already exists: {target}; use --force to overwrite")
                cmake_content = updated_cmakelists(cmake_path, target.name) if args.add_to_cmake else None
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content, encoding="utf-8")
                if cmake_content is not None:
                    cmake_path.write_text(cmake_content, encoding="utf-8")
                print(f"Created {target}")
                if args.control_id is not None:
                    print("Next: edit ReadPrefix shared by config and Control, and its same-file schema. Walkthrough: doc/dev_guide/first_control.md")
                if args.kind == "model" and capability == "llm":
                    print("Next: edit BuildPrompt and FormatAnswer. Walkthrough: doc/dev_guide/first_custom_node.md")
            else:
                print(content)
            if args.generate_test:
                print("// Starter test snippet (add to an existing test suite):")
                print(render_test_stub(name))
                if args.control_id is not None:
                    print(render_control_test_stub(name, args.control_id, in_port[0], out_port[0]))
            return 0

        # --write-test mode
        test_content = render_standalone_test(name, args.description or f"Custom algorithm node {name}.",
                                              args.kind, capability, in_port, out_port, args.control_id)

        plan = ChangePlan()
        plan.add_new_file(target, content)
        plan.add_new_file(test_target, test_content)

        if args.add_to_cmake:
            orig_cmake = cmake_path.read_text(encoding="utf-8")
            new_cmake = updated_cmakelists(cmake_path, target.name)
            plan.add_modification(cmake_path, orig_cmake, new_cmake)

            orig_test_cmake = test_cmake_path.read_text(encoding="utf-8")
            new_test_cmake = updated_custom_node_tests_cmake(test_cmake_path, test_filename)
            plan.add_modification(test_cmake_path, orig_test_cmake, new_test_cmake)

        if args.dry_run:
            print(f"--- {target} (new file) ---")
            print(content)
            print(f"--- {test_target} (new test file) ---")
            print(test_content)
            if args.add_to_cmake:
                print(f"--- {cmake_path} registration ---")
                print(f"+  {target.name}")
                print(f"--- {test_cmake_path} registration ---")
                print(f"+  {test_filename}")
            return 0

        plan.commit()

        print(f"Created {target}")
        print(f"Created {test_target}")
        if args.add_to_cmake:
            print(f"Registered {target.name} in {cmake_path}")
            print(f"Registered {test_filename} in {test_cmake_path}")
            runner_target, runner_binary = get_runner_info(root)
            print("Next steps:")
            print(f"  Build command: cmake --build build --target {runner_target}")
            print(f"  Test filter: CustomNodeCatalogTest.{name}_*")
            print(f"  Run command: ctest --test-dir build -R CommonNodesTest")
            print(f"  (or: ./build/{runner_binary} --gtest_filter=\"CustomNodeCatalogTest.{name}_*\")")
        else:
            print("Pending registrations:")
            print(f"  Add {target.name} to {cmake_path}")
            print(f"  Add {test_filename} to {test_cmake_path}")

        if args.control_id is not None:
            print("Next: edit ReadPrefix shared by config and Control, and its same-file schema. Walkthrough: doc/dev_guide/first_control.md")
        if args.kind == "model" and capability == "llm":
            print("Next: edit BuildPrompt and FormatAnswer. Walkthrough: doc/dev_guide/first_custom_node.md")

    except (ValueError, OSError, RuntimeError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
