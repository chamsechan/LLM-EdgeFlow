#include <cmath>
#include <cstdint>
#include <limits>

#include "company_alg_log.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/model_interface.h"
#include "nodes/node_error_codes.h"
#include "nodes/traceable_unary_inference_node.h"

namespace alg_framework {

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
    gen_opt_.temperature = config.value("temperature", 0.7f);
    gen_opt_.max_tokens = config.value("max_tokens", 128);
    gen_opt_.top_k = config.value("top_k", 0);
    gen_opt_.top_p = config.value("top_p", 0.9f);
    gen_opt_.repetition_penalty = config.value("repetition_penalty", 1.0f);
    if (gen_opt_.max_tokens <= 0 || !std::isfinite(gen_opt_.temperature) ||
        gen_opt_.temperature < 0.0f || gen_opt_.temperature > 2.0f ||
        gen_opt_.top_k < 0 || !std::isfinite(gen_opt_.top_p) ||
        gen_opt_.top_p <= 0.0f || gen_opt_.top_p > 1.0f ||
        !std::isfinite(gen_opt_.repetition_penalty) ||
        gen_opt_.repetition_penalty <= 0.0f ||
        gen_opt_.repetition_penalty > 100.0f) {
      return false;
    }
    gen_opt_.stop_words.clear();
    if (config.contains("stop_words")) {
      if (!config["stop_words"].is_array()) return false;
      for (const auto& stop_word : config["stop_words"]) {
        if (!stop_word.is_string() || stop_word.get<std::string>().empty()) {
          return false;
        }
        gen_opt_.stop_words.push_back(stop_word.get<std::string>());
      }
    }
    return true;
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

}  // namespace alg_framework
