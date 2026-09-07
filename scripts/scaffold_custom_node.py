#!/usr/bin/env python3
"""LLM-EdgeFlow Custom Node Scaffolding Tool.

Assists solution developers in generating boilerplate, port definitions,
provenance preservation loops, CMake registration, and test stubs for custom nodes.
Complies with Layer 3 architecture invariants and LayerGuard rules.
"""

import argparse
import os
from pathlib import Path
import re
import sys
from typing import Dict, List, Optional, Tuple


CAPABILITY_MAP = {
    "llm": {
        "interface": "ILlmModel",
        "header": "engine/model_interface.h",
        "default_in": ("input", "TextBatch", "std::string"),
        "default_out": ("output", "TextBatch", "std::string"),
        "call_snippet": "model->Generate(prompts, gen_options, &model_outputs);",
    },
    "embedding": {
        "interface": "IEmbeddingModel",
        "header": "engine/model_interface.h",
        "default_in": ("input", "TextBatch", "std::string"),
        "default_out": ("output", "EmbeddingBatch", "std::vector<float>"),
        "call_snippet": "model->Embed(texts, embed_options, &model_outputs);",
    },
    "asr": {
        "interface": "IAsrModel",
        "header": "engine/model_interface.h",
        "default_in": ("audio", "AudioPcmBatch", "AudioPcmData"),
        "default_out": ("text", "TextBatch", "std::string"),
        "call_snippet": "model->Transcribe(audios, asr_options, &model_outputs);",
    },
    "ocr": {
        "interface": "IOcrModel",
        "header": "engine/model_interface.h",
        "default_in": ("image", "ImageRefBatch", "ImageRefData"),
        "default_out": ("boxes", "OcrDocumentBatch", "OcrDocumentData"),
        "call_snippet": "model->Detect(images, &model_outputs);",
    },
    "rerank": {
        "interface": "IRerankModel",
        "header": "engine/model_interface.h",
        "default_in": ("candidates", "QueryCandidatesBatch", "QueryCandidatesData"),
        "default_out": ("ranked", "RankedTextBatch", "RankedTextData"),
        "call_snippet": "model->Score(candidates, &model_outputs);",
    },
}


def to_snake_case(name: str) -> str:
    """Converts PascalCase identifier to snake_case."""
    s = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", s).lower()


def parse_port_spec(spec: str, default_role: str) -> Tuple[str, str, str, str]:
    """Parses port spec: name:DataType[:cardinality[:provenance_policy]]."""
    parts = spec.split(":")
    name = parts[0].strip()
    data_type = parts[1].strip() if len(parts) > 1 else "TextBatch"
    cardinality = parts[2].strip() if len(parts) > 2 else "1:1"
    provenance = parts[3].strip() if len(parts) > 3 else "preserve"
    return name, data_type, cardinality, provenance


def get_item_type_for_batch(data_type: str) -> str:
    """Returns the underlying element payload type for standard batch types."""
    type_map = {
        "TextBatch": "std::string",
        "EmbeddingBatch": "std::vector<float>",
        "RankedTextBatch": "RankedTextData",
        "RuleMatchBatch": "RuleMatchData",
        "StructuredDocumentBatch": "StructuredDocumentData",
        "AudioPcmBatch": "AudioPcmData",
        "OcrDocumentBatch": "OcrDocumentData",
        "QueryCandidatesBatch": "QueryCandidatesData",
        "ImageRefBatch": "ImageRefData",
        "Int32Batch": "int32_t",
    }
    return type_map.get(data_type, "std::string")


def render_compute_node(
    node_name: str,
    description: str,
    in_port: Tuple[str, str, str, str],
    out_port: Tuple[str, str, str, str],
) -> str:
    in_name, in_type, in_card, in_prov = in_port
    out_name, out_type, out_card, out_prov = out_port

    return f"""#include <string>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "contracts/traceable_item.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "nodes/node_support.h"

namespace llm_edgeflow {{
namespace custom_nodes {{

namespace error {{
inline constexpr int kMissingInput = -8101;
inline constexpr int kProcessingFailed = -8102;
}}  // namespace error

/**
 * @brief {description}
 */
class {node_name} final : public NodeBase {{
 public:
  inline static constexpr char kNodeType[] = "{node_name}";

  explicit {node_name}(std::string node_name = kNodeType)
      : NodeBase(std::move(node_name)),
        in_port_("{in_name}"),
        out_port_("{out_name}") {{}}

 protected:
  bool InitNode(const NodeInitContext& init_ctx,
                const nlohmann::json& config,
                SessionContext& session_ctx) override {{
    (void)session_ctx;
    BindPort(init_ctx, in_port_);
    BindPort(init_ctx, out_port_);

    // Extract node configuration here.
    (void)config;
    return true;
  }}

  int ProcessNode(AlgContext& req_ctx) override {{
    const auto* inputs =
        in_port_.Require(req_ctx, error::kMissingInput, "{in_name}");
    if (!inputs) {{
      return error::kMissingInput;
    }}

    {out_type} outputs;
    outputs.reserve(inputs->size());

    for (const auto& item : *inputs) {{
      // TODO: Perform domain computation while preserving (req_id, sub_id) provenance.
      outputs.push_back(MakeTraceableItem(item.payload, item.req_id, item.sub_id));
    }}

    out_port_.Set(req_ctx, std::move(outputs));
    return 0;
  }}

 private:
  BoundInput<{in_type}> in_port_;
  BoundOutput<{out_type}> out_port_;
}};

REGISTER_NODE_WITH_DEFINITION(
    {node_name},
    NodeDefinition{{
        /*node_type=*/{node_name}::kNodeType,
        /*version=*/"1.0.0",
        /*category=*/"custom",
        /*description=*/"{description}",
        /*ports=*/{{
            InputPort{{/*logical_name=*/"{in_name}",
                      /*data_type=*/"{in_type}",
                      /*cardinality=*/"{in_card}",
                      /*provenance_policy=*/"{in_prov}",
                      /*description=*/"Input payload batch"}},
            OutputPort{{/*logical_name=*/"{out_name}",
                       /*data_type=*/"{out_type}",
                       /*cardinality=*/"{out_card}",
                       /*provenance_policy=*/"{out_prov}",
                       /*description=*/"Output payload batch"}},
        }},
        /*config_schema=*/nlohmann::json::object(),
        /*control_schema=*/nlohmann::json::object(),
        /*model_capability=*/std::nullopt,
        /*model_config_field=*/std::nullopt,
        /*parallel_safe=*/true,
        /*biz_names=*/{{}},
    }});

}}  // namespace custom_nodes
}}  // namespace llm_edgeflow
"""


def render_model_node(
    node_name: str,
    description: str,
    capability: str,
    in_port: Tuple[str, str, str, str],
    out_port: Tuple[str, str, str, str],
) -> str:
    cap_info = CAPABILITY_MAP[capability]
    interface_name = cap_info["interface"]
    in_name, in_type, in_card, in_prov = in_port
    out_name, out_type, out_card, out_prov = out_port

    return f"""#include <string>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "contracts/traceable_item.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "{cap_info['header']}"
#include "nodes/model_bound_node.h"
#include "nodes/node_support.h"

namespace llm_edgeflow {{
namespace custom_nodes {{

namespace error {{
inline constexpr int kMissingInput = -8201;
inline constexpr int kModelInferenceFailed = -8202;
}}  // namespace error

/**
 * @brief {description}
 */
class {node_name} final : public ModelBoundNode<{interface_name}> {{
 public:
  inline static constexpr char kNodeType[] = "{node_name}";

  explicit {node_name}(std::string node_name = kNodeType)
      : ModelBoundNode<{interface_name}>(std::move(node_name)),
        in_port_("{in_name}"),
        out_port_("{out_name}") {{}}

 protected:
  bool InitModelNode(const NodeInitContext& init_ctx,
                     const nlohmann::json& config,
                     SessionContext& session_ctx) override {{
    (void)session_ctx;
    BindPort(init_ctx, in_port_);
    BindPort(init_ctx, out_port_);

    // Extract model parameters and options from config:
    (void)config;
    return true;
  }}

  int ProcessNode(AlgContext& req_ctx) override {{
    const auto* inputs =
        in_port_.Require(req_ctx, error::kMissingInput, "{in_name}");
    if (!inputs) {{
      return error::kMissingInput;
    }}

    auto* model = GetModel();
    if (!model) {{
      ALG_LOG_ERROR("[{node_name}] Bound model interface is null\\n");
      return error::kModelInferenceFailed;
    }}

    // TODO: Perform pre-processing, call model capability, and post-process.
    // Ensure all published results preserve original (req_id, sub_id) provenance.
    {out_type} outputs;
    outputs.reserve(inputs->size());

    out_port_.Set(req_ctx, std::move(outputs));
    return 0;
  }}

 private:
  BoundInput<{in_type}> in_port_;
  BoundOutput<{out_type}> out_port_;
}};

REGISTER_NODE_WITH_DEFINITION(
    {node_name},
    NodeDefinition{{
        /*node_type=*/{node_name}::kNodeType,
        /*version=*/"1.0.0",
        /*category=*/"custom",
        /*description=*/"{description}",
        /*ports=*/{{
            InputPort{{/*logical_name=*/"{in_name}",
                      /*data_type=*/"{in_type}",
                      /*cardinality=*/"{in_card}",
                      /*provenance_policy=*/"{in_prov}",
                      /*description=*/"Input payload batch"}},
            OutputPort{{/*logical_name=*/"{out_name}",
                       /*data_type=*/"{out_type}",
                       /*cardinality=*/"{out_card}",
                       /*provenance_policy=*/"{out_prov}",
                       /*description=*/"Output payload batch"}},
        }},
        /*config_schema=*/nlohmann::json::object(),
        /*control_schema=*/nlohmann::json::object(),
        /*model_capability=*/"{capability}",
        /*model_config_field=*/"model_id",
        /*parallel_safe=*/true,
        /*biz_names=*/{{}},
    }});

}}  // namespace custom_nodes
}}  // namespace llm_edgeflow
"""


def render_unary_inference_node(
    node_name: str,
    description: str,
    capability: str,
    in_port: Tuple[str, str, str, str],
    out_port: Tuple[str, str, str, str],
) -> str:
    cap_info = CAPABILITY_MAP[capability]
    interface_name = cap_info["interface"]
    in_name, in_type, _, _ = in_port
    out_name, out_type, _, _ = out_port
    in_item = get_item_type_for_batch(in_type)
    out_item = get_item_type_for_batch(out_type)

    return f"""#include <string>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "contracts/traceable_item.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "{cap_info['header']}"
#include "nodes/traceable_unary_inference_node.h"

namespace llm_edgeflow {{
namespace custom_nodes {{

namespace error {{
inline constexpr int kMissingInput = -8301;
inline constexpr int kInferenceFailed = -8302;
inline constexpr int kCountMismatch = -8303;
inline constexpr int kProvenanceMismatch = -8304;
}}  // namespace error

/**
 * @brief {description}
 */
class {node_name} final
    : public TraceableUnaryInferenceNode<{interface_name}, {in_item}, {out_item}> {{
 public:
  inline static constexpr char kNodeType[] = "{node_name}";

  explicit {node_name}(std::string node_name = kNodeType)
      : TraceableUnaryInferenceNode<{interface_name}, {in_item}, {out_item}>(
            std::move(node_name),
            /*input_port_name=*/"{in_name}",
            /*output_port_name=*/"{out_name}",
            /*missing_input_error=*/error::kMissingInput,
            /*count_mismatch_error=*/error::kCountMismatch,
            /*provenance_mismatch_error=*/error::kProvenanceMismatch) {{}}

 protected:
  int InferBatch(const InputBatch& input, OutputBatch* output) override {{
    if (!output) {{
      return error::kInferenceFailed;
    }}
    output->clear();
    output->reserve(input.size());

    auto* model = this->GetModel();
    if (!model) {{
      ALG_LOG_ERROR("[{node_name}] Bound model interface is null\\n");
      return error::kInferenceFailed;
    }}

    // TODO: Perform 1:1 inference while preserving provenance:
    for (const auto& item : input) {{
      {out_item} result_item{{}};
      output->push_back(
          MakeTraceableItem(std::move(result_item), item.req_id, item.sub_id));
    }}
    return 0;
  }}
}};

REGISTER_NODE_WITH_DEFINITION(
    {node_name},
    NodeDefinition{{
        /*node_type=*/{node_name}::kNodeType,
        /*version=*/"1.0.0",
        /*category=*/"custom",
        /*description=*/"{description}",
        /*ports=*/{{
            InputPort{{/*logical_name=*/"{in_name}",
                      /*data_type=*/"{in_type}",
                      /*cardinality=*/"1:1",
                      /*provenance_policy=*/"preserve",
                      /*description=*/"Input payload batch"}},
            OutputPort{{/*logical_name=*/"{out_name}",
                       /*data_type=*/"{out_type}",
                       /*cardinality=*/"1:1",
                       /*provenance_policy=*/"preserve",
                       /*description=*/"Output payload batch"}},
        }},
        /*config_schema=*/nlohmann::json::object(),
        /*control_schema=*/nlohmann::json::object(),
        /*model_capability=*/"{capability}",
        /*model_config_field=*/"model_id",
        /*parallel_safe=*/true,
        /*biz_names=*/{{}},
    }});

}}  // namespace custom_nodes
}}  // namespace llm_edgeflow
"""


def render_test_stub(node_name: str) -> str:
    return f"""#include <gtest/gtest.h>

#include "core/node_registry.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {{
namespace {{

TEST(CustomNodeCatalogTest, {node_name}RegistrationAndInstantiation) {{
  const auto* def = PipelineCatalog::Instance().FindNodeDefinition("{node_name}");
  ASSERT_NE(def, nullptr);
  EXPECT_EQ(def->node_type, "{node_name}");
  EXPECT_EQ(def->category, "custom");
  EXPECT_FALSE(def->ports.empty());

  auto node = NodeRegistry::Instance().Create("{node_name}");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->Name(), "{node_name}");
}}

}}  // namespace
}}  // namespace llm_edgeflow
"""


def add_to_cmakelists(cmake_path: Path, filename: str) -> bool:
    """Adds filename to src/custom_nodes/CMakeLists.txt target_sources."""
    if not cmake_path.exists():
        return False
    content = cmake_path.read_text(encoding="utf-8")
    if filename in content:
        return True

    # Find the target_sources closing parenthesis
    pattern = r"(target_sources\(edgeflow_layer3_node_objects\s+PRIVATE[\s\S]*?)(\))"
    match = re.search(pattern, content)
    if not match:
        return False

    prefix, suffix = match.group(1), match.group(2)
    if not prefix.endswith("\n"):
        prefix += "\n"
    updated = content[:match.start()] + prefix + f"  {filename}\n" + suffix + content[match.end():]
    cmake_path.write_text(updated, encoding="utf-8")
    return True


def run_self_tests():
    """Runs internal self-tests validating template rendering."""
    print("Running scaffold_custom_node internal self-tests...")
    # 1. to_snake_case
    assert to_snake_case("DomainSummaryNode") == "domain_summary_node"
    assert to_snake_case("OCRPreprocessNode") == "ocr_preprocess_node"
    assert to_snake_case("PromptGuidedLlmNode") == "prompt_guided_llm_node"

    # 2. compute rendering
    code = render_compute_node(
        "FooNode", "Test compute node",
        ("in", "TextBatch", "1:1", "preserve"),
        ("out", "TextBatch", "1:1", "preserve")
    )
    assert "class FooNode final : public NodeBase" in code
    assert "REGISTER_NODE_WITH_DEFINITION" in code
    assert "/*category=*/\"custom\"" in code

    # 3. model rendering
    code = render_model_node(
        "BarLlmNode", "Test LLM node", "llm",
        ("in", "TextBatch", "1:1", "preserve"),
        ("out", "TextBatch", "1:1", "preserve")
    )
    assert "class BarLlmNode final : public ModelBoundNode<ILlmModel>" in code
    assert "/*model_capability=*/\"llm\"" in code

    # 4. unary inference rendering
    code = render_unary_inference_node(
        "BazAsrNode", "Test ASR node", "asr",
        ("audio", "AudioPcmBatch", "1:1", "preserve"),
        ("text", "TextBatch", "1:1", "preserve")
    )
    assert "class BazAsrNode final" in code
    assert "TraceableUnaryInferenceNode<IAsrModel, AudioPcmData, std::string>" in code

    print("All self-tests passed successfully!")


def main():
    parser = argparse.ArgumentParser(
        description="Scaffold custom nodes for LLM-EdgeFlow solution developers."
    )
    parser.add_argument(
        "node_name",
        nargs="?",
        help="PascalCase node class name, e.g. DomainSummaryNode.",
    )
    parser.add_argument(
        "-k", "--kind",
        choices=["compute", "model", "unary_inference"],
        default="compute",
        help="Node kind: compute (NodeBase), model (ModelBoundNode), unary_inference (TraceableUnaryInferenceNode)",
    )
    parser.add_argument(
        "-m", "--model-capability",
        choices=list(CAPABILITY_MAP.keys()),
        default=None,
        help="Required for model and unary_inference: llm, embedding, asr, ocr, rerank",
    )
    parser.add_argument(
        "-d", "--description",
        default="",
        help="Node description for NodeDefinition catalog metadata",
    )
    parser.add_argument(
        "-i", "--in-port",
        default=None,
        help="Input port spec: name:DataType[:cardinality[:provenance_policy]] (e.g. input:TextBatch:1:1:preserve)",
    )
    parser.add_argument(
        "-o", "--out-port",
        default=None,
        help="Output port spec: name:DataType[:cardinality[:provenance_policy]] (e.g. output:TextBatch:1:1:preserve)",
    )
    parser.add_argument(
        "--output-dir",
        default="src/custom_nodes",
        help="Directory to place the generated node file (default: src/custom_nodes)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print generated C++ code and test snippet to stdout without writing files",
    )
    parser.add_argument(
        "-f", "--force",
        action="store_true",
        help="Overwrite target file if it already exists",
    )
    parser.add_argument(
        "--add-to-cmake",
        action="store_true",
        help="Automatically register new source file in src/custom_nodes/CMakeLists.txt",
    )
    parser.add_argument(
        "--generate-test",
        action="store_true",
        help="Also output/generate starter Google Test stub",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run internal unit tests and exit",
    )

    args = parser.parse_args()

    if args.self_test:
        run_self_tests()
        sys.exit(0)

    if not args.node_name:
        parser.error("the following arguments are required: node_name")

    node_name = args.node_name.strip()
    if not node_name.endswith("Node"):
        node_name += "Node"

    if not re.match(r"^[A-Z][a-zA-Z0-9_]*$", node_name):
        sys.exit(f"Error: '{node_name}' is not a valid C++ PascalCase identifier.")

    snake_name = to_snake_case(node_name)
    filename = f"{snake_name}.cpp"

    desc = args.description or f"Custom algorithm node {node_name}."

    if args.kind in ("model", "unary_inference"):
        if not args.model_capability:
            args.model_capability = "llm"

    cap = args.model_capability

    # Determine default ports
    if cap and cap in CAPABILITY_MAP:
        default_in_name, default_in_type, _ = CAPABILITY_MAP[cap]["default_in"]
        default_out_name, default_out_type, _ = CAPABILITY_MAP[cap]["default_out"]
    else:
        default_in_name, default_in_type = "input", "TextBatch"
        default_out_name, default_out_type = "output", "TextBatch"

    in_spec = args.in_port or f"{default_in_name}:{default_in_type}:1:1:preserve"
    out_spec = args.out_port or f"{default_out_name}:{default_out_type}:1:1:preserve"

    in_port = parse_port_spec(in_spec, "input")
    out_port = parse_port_spec(out_spec, "output")

    if args.kind == "compute":
        content = render_compute_node(node_name, desc, in_port, out_port)
    elif args.kind == "model":
        content = render_model_node(node_name, desc, cap, in_port, out_port)
    elif args.kind == "unary_inference":
        content = render_unary_inference_node(node_name, desc, cap, in_port, out_port)
    else:
        sys.exit(f"Unknown kind: {args.kind}")

    test_content = render_test_stub(node_name)

    if args.dry_run:
        print(f"// === Generated {filename} ===")
        print(content)
        if args.generate_test:
            print(f"// === Generated test snippet ===")
            print(test_content)
        return

    # File output handling
    project_root = Path(__file__).resolve().parent.parent
    target_dir = project_root / args.output_dir
    target_file = target_dir / filename

    if target_file.exists() and not args.force:
        sys.exit(f"Error: Target file '{target_file}' already exists. Use --force to overwrite.")

    target_dir.mkdir(parents=True, exist_ok=True)
    target_file.write_text(content, encoding="utf-8")
    print(f"Created custom node skeleton at: {target_file}")

    if args.add_to_cmake:
        cmake_file = target_dir / "CMakeLists.txt"
        if add_to_cmakelists(cmake_file, filename):
            print(f"Appended {filename} to {cmake_file}")
        else:
            print(f"Warning: Could not automatically append to {cmake_file}. Please register manually.")

    if args.generate_test:
        print("\nStarter Google Test snippet for this node:\n")
        print(test_content)


if __name__ == "__main__":
    main()
