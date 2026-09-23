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
    "llm": ("LlmCall", "TextBatch", "TextBatch", "Generate"),
    "embedding": ("EmbeddingCall", "TextBatch", "EmbeddingBatch", "Embed"),
    "asr": ("AsrCall", "AudioPcmBatch", "TextBatch", "Transcribe"),
    "ocr": ("OcrCall", "ImageRefBatch", "OcrDocumentBatch", "Recognize"),
    "rerank": ("RerankCall", "QueryCandidatesBatch", "ScoreBatch", "Score"),
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


def render_map_node(name, description, in_port, out_port):
    in_name, in_type, in_card, in_prov = in_port
    out_name, out_type, out_card, out_prov = out_port
    spec_func = f"{name}Spec"
    return f"""#include <string>

#include "nodes/authoring.h"

namespace llm_edgeflow {{
namespace custom_nodes {{
namespace {{
// Map starter: transforms each input item independently while preserving provenance.
static std::string Transform(const std::string& input) {{
  // TODO: Replace with your domain logic.
  return input;
}}

auto {spec_func}() {{
  return MakeMapSpec(
      Input<{in_type}>({cpp_string(in_name)}),
      Output<{out_type}>({cpp_string(out_name)}),
      &Transform)
      .Description({cpp_string(description)});
}}

REGISTER_FUNCTION_NODE({name}, {spec_func}());

}}  // namespace
}}  // namespace custom_nodes
}}  // namespace llm_edgeflow
"""


def render_llm_starter(name, description, in_name, out_name):
    source = STARTER_LLM_TEMPLATE.read_text(encoding="utf-8")
    source = source.replace("StarterLlmNode", name)
    source = source.replace("StarterLlmSpec", f"{name}Spec")
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

static NodeResult<EmbeddingBatch> Run(const Inputs& input, const NoParameters&,
                                      const Models& models) {{
  // TODO: Add domain preprocessing or postprocessing as needed.
  return models.encoder.Embed(*input.texts);
}}

auto {name}Spec() {{
  return MakeBatchSpec(
      InputsOf<Inputs>({{Required({cpp_string(in_name)}, &Inputs::texts)}}),
      PreservedOutput<EmbeddingBatch>({cpp_string(out_name)}, {cpp_string(in_name)}),
      ModelsOf<Models>({{Embedding("encoder", "bind_model", &Models::encoder)}}),
      &Run)
      .Description({cpp_string(description)});
}}

REGISTER_FUNCTION_NODE({name}, {name}Spec());

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
        source = source.replace("StarterControlNode", name)
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
    model_binding = "ModelsOf<NoModels>{}"
    model_type = "NoModels"
    if kind == "model":
        call, _, _, method = CAPABILITY_MAP[capability]
        models = f"struct Models {{ {call} model; }};\n"
        model_type = "Models"
        model_binding = 'ModelsOf<Models>{Model("model", "bind_model", &Models::model)}'
        processing = f"  return models.model.{method}(*inputs.items);"
    elif preserved and in_type == out_type:
        processing = f"  return NodeResult<{out_type}>::Success(*inputs.items);"
    else:
        processing = f'''  (void)inputs;
  // TODO: Implement the declared transformation and provenance policy.
  // Do not publish default payloads as successful business results.
  return NodeResult<{out_type}>::Failure(
      NodeErrorKind::kBusinessError, "domain transformation is not implemented", -8102);'''
    return f'''#include "nodes/authoring.h"

namespace llm_edgeflow {{
namespace custom_nodes {{
namespace {{
struct Inputs {{ const {in_type}* items = nullptr; }};
{models}
static NodeResult<{out_type}> Run(const Inputs& inputs, const NoParameters&,
                                 const {model_type}&{ " models" if kind == "model" else ""}) {{
{processing}
}}

auto {name}Spec() {{
  return MakeBatchSpec(
      InputsOf<Inputs>{{Required({cpp_string(in_name)}, &Inputs::items{input_flow})}},
      {output},
      {model_binding}, &Run)
      .Description({cpp_string(description)});
}}

REGISTER_FUNCTION_NODE({name}, {name}Spec());
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
    if control_id is None and ((kind == "compute" and in_type == out_type == "TextBatch" and preserved)
                               or (kind == "model" and capability == "llm")):
        if kind == "compute":
            return f"""#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "tests/support/node_harness.h"

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

TEST(CustomNodeCatalogTest, {name}_MapPreservesInputData) {{
  NodeHarness harness({cpp_string(name)});
  harness.TextInput({cpp_string(in_name)}, {{"sample_text_1", "sample_text_2"}});
  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues({cpp_string(out_name)}),
            (std::vector<std::string>{{"sample_text_1", "sample_text_2"}}));
}}

}}  // namespace llm_edgeflow
"""
        elif kind == "model" and capability == "llm":
            return f"""#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "engine/model_interface.h"
#include "tests/support/node_harness.h"
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

TEST(CustomNodeCatalogTest, {name}_ExecutesLlmGeneration) {{
  auto mock_model = std::make_shared<test::ControlledMockLlmModel>();
  NodeHarness harness({cpp_string(name)});
  harness.Config({{ {{"bind_model", "test_model"}} }});
  harness.BindModel("test_model", mock_model);
  harness.TextInput({cpp_string(in_name)}, {{"hello", "world"}});
  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues({cpp_string(out_name)}),
            (std::vector<std::string>{{"mock_answer:hello", "mock_answer:world"}}));
}}

}}  // namespace llm_edgeflow
"""

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

#include "core/common_contracts.h"
#include "core/node_definition.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "tests/support/node_harness.h"
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
  NodeHarness harness({cpp_string(name)});
  harness.Config({{{{"prefix", "initial:"}}}});
  const auto check_output = [&](const std::string& expected) {{
    TextBatch input;
    input.emplace_back(17, 3, "sample");
    harness.CustomInput({cpp_string(in_name)}, std::move(input));
    auto result = harness.Run();
    ASSERT_TRUE(result.ok()) << result.diagnostic();
    const auto* output = result.Output<TextBatch>({cpp_string(out_name)});
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 1u);
    EXPECT_EQ(output->at(0).req_id, 17u);
    EXPECT_EQ(output->at(0).sub_id, 3u);
    EXPECT_EQ(output->at(0).data, expected);
  }};
  check_output("initial:sample");
  const auto update = [&](const nlohmann::json& payload) {{
    return harness.Control({control_id}, payload).status;
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
  NodeHarness harness({cpp_string(name)});
  auto result = harness.Config(invalid).Run();
  EXPECT_TRUE(result.init_failed());
  EXPECT_NE(result.diagnostic().find("prefix exceeds 64 UTF-8 bytes"), std::string::npos);
}}

TEST(CustomNodeCatalogTest, {name}_MissingInputFailsWithoutPublishing) {{
  NodeHarness harness({cpp_string(name)});

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.Output<{out_type}>({cpp_string(out_name)}), nullptr);
}}

TEST(CustomNodeCatalogTest, {name}_BusinessExample) {{
  NodeHarness harness({cpp_string(name)});
  harness.Config({{{{"prefix", "biz:"}}}});

  // Editable business input with independent expected output.
  TextBatch input;
  input.emplace_back(101, 1, "task_sample");
  harness.CustomInput({cpp_string(in_name)}, std::move(input));
  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  const auto* output = result.Output<TextBatch>({cpp_string(out_name)});
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

#include "core/common_contracts.h"
#include "core/node_definition.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "tests/support/node_harness.h"
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
  NodeHarness harness({cpp_string(name)});

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.Output<{out_type}>({cpp_string(out_name)}), nullptr);
}}

TEST(CustomNodeCatalogTest, {name}_EmptyBatchPassesThrough) {{
  NodeHarness harness({cpp_string(name)});

  harness.CustomInput({cpp_string(in_name)}, {in_type}{{}});
  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  const auto* output = result.Output<{out_type}>({cpp_string(out_name)});
  ASSERT_NE(output, nullptr);
  EXPECT_TRUE(output->empty());
}}

TEST(CustomNodeCatalogTest, {name}_PreservesBatchDataAndProvenance) {{
  NodeHarness harness({cpp_string(name)});

  {in_type} input;
  input.emplace_back(101, 3, {sample_in_1});
  input.emplace_back(101, 19, {sample_in_2});
  input.emplace_back(205, 1, {sample_in_3});
  harness.CustomInput({cpp_string(in_name)}, input);
  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  const auto* output = result.Output<{out_type}>({cpp_string(out_name)});
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
  auto repeated = harness.Run();
  ASSERT_TRUE(repeated.ok()) << repeated.diagnostic();
  const auto* repeated_output = repeated.Output<{out_type}>({cpp_string(out_name)});
  ASSERT_NE(repeated_output, nullptr);
  ASSERT_EQ(repeated_output->size(), 3u);
  EXPECT_EQ(repeated_output->at(1).req_id, 101u);
  EXPECT_EQ(repeated_output->at(1).sub_id, 19u);
{payload_check("repeated_output->at(1).data", in_type, sample_in_2, "repeated_expected")}
}}

TEST(CustomNodeCatalogTest, {name}_BusinessExample) {{
  NodeHarness harness({cpp_string(name)});

  {in_type} input;
  input.emplace_back(1, 0, {sample_in_1});
  harness.CustomInput({cpp_string(in_name)}, input);
  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  const auto* output = result.Output<{out_type}>({cpp_string(out_name)});
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

#include "core/common_contracts.h"
#include "core/node_definition.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "tests/support/node_harness.h"
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
  NodeHarness harness({cpp_string(name)});

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.Output<{out_type}>({cpp_string(out_name)}), nullptr);
}}

TEST(CustomNodeCatalogTest, {name}_UnimplementedDomainLogicFailsCleanly) {{
  NodeHarness harness({cpp_string(name)});

  {in_type} input;
  input.emplace_back(1, 0, {sample_in_1});
  harness.CustomInput({cpp_string(in_name)}, std::move(input));
  auto result = harness.Run();
  EXPECT_EQ(result.process_code(), -8102);
  EXPECT_EQ(result.Output<{out_type}>({cpp_string(out_name)}), nullptr);
}}

}}  // namespace llm_edgeflow
"""

    # Model calls
    count_error = "node_error::author_node::kOutputCountMismatch"
    provenance_error = "node_error::author_node::kOutputProvenanceMismatch"
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

#include "core/common_contracts.h"
#include "core/node_definition.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "tests/support/node_harness.h"
#include "tests/support/node_test_utils.h"
#include "nodes/node_error_codes.h"

namespace llm_edgeflow {{

TEST(CustomNodeCatalogTest, {name}_RegistrationAndInstantiation) {{
  const auto def = PipelineCatalog::FindNode({cpp_string(name)});
  ASSERT_TRUE(def.has_value());
  EXPECT_EQ(def->category, "custom");
  ASSERT_EQ(def->model_dependencies.size(), 1U);
  EXPECT_EQ(def->model_dependencies[0].capability, {cpp_string(capability)});
  EXPECT_EQ(def->model_dependencies[0].config_field, "bind_model");
  auto node = NodeRegistry::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->Name(), {cpp_string(name)});
}}

TEST(CustomNodeCatalogTest, {name}_EmptyBatchPassesThrough) {{
  NodeHarness harness({cpp_string(name)});
  auto mock_model = std::make_shared<test::{mock_class}>();
  harness.BindModel("test_model", mock_model);
  harness.Config({{{{"bind_model", "test_model"}}}});

  harness.CustomInput({cpp_string(in_name)}, {in_type}{{}});
  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  const auto* output = result.Output<{out_type}>({cpp_string(out_name)});
  ASSERT_NE(output, nullptr);
  EXPECT_TRUE(output->empty());
}}

TEST(CustomNodeCatalogTest, {name}_ControlledExecutionAndModelFailure) {{
  NodeHarness harness({cpp_string(name)});
  auto mock_model = std::make_shared<test::{mock_class}>();
  harness.BindModel("test_model", mock_model);
  harness.Config({{{{"bind_model", "test_model"}}}});

  // 1. Missing input fails without publishing output
  {{
    auto result = harness.Run();
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.Output<{out_type}>({cpp_string(out_name)}), nullptr);
  }}

  // 2. Normal execution succeeds
  {{
    {in_type} input;
    input.emplace_back(101, 1, {sample_in_1});
    input.emplace_back(101, 2, {sample_in_2});
    harness.CustomInput({cpp_string(in_name)}, std::move(input));
    auto result = harness.Run();
    ASSERT_TRUE(result.ok()) << result.diagnostic();
    const auto* output = result.Output<{out_type}>({cpp_string(out_name)});
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 2u);
    EXPECT_EQ(output->at(0).req_id, 101u);
    EXPECT_EQ(output->at(0).sub_id, 1u);
  }}

  // 3. Model failure fails without publishing output
  {{
    mock_model->fail_ = true;

    {in_type} input;
    input.emplace_back(101, 1, {sample_in_1});
    harness.CustomInput({cpp_string(in_name)}, std::move(input));
    auto result = harness.Run();
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.Output<{out_type}>({cpp_string(out_name)}), nullptr);
    mock_model->fail_ = false;
  }}

  // 4. Output count mismatch fails without publishing
  {{
    mock_model->return_wrong_count_ = true;

    {in_type} input;
    input.emplace_back(101, 1, {sample_in_1});
    input.emplace_back(101, 2, {sample_in_2});
    harness.CustomInput({cpp_string(in_name)}, std::move(input));
    auto result = harness.Run();
    EXPECT_EQ(result.process_code(), {count_error});
    EXPECT_EQ(result.Output<{out_type}>({cpp_string(out_name)}), nullptr);
    mock_model->return_wrong_count_ = false;
  }}

  // 5. Corrupted provenance fails without publishing
  {{
    mock_model->corrupt_provenance_ = true;

    {in_type} input;
    input.emplace_back(101, 1, {sample_in_1});
    harness.CustomInput({cpp_string(in_name)}, std::move(input));
    auto result = harness.Run();
    EXPECT_EQ(result.process_code(), {provenance_error});
    EXPECT_EQ(result.Output<{out_type}>({cpp_string(out_name)}), nullptr);
    mock_model->corrupt_provenance_ = false;
  }}
}}

TEST(CustomNodeCatalogTest, {name}_BusinessExample) {{
  NodeHarness harness({cpp_string(name)});
  auto mock_model = std::make_shared<test::{mock_class}>();
  harness.BindModel("test_model", mock_model);
  harness.Config({{{{"bind_model", "test_model"}}}});

  {in_type} input;
  input.emplace_back(1, 0, {sample_in_1});
  harness.CustomInput({cpp_string(in_name)}, std::move(input));
  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  const auto* output = result.Output<{out_type}>({cpp_string(out_name)});
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
    parser.add_argument("--add-to-cmake", action="store_true")
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

    if args.write_test and args.force:
        parser.error("--write-test rejects --force to prevent multi-file overwrite; remove existing files explicitly")
    if args.write_test and args.output_dir != "src/custom_nodes":
        parser.error("--write-test requires the standard source directory src/custom_nodes")

    name = args.node_name if args.node_name.endswith("Node") else args.node_name + "Node"
    if args.kind == "compute" and args.model_capability:
        parser.error("--model-capability requires --kind model")
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

        plan = ChangePlan()
        if args.force and target.exists():
            plan.add_modification(target, target.read_text(encoding="utf-8"), content)
        else:
            plan.add_new_file(target, content)
        if args.write_test:
            test_content = render_standalone_test(name, args.description or f"Custom algorithm node {name}.",
                                                  args.kind, capability, in_port, out_port, args.control_id)
            plan.add_new_file(test_target, test_content)

        if args.add_to_cmake:
            orig_cmake = cmake_path.read_text(encoding="utf-8")
            new_cmake = updated_cmakelists(cmake_path, target.name)
            plan.add_modification(cmake_path, orig_cmake, new_cmake)

            if args.write_test:
                orig_test_cmake = test_cmake_path.read_text(encoding="utf-8")
                new_test_cmake = updated_custom_node_tests_cmake(test_cmake_path, test_filename)
                plan.add_modification(test_cmake_path, orig_test_cmake, new_test_cmake)

        if args.dry_run:
            if args.write_test:
                print(f"--- {target} (new file) ---")
            print(content)
            if args.write_test:
                print(f"--- {test_target} (new test file) ---")
                print(test_content)
            if args.add_to_cmake:
                print(f"--- {cmake_path} registration ---")
                print(f"+  {target.name}")
                if args.write_test:
                    print(f"--- {test_cmake_path} registration ---")
                    print(f"+  {test_filename}")
            return 0

        plan.commit()

        print(f"Created {target}")
        if args.write_test:
            print(f"Created {test_target}")
        if args.add_to_cmake:
            print(f"Registered {target.name} in {cmake_path}")
            if args.write_test:
                print(f"Registered {test_filename} in {test_cmake_path}")
                print("Next steps:")
                print("  Build command: cmake --build build --target edgeflow_test_nodes_runner")
                print(f"  Test filter: CustomNodeCatalogTest.{name}_*")
                print("  Run command: ctest --test-dir build -R CommonNodesTest")
                print(f"  (or: ./build/edgeflow_test_nodes_runner --gtest_filter=\"CustomNodeCatalogTest.{name}_*\")")

        else:
            print("Pending registrations:")
            print(f"  Add {target.name} to {cmake_path}")
            if args.write_test:
                print(f"  Add {test_filename} to {test_cmake_path}")

        if args.control_id is not None:
            print("Next: edit ApplyPrefix for business logic and Parameters/Field for config and Control validation. Walkthrough: doc/dev_guide/first_control.md")
        if args.kind == "model" and capability == "llm":
            print("Next: edit BuildPrompt and FormatAnswer. Walkthrough: doc/dev_guide/first_custom_node.md")

    except (ValueError, OSError, RuntimeError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
