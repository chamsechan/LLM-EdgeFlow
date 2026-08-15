#include <cstring>
#include <iostream>

#include "company_alg_interface.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

/**
 * @brief 文档问答后处理与多样本对齐算子
 */
class DocQaPostNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* raw_req_ids = req_ctx->Get<std::vector<uint64_t>>("raw_request_ids");
    auto* intents =
        req_ctx->Get<std::vector<std::string>>("recognized_intents");
    auto* confidences = req_ctx->Get<std::vector<float>>("intent_confidences");
    auto* chunk_counts = req_ctx->Get<std::vector<int>>("chunk_counts_per_req");
    auto* answers = req_ctx->Get<std::vector<TraceableItem<std::string>>>(
        "generated_llm_answers");

    if (!raw_req_ids || !intents || !answers) {
      req_ctx->SetError(-4401, "DocQaPostNode: Missing inputs for aggregation");
      return -4401;
    }

    size_t batch_size = raw_req_ids->size();
    std::vector<CompanyDocOutputStruct> final_outputs(batch_size);

    // 默认初始化
    for (size_t i = 0; i < batch_size; ++i) {
      final_outputs[i].request_id = (*raw_req_ids)[i];
      final_outputs[i].status_code = 0;
      final_outputs[i].chunk_count =
          (i < chunk_counts->size()) ? (*chunk_counts)[i] : 0;
      final_outputs[i].confidence =
          (i < confidences->size()) ? (*confidences)[i] : 0.0f;

      const std::string& intent_str = (*intents)[i];
      strncpy(final_outputs[i].intent_name, intent_str.c_str(),
              sizeof(final_outputs[i].intent_name) - 1);
      final_outputs[i].intent_name[sizeof(final_outputs[i].intent_name) - 1] =
          '\0';
    }

    // 根据 req_id 填充 LLM 生成回答
    for (const auto& ans_item : *answers) {
      uint32_t r_id = ans_item.req_id;
      if (r_id < batch_size) {
        strncpy(final_outputs[r_id].answer_text, ans_item.data.c_str(),
                sizeof(final_outputs[r_id].answer_text) - 1);
        final_outputs[r_id]
            .answer_text[sizeof(final_outputs[r_id].answer_text) - 1] = '\0';
      }
    }

    std::cout << "[DocQaPostNode] Successfully aggregated and aligned "
              << batch_size << " output results." << std::endl;

    req_ctx->Set("final_doc_outputs", std::move(final_outputs));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "DocQaPostNode";
    return name;
  }
};

REGISTER_NODE(DocQaPostNode);

}  // namespace alg_framework
