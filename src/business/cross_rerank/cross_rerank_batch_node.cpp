#include <iostream>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"

namespace alg_framework {

class CrossRerankBatchNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    std::string bind_model_id = config.value("bind_model", "rerank_model_v1");
    rerank_engine_ =
        session_ctx->GetModelManager().GetModel<IRerankEngine>(bind_model_id);
    if (!rerank_engine_) {
      std::cerr << "[CrossRerankBatchNode] Failed to get IRerankEngine: "
                << bind_model_id << std::endl;
      return false;
    }
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* pair_items =
        req_ctx->Get<std::vector<TraceableItem<IRerankEngine::PairInput>>>(
            "rerank_pair_items");
    if (!pair_items) {
      req_ctx->SetError(-7201,
                        "CrossRerankBatchNode: Missing rerank_pair_items");
      return -7201;
    }

    std::vector<TraceableItem<float>> scores;
    std::cout << "[CrossRerankBatchNode] Scoring " << pair_items->size()
              << " (query, passage) pairs..." << std::endl;
    int ret = rerank_engine_->ScoreTraceableBatch(*pair_items, &scores);
    if (ret != 0) {
      req_ctx->SetError(ret, "CrossRerankBatchNode: Rerank scoring failed");
      return ret;
    }

    req_ctx->Set("rerank_scored_items", std::move(scores));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "CrossRerankBatchNode";
    return name;
  }

 private:
  std::shared_ptr<IRerankEngine> rerank_engine_;
};

REGISTER_NODE(CrossRerankBatchNode);

}  // namespace alg_framework
