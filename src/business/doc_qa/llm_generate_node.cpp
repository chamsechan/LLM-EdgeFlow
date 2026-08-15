#include <iostream>

#include "core/node_base.h"
#include "core/node_registry.h"
#include "engine/engine_interface.h"

namespace alg_framework {

/**
 * @brief LLM 推理生成算子 (调用绑定的 LLM 引擎)
 */
class LlmGenerateNode : public INode {
 public:
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
    auto* prompts = req_ctx->Get<std::vector<TraceableItem<std::string>>>(
        "llm_input_prompts");
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

    req_ctx->Set("generated_llm_answers", std::move(generated_outputs));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "LlmGenerateNode";
    return name;
  }

 private:
  std::shared_ptr<ILlmEngine> engine_;
  ILlmEngine::GenerateOption gen_opt_;
};

REGISTER_NODE(LlmGenerateNode);

}  // namespace alg_framework
