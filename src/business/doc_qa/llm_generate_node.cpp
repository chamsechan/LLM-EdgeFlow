#include <iostream>

#include "business/doc_qa/doc_qa_contract.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "engine/engine_interface.h"

namespace alg_framework {

/**
 * @brief LLM 推理生成算子 (调用绑定的 LLM 引擎)
 */
class LlmGenerateNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "LlmGenerateNode";

  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    std::string bind_model_id = config.value("bind_model", "llm_model_v1");
    engine_ =
        session_ctx->GetModelManager().GetModel<ILlmEngine>(bind_model_id);
    if (!engine_) {
      std::cerr << "[LlmGenerateNode] Failed to get model: " << bind_model_id
                << std::endl;
      return false;
    }

    gen_opt_.temperature = config.value("temperature", 0.7f);
    gen_opt_.max_tokens = config.value("max_tokens", 128);
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* prompts = req_ctx->Get(kLlmInputPrompts);
    if (!prompts) {
      req_ctx->SetError(-4301, "LlmGenerateNode: Missing llm_input_prompts");
      return -4301;
    }

    std::vector<TraceableItem<std::string>> generated_outputs;

    std::cout << "[LlmGenerateNode] Inferring LLM outputs for "
              << prompts->size() << " prompt items..." << std::endl;
    int ret =
        engine_->InferTraceableBatch(*prompts, gen_opt_, &generated_outputs);
    if (ret != 0) {
      req_ctx->SetError(ret, "LlmGenerateNode: LLM inference failed");
      return ret;
    }

    req_ctx->Set(kGeneratedLlmAnswers, std::move(generated_outputs));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = kNodeType;
    return name;
  }

 private:
  std::shared_ptr<ILlmEngine> engine_;
  ILlmEngine::GenerateOption gen_opt_;
};

NodeDefinition MakeLlmGenerateNodeDefinition() {
  NodeDefinition def;
  def.node_type = LlmGenerateNode::kNodeType;
  def.category = "business";
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
  def.business_names = {"doc_qa_embedding_v1", "doc_qa_rerank_v1",
                        "entity_extract_0.6b_v1"};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(LlmGenerateNode, MakeLlmGenerateNodeDefinition());

}  // namespace alg_framework
