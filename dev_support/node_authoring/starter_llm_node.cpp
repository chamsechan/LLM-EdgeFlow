#include <string>
#include <utility>

#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "engine/model_interface.h"
#include "nodes/model_bound_node.h"
#include "nodes/traceable_batch_validation.h"

namespace llm_edgeflow {
namespace custom_nodes {
namespace {

// LLM starter: see doc/dev_guide/first_custom_node.md for the authoring
// walkthrough.
class StarterLlmNode final : public ModelBoundNode<ILlmModel> {
  // Start here: these two functions work on text, not platform structures.
  static std::string BuildPrompt(const std::string& text) { return text; }

  static std::string FormatAnswer(const std::string& text) { return text; }

 public:
  inline static constexpr char kNodeType[] = "StarterLlmNode";
  inline static constexpr BlackboardKey<TextBatch> kInput{"input", "TextBatch"};
  inline static constexpr BlackboardKey<TextBatch> kOutput{"output",
                                                           "TextBatch"};

  StarterLlmNode()
      : ModelBoundNode<ILlmModel>(kNodeType),
        input_(kInput.name),
        output_(kOutput.name) {}

 protected:
  // ModelBoundNode validates fields and applies Definition defaults before this
  // hook.
  bool InitModelNode(const NodeInitContext& init_ctx, const nlohmann::json&,
                     SessionContext&) override {
    BindPort(init_ctx, input_);
    BindPort(init_ctx, output_);
    return true;
  }

  int ProcessNode(AlgContext& ctx) override {
    const auto* inputs = input_.Require(ctx, -8101);
    if (!inputs) return -8101;
    TextBatch outputs;
    if (inputs->empty()) {
      output_.Set(ctx, std::move(outputs));
      return 0;
    }

    // 1. Prepare each prompt. Keep the original request and item identifiers.
    TextBatch prompts;
    prompts.reserve(inputs->size());
    for (const auto& item : *inputs) {
      prompts.emplace_back(item.req_id, item.sub_id, BuildPrompt(item.data));
    }

    // 2. Call the bound model. Reject failures before interpreting any output.
    const int ret = model()->Generate(prompts, GenerateOptions{}, &outputs);
    if (ret != 0) return Fail(ctx, ret, Name() + ": model inference failed");
    if (!ValidatePreservedTraceableAlignment(*inputs, outputs).IsAligned()) {
      return Fail(ctx, -8103, Name() + ": output count or provenance mismatch");
    }

    // 3. Format each answer. Only data changes; provenance stays intact.
    for (auto& item : outputs) item.data = FormatAnswer(item.data);
    output_.Set(ctx, std::move(outputs));
    return 0;
  }

 private:
  BoundInput<TextBatch> input_;
  BoundOutput<TextBatch> output_;
};

NodeDefinition MakeStarterLlmNodeDefinition() {
  NodeDefinition def;
  def.node_type = StarterLlmNode::kNodeType;
  def.category = "custom";
  def.description = "LLM authoring starter";
  def.inputs = {RequiredInputPort(StarterLlmNode::kInput.name,
                                  StarterLlmNode::kInput, "1:1", "preserve",
                                  "request")};
  def.outputs = {OutputPort(StarterLlmNode::kOutput.name,
                            StarterLlmNode::kOutput, "1:1", "preserve",
                            "request")};
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, true}};
  def.model_capability = "llm";
  def.model_config_field = "bind_model";
  // Review node-owned shared state before enabling parallel scheduling.
  def.parallel_safe = false;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(StarterLlmNode, MakeStarterLlmNodeDefinition());

}  // namespace
}  // namespace custom_nodes
}  // namespace llm_edgeflow
