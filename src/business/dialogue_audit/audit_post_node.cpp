#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "business/dialogue_audit/dialogue_audit_dto.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

/**
 * @brief 风控审核后处理与结果打包算子 (Node 6: 组装输出领域 DTO)
 */
class AuditPostNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* req_ids = req_ctx->Get<std::vector<uint64_t>>("raw_request_ids");
    auto* policies =
        req_ctx->Get<std::vector<std::string>>("matched_policy_clauses");
    auto* rerank_scores = req_ctx->Get<std::vector<float>>("rerank_scores");
    auto* verdicts = req_ctx->Get<std::vector<TraceableItem<std::string>>>(
        "generated_verdicts");

    if (!req_ids || !policies || !verdicts) return -8501;

    size_t batch_size = req_ids->size();
    std::vector<DialogueAuditResult> outputs(batch_size);

    for (size_t i = 0; i < batch_size; ++i) {
      outputs[i].request_id = (*req_ids)[i];
      outputs[i].status_code = 0;
      outputs[i].risk_score = (rerank_scores && i < rerank_scores->size())
                                  ? (*rerank_scores)[i]
                                  : 0.0f;
      outputs[i].risk_level = "SAFE";
      outputs[i].matched_policy_clause = (*policies)[i];
    }

    // 从 LLM 输出解析结构化字段
    for (const auto& item : *verdicts) {
      uint32_t r_id = item.req_id;
      if (r_id < batch_size) {
        outputs[r_id].audit_verdict_json = item.data;

        try {
          nlohmann::json j = nlohmann::json::parse(item.data);
          std::string level = j.value("risk_level", "SAFE");
          float score = j.value("risk_score", outputs[r_id].risk_score);
          outputs[r_id].risk_score = score;
          outputs[r_id].risk_level = level;
        } catch (...) {
        }
      }
    }

    std::cout << "[AuditPostNode] Finalized " << batch_size
              << " compliance audit results." << std::endl;
    req_ctx->Set("compliance_audit_outputs", std::move(outputs));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "AuditPostNode";
    return name;
  }
};

REGISTER_NODE(AuditPostNode);

}  // namespace alg_framework
