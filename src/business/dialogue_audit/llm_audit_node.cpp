#include <iostream>
#include <string>
#include <vector>

#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"

namespace alg_framework {

/**
 * @brief LLM 合规风控大模型判决算子 (Node 5: 使用 Model 3 - LLM 引擎)
 */
class LlmAuditNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    std::string bind_model = config.value("bind_model", "audit_llm_v1");
    llm_engine_ =
        session_ctx->GetModelManager().GetModel<ILlmEngine>(bind_model);
    if (!llm_engine_) {
      std::cerr << "[LlmAuditNode] Failed to bind model: " << bind_model
                << std::endl;
      return false;
    }
    gen_opt_.temperature = config.value("temperature", 0.1f);
    gen_opt_.max_tokens = config.value("max_tokens", 256);
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* prompts = req_ctx->Get<std::vector<TraceableItem<std::string>>>(
        "llm_audit_prompts");
    if (!prompts) return -8401;

    std::vector<TraceableItem<std::string>> generated_verdicts;
    std::cout << "[LlmAuditNode] Calling Audit LLM on " << prompts->size()
              << " audit prompts..." << std::endl;
    int ret = llm_engine_->InferTraceableBatch(*prompts, gen_opt_,
                                               &generated_verdicts);
    if (ret != 0) return ret;

    req_ctx->Set("generated_verdicts", std::move(generated_verdicts));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "LlmAuditNode";
    return name;
  }

 private:
  std::shared_ptr<ILlmEngine> llm_engine_;
  ILlmEngine::GenerateOption gen_opt_;
};

REGISTER_NODE(LlmAuditNode);

}  // namespace alg_framework
