#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "contracts/control_payload.h"
#include "core/node_registry.h"
#include "nodes/node_base.h"

namespace llm_edgeflow::custom_nodes {
namespace {

// Generated into the author's Node; this template is not a production Node.
class StarterControlNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "StarterControlNode";
  // Choose a stable, unused custom ID using the current Catalog.
  inline static constexpr int kUpdatePrefix = 1001;
  inline static constexpr BlackboardKey<TextBatch> kInput{"input", "TextBatch"};
  inline static constexpr BlackboardKey<TextBatch> kOutput{"output",
                                                           "TextBatch"};

  static const std::vector<ConfigFieldDefinition>& PrefixConfigFields() {
    static const std::vector<ConfigFieldDefinition> fields = {
        {"prefix",
         ConfigValueKind::kString,
         false,
         "",
         std::nullopt,
         std::nullopt,
         {},
         "Text prepended to each input; at most 64 UTF-8 bytes. "
         "Control replaces this initial value."}};
    return fields;
  }

  // Share parsing and business checks across initial configuration and Control.
  static bool ReadPrefix(const nlohmann::json& config, std::string* prefix,
                         std::string* diagnostic) {
    nlohmann::json normalized;
    std::vector<ConfigFieldValidationError> errors;
    if (!ValidateAndNormalizeFields(PrefixConfigFields(), config, &normalized,
                                    &errors)) {
      if (diagnostic && !errors.empty()) *diagnostic = errors.front().message;
      return false;
    }
    std::string next = normalized.at("prefix").get<std::string>();
    if (next.size() > 64) {
      if (diagnostic) *diagnostic = "prefix exceeds 64 UTF-8 bytes";
      return false;
    }
    *prefix = std::move(next);
    return true;
  }

  static const ControlCommandDefinition& PrefixCommand() {
    static const ControlCommandDefinition command(
        kUpdatePrefix, "set_prefix",
        "Replace the text prefix (at most 64 bytes)",
        {{"type", "object"},
         {"required", {"prefix"}},
         {"additionalProperties", false},
         {"properties", {{"prefix", {{"type", "string"}}}}}},
        true);
    return command;
  }

  StarterControlNode()
      : NodeBase(kNodeType), input_(kInput.name), output_(kOutput.name) {}

 protected:
  bool InitNode(const NodeInitContext& ctx, const nlohmann::json& config,
                SessionContext&) override {
    std::string next;
    std::string error;
    if (!ReadPrefix(config, &next, &error)) return ctx.Fail(error);
    BindPort(ctx, input_);
    BindPort(ctx, output_);
    std::unique_lock<std::shared_mutex> lock(config_mutex_);
    prefix_.swap(next);
    return true;
  }

  NodeControlResult ControlNode(int cmd, const std::string& text) override {
    if (cmd != kUpdatePrefix) return NodeControlResult::Unsupported();
    nlohmann::json payload;
    std::string error;
    if (!ParseControlPayload(text, PrefixCommand().payload_schema, &payload,
                             &error)) {
      return NodeControlResult::Failed(-1, std::move(error));
    }
    std::string next;
    if (!ReadPrefix(payload, &next, &error)) {
      return NodeControlResult::Failed(-1, std::move(error));
    }
    std::unique_lock<std::shared_mutex> lock(config_mutex_);
    prefix_.swap(next);
    return NodeControlResult::Handled();
  }

  int ProcessNode(AlgContext& ctx) override {
    const auto* inputs = input_.Require(ctx, -8101);
    if (!inputs) return -8101;
    std::string prefix;
    {
      std::shared_lock<std::shared_mutex> lock(config_mutex_);
      prefix = prefix_;  // One consistent value for the whole request batch.
    }
    TextBatch outputs;
    outputs.reserve(inputs->size());
    for (const auto& item : *inputs) {
      outputs.emplace_back(item.req_id, item.sub_id, prefix + item.data);
    }
    output_.Set(ctx, std::move(outputs));
    return 0;
  }

 private:
  std::shared_mutex config_mutex_;
  std::string prefix_;
  BoundInput<TextBatch> input_;
  BoundOutput<TextBatch> output_;
};

NodeDefinition MakeStarterControlNodeDefinition() {
  NodeDefinition def;
  def.node_type = StarterControlNode::kNodeType;
  def.category = "custom";
  def.description = "Control authoring starter";
  def.config_fields = StarterControlNode::PrefixConfigFields();
  def.validate_config = [](const nlohmann::json& config, const auto&,
                           std::string* diagnostic) {
    std::string prefix;
    return StarterControlNode::ReadPrefix(config, &prefix, diagnostic);
  };
  def.inputs = {RequiredInputPort(StarterControlNode::kInput.name,
                                  StarterControlNode::kInput, "1:1", "preserve",
                                  "request")};
  def.outputs = {OutputPort(StarterControlNode::kOutput.name,
                            StarterControlNode::kOutput, "1:1", "preserve",
                            "request")};
  def.control_commands = {StarterControlNode::PrefixCommand()};
  // Review any changes to shared state before enabling parallel scheduling.
  def.parallel_safe = false;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(StarterControlNode,
                              MakeStarterControlNodeDefinition());

}  // namespace
}  // namespace llm_edgeflow::custom_nodes
