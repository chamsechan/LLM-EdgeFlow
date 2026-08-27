#include <iostream>

#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/engine_interface.h"
#include "nodes/traceable_unary_inference_node.h"

namespace alg_framework {

/**
 * @brief LLM 推理生成公共算子 (调用绑定的 ILlmEngine)
 */
class LlmGenerateNode final
    : public TraceableUnaryInferenceNode<ILlmEngine, std::string, std::string> {
 public:
  inline static constexpr char kNodeType[] = "LlmGenerateNode";

  LlmGenerateNode()
      : TraceableUnaryInferenceNode(kNodeType, "llm_model_v1", kLlmInputPrompts,
                                    kGeneratedLlmAnswers, -4301) {}

 protected:
  bool InitModelNode(const nlohmann::json& config,
                     SessionContext& /*session_ctx*/) override {
    gen_opt_.temperature = config.value("temperature", 0.7f);
    gen_opt_.max_tokens = config.value("max_tokens", 128);
    return true;
  }

  int InferBatch(const InputBatch& prompts, OutputBatch* outputs) override {
    std::cout << "[LlmGenerateNode] Inferring LLM outputs for "
              << prompts.size() << " prompt items..." << std::endl;
    return engine()->InferTraceableBatch(prompts, gen_opt_, outputs);
  }

 private:
  ILlmEngine::GenerateOption gen_opt_;
};

NodeDefinition MakeLlmGenerateNodeDefinition() {
  NodeDefinition def;
  def.node_type = LlmGenerateNode::kNodeType;
  def.category = "common";
  def.description = "LLM generate text inference node";
  def.inputs = {RequiredInput(kLlmInputPrompts)};
  def.outputs = {Output(kGeneratedLlmAnswers)};
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, false,
                            "llm_model_v1"},
      ConfigFieldDefinition{"temperature", ConfigValueKind::kNumber, false, 0.7,
                            0.0, 2.0},
      ConfigFieldDefinition{"max_tokens", ConfigValueKind::kInteger, false, 128,
                            1.0, 32768.0}};
  def.model_capability = "llm";
  def.model_config_field = "bind_model";
  // Empty means this common node is reusable by any compatible business.
  def.biz_names = {};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(LlmGenerateNode, MakeLlmGenerateNodeDefinition());

}  // namespace alg_framework
