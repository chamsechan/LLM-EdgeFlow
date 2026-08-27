#include <iostream>
#include <string>
#include <vector>

#include "biz/dialogue_audit/dialogue_audit_contract.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"
#include "nodes/traceable_unary_inference_node.h"

namespace alg_framework {

/**
 * @brief LLM 合规风控大模型判决算子 (Node 5: 使用 Model 3 - LLM 引擎)
 */
class LlmAuditNode final
    : public TraceableUnaryInferenceNode<ILlmEngine, std::string, std::string> {
 public:
  inline static constexpr char kNodeType[] = "LlmAuditNode";

  LlmAuditNode()
      : TraceableUnaryInferenceNode(kNodeType, "audit_llm_v1", kLlmAuditPrompts,
                                    kGeneratedVerdicts, -8401) {}

 protected:
  bool InitModelNode(const nlohmann::json& config,
                     SessionContext& /*session_ctx*/) override {
    gen_opt_.temperature = config.value("temperature", 0.1f);
    gen_opt_.max_tokens = config.value("max_tokens", 256);
    return true;
  }

  int InferBatch(const InputBatch& prompts, OutputBatch* outputs) override {
    std::cout << "[LlmAuditNode] Calling Audit LLM on " << prompts.size()
              << " audit prompts..." << std::endl;
    return engine()->InferTraceableBatch(prompts, gen_opt_, outputs);
  }

 private:
  ILlmEngine::GenerateOption gen_opt_;
};

NodeDefinition MakeLlmAuditNodeDefinition() {
  NodeDefinition def;
  def.node_type = LlmAuditNode::kNodeType;
  def.category = "biz";
  def.description = "LLM dialogue compliance audit node";
  def.inputs = {RequiredInput(kLlmAuditPrompts)};
  def.outputs = {Output(kGeneratedVerdicts)};
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, false,
                            "audit_llm_v1"},
      ConfigFieldDefinition{"temperature", ConfigValueKind::kNumber, false, 0.1,
                            0.0, 2.0},
      ConfigFieldDefinition{"max_tokens", ConfigValueKind::kInteger, false, 256,
                            1.0, 32768.0}};
  def.model_capability = "llm";
  def.model_config_field = "bind_model";
  def.biz_names = {kDialogueAuditBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(LlmAuditNode, MakeLlmAuditNodeDefinition());

}  // namespace alg_framework
