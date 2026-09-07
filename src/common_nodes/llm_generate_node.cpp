#include <cmath>
#include <cstdint>
#include <limits>

#include "company_alg_log.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/model_interface.h"
#include "nodes/node_error_codes.h"
#include "nodes/traceable_unary_inference_node.h"

namespace llm_edgeflow {
namespace {

bool ParseGenerateOptions(const nlohmann::json& config,
                          GenerateOptions* options, std::string* diagnostic) {
  auto reject = [&](const std::string& message) {
    if (diagnostic) *diagnostic = message;
    return false;
  };
  try {
    const auto max_tokens = config.value("max_tokens", nlohmann::json(128));
    const auto top_k = config.value("top_k", nlohmann::json(0));
    if (!max_tokens.is_number_integer() || max_tokens < 1 ||
        max_tokens > 32768 || !top_k.is_number_integer() || top_k < 0 ||
        top_k > std::numeric_limits<int32_t>::max()) {
      return reject(
          "max_tokens or top_k is outside the supported integer range");
    }
    const double temperature = config.value("temperature", 0.7);
    const double top_p = config.value("top_p", 0.9);
    const double repetition_penalty = config.value("repetition_penalty", 1.0);
    if (!std::isfinite(temperature) || temperature < 0 || temperature > 2 ||
        !std::isfinite(top_p) || top_p < 1.0e-9 || top_p > 1 ||
        !std::isfinite(repetition_penalty) || repetition_penalty < 1.0e-9 ||
        repetition_penalty > 100) {
      return reject("Generation options are outside the supported range");
    }
    GenerateOptions parsed;
    parsed.max_tokens = max_tokens.get<int>();
    parsed.top_k = top_k.get<int>();
    parsed.temperature = static_cast<float>(temperature);
    parsed.top_p = static_cast<float>(top_p);
    parsed.repetition_penalty = static_cast<float>(repetition_penalty);
    if (config.contains("stop_words")) {
      if (!config["stop_words"].is_array())
        return reject("stop_words must be an array");
      for (const auto& word : config["stop_words"]) {
        if (!word.is_string() || word.get_ref<const std::string&>().empty()) {
          return reject("stop_words must contain non-empty strings");
        }
        parsed.stop_words.push_back(word.get<std::string>());
      }
    }
    if (options) *options = std::move(parsed);
    return true;
  } catch (const std::exception& error) {
    return reject(error.what());
  }
}

}  // namespace

/**
 * @brief LLM 推理生成公共算子 (LlmGenerateNode, 调用绑定的 ILlmModel)
 */
class LlmGenerateNode final
    : public TraceableUnaryInferenceNode<ILlmModel, std::string, std::string> {
 public:
  inline static constexpr char kNodeType[] = "LlmGenerateNode";

  LlmGenerateNode()
      : TraceableUnaryInferenceNode(
            kNodeType, "prompt", "text",
            node_error::llm_generate::kMissingInput,
            node_error::llm_generate::kOutputCountMismatch,
            node_error::llm_generate::kOutputProvenanceMismatch) {}

 protected:
  bool InitModelNode(const NodeInitContext& init_ctx,
                     const nlohmann::json& config,
                     SessionContext& session_ctx) override {
    if (!TraceableUnaryInferenceNode::InitModelNode(init_ctx, config,
                                                    session_ctx)) {
      return false;
    }
    return ParseGenerateOptions(config, &gen_opt_, nullptr);
  }

  int InferBatch(const InputBatch& prompts, OutputBatch* outputs) override {
    ALG_LOG_DEBUG(
        "[LlmGenerateNode] Inferring LLM outputs for %zu prompt "
        "items...\n",
        prompts.size());
    return model()->Generate(prompts, gen_opt_, outputs);
  }

 private:
  GenerateOptions gen_opt_;
};

NodeDefinition MakeLlmGenerateNodeDefinition() {
  NodeDefinition def;
  def.node_type = LlmGenerateNode::kNodeType;
  def.category = "common";
  def.validate_config = [](const nlohmann::json& config, const auto&,
                           std::string* diagnostic) {
    return ParseGenerateOptions(config, nullptr, diagnostic);
  };
  def.description = "LLM generate text inference node";
  def.inputs = {RequiredInputPort("prompt",
                                  BlackboardKey<TextBatch>{"", "TextBatch"},
                                  "1:1", "preserve", "request")};
  def.outputs = {OutputPort("text", BlackboardKey<TextBatch>{"", "TextBatch"},
                            "1:1", "preserve", "request")};
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, false,
                            "llm_model_v1"},
      ConfigFieldDefinition{"temperature", ConfigValueKind::kNumber, false, 0.7,
                            0.0, 2.0},
      ConfigFieldDefinition{"max_tokens", ConfigValueKind::kInteger, false, 128,
                            1.0, 32768.0},
      ConfigFieldDefinition{
          "top_k", ConfigValueKind::kInteger, false, 0, 0.0,
          static_cast<double>(std::numeric_limits<int32_t>::max())},
      ConfigFieldDefinition{"top_p", ConfigValueKind::kNumber, false, 0.9,
                            1.0e-9, 1.0},
      ConfigFieldDefinition{"repetition_penalty", ConfigValueKind::kNumber,
                            false, 1.0, 1.0e-9, 100.0},
      ConfigFieldDefinition{"stop_words", ConfigValueKind::kArray, false,
                            nlohmann::json::array()}};
  def.model_capability = "llm";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(LlmGenerateNode, MakeLlmGenerateNodeDefinition());

}  // namespace llm_edgeflow
