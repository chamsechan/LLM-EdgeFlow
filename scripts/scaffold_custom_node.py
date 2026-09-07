#!/usr/bin/env python3
"""Generate source-based Layer 3 extensions using the existing Node interfaces.

This is an authoring aid, not a second catalog or Pipeline validator. Rebuild and
query alg_pipeline_tool to discover registrations and validate actual wiring.
"""

import argparse
import json
from pathlib import Path
import re
import sys


STARTER_LLM_TEMPLATE = (Path(__file__).resolve().parents[1]
                        / "dev_support/node_authoring/starter_llm_node.cpp")


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


def render_node(name, description, kind, capability, in_port, out_port):
    in_name, in_type, in_card, in_prov = in_port
    out_name, out_type, out_card, out_prov = out_port
    signature = CAPABILITY_MAP.get(capability)
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
#include "core/pipeline_catalog.h"
#include "engine/model_interface.h"
#include "nodes/model_bound_node.h"
#include "nodes/node_support.h"
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
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {{
TEST(CustomNodeCatalogTest, {name}RegistrationAndInstantiation) {{
  const auto def = PipelineCatalog::FindNode({cpp_string(name)});
  ASSERT_TRUE(def.has_value());
  EXPECT_EQ(def->category, "custom");
  EXPECT_FALSE(def->inputs.empty());
  EXPECT_FALSE(def->outputs.empty());
  auto node = NodeFactory::Instance().Create({cpp_string(name)});
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->Name(), {cpp_string(name)});
  // Add domain assertions, missing input, model failure and provenance coverage.
}}
}}  // namespace llm_edgeflow
"""


def updated_cmakelists(cmake_path, filename):
    content = cmake_path.read_text(encoding="utf-8")
    if re.search(r"(?m)^\s*" + re.escape(filename) + r"\s*$", content):
        return content
    match = re.search(r"(target_sources\(edgeflow_layer3_node_objects\s+PRIVATE[^)]*)(\))", content)
    if not match:
        raise ValueError(f"No edgeflow_layer3_node_objects target_sources in {cmake_path}")
    return content[:match.end(1)].rstrip() + f"\n  {filename}\n" + content[match.start(2):]


def add_to_cmakelists(cmake_path, filename):
    try:
        content = updated_cmakelists(cmake_path, filename)
    except (OSError, ValueError):
        return False
    cmake_path.write_text(content, encoding="utf-8")
    return True


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
    parser.add_argument("--self-test", action="store_true", help="Run the generator's Python tests")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    if args.self_test:
        import subprocess
        return subprocess.call([sys.executable, str(root / "tests/tooling/test_scaffold_custom_node.py")])
    if not args.node_name or not re.fullmatch(r"[A-Z][A-Za-z0-9]*", args.node_name):
        parser.error("node_name must be a PascalCase C++ identifier")
    name = args.node_name if args.node_name.endswith("Node") else args.node_name + "Node"
    if args.kind == "compute" and args.model_capability:
        parser.error("--model-capability requires --kind model or unary_inference")
    capability = (args.model_capability or "llm") if args.kind != "compute" else None
    signature = CAPABILITY_MAP.get(capability, (None, "TextBatch", "TextBatch"))
    try:
        in_port = parse_port_spec(args.in_port or f"input:{signature[1]}", "input")
        out_port = parse_port_spec(args.out_port or f"output:{signature[2]}", "output")
        content = render_node(name, args.description or f"Custom algorithm node {name}.",
                              args.kind, capability, in_port, out_port)
        target = root / args.output_dir / (to_snake_case(name) + ".cpp")
        if not args.dry_run:
            if target.exists() and not args.force:
                raise ValueError(f"Target already exists: {target}; use --force to overwrite")
            cmake_path = target.parent / "CMakeLists.txt"
            # Validate all destinations before writing any source.
            cmake_content = updated_cmakelists(cmake_path, target.name) if args.add_to_cmake else None
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8")
            if cmake_content is not None:
                cmake_path.write_text(cmake_content, encoding="utf-8")
            print(f"Created {target}")
            if args.kind == "model" and capability == "llm":
                print("Next: edit BuildPrompt and FormatAnswer. Walkthrough: doc/dev_guide/first_custom_node.md")
        else:
            print(content)
        if args.generate_test:
            print("// Starter test snippet (add to an existing test suite):")
            print(render_test_stub(name))
    except (ValueError, OSError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
