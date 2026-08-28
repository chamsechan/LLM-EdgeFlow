#include <iostream>

#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/engine_interface.h"
#include "nodes/traceable_unary_inference_node.h"

namespace alg_framework {

/**
 * @brief LLM 推理生成公共算子 (LlmGenerateNode, 调用绑定的 ILlmEngine)
 */
class LlmGenerateNode final
    : public TraceableUnaryInferenceNode<ILlmEngine, std::string, std::string> {
 public:
  inline static constexpr char kNodeType[] = "LlmGenerateNode";

  LlmGenerateNode()
      : TraceableUnaryInferenceNode(kNodeType, "prompt", "text", -4301) {}

 protected:
  bool InitModelNode(const NodeInitContext& init_ctx,
                     const nlohmann::json& config,
                     SessionContext& session_ctx) override {
    TraceableUnaryInferenceNode::InitModelNode(init_ctx, config, session_ctx);
    gen_opt_.temperature = config.value("temperature", 0.7f);
    gen_opt_.max_tokens = config.value("max_tokens", 128);
    gen_opt_.top_p = config.value("top_p", 0.9f);
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
  def.inputs = {RequiredInputPort("prompt",
                                  BlackboardKey<TextBatch>{"", "TextBatch"},
                                  "1:1", "preserve", "request")};
  def.outputs = {OutputPort("text", BlackboardKey<TextBatch>{"", "TextBatch"},
                            false, "1:1", "preserve", "request")};
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, false,
                            "llm_model_v1"},
      ConfigFieldDefinition{"temperature", ConfigValueKind::kNumber, false, 0.7,
                            0.0, 2.0},
      ConfigFieldDefinition{"max_tokens", ConfigValueKind::kInteger, false, 128,
                            1.0, 32768.0},
      ConfigFieldDefinition{"top_p", ConfigValueKind::kNumber, false, 0.9, 0.0,
                            1.0}};
  def.model_capability = "llm";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(LlmGenerateNode, MakeLlmGenerateNodeDefinition());

}  // namespace alg_framework
