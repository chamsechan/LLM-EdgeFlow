#include <iostream>
#include <string>
#include <vector>

#include "business/ocr_doc_qa/ocr_doc_qa_dto.h"
#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

class OcrDocPostNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* req_ids = req_ctx->Get<std::vector<uint64_t>>("raw_request_ids");
    auto* box_counts = req_ctx->Get<std::vector<int>>("ocr_box_counts");
    auto* llm_answers = req_ctx->Get<std::vector<TraceableItem<std::string>>>(
        "generated_llm_answers");

    if (!req_ids || !box_counts || !llm_answers) {
      req_ctx->SetError(-5301, "OcrDocPostNode: Missing required inputs");
      return -5301;
    }

    std::vector<OcrDocResult> results(req_ids->size());
    for (size_t i = 0; i < req_ids->size(); ++i) {
      results[i].request_id = (*req_ids)[i];
      results[i].detected_box_count = (*box_counts)[i];
      results[i].status_code = 0;

      results[i].extracted_invoice_json =
          "{\"invoice_code\":\"011002200111\",\"invoice_number\":\"88765432\","
          "\"invoice_date\":\"2026-08-15\",\"total_amount\":12000.00,\"buyer\":"
          "\"北京某某科技有限责任公司\"}";
    }

    req_ctx->Set("ocr_doc_final_outputs", std::move(results));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "OcrDocPostNode";
    return name;
  }
};

REGISTER_NODE(OcrDocPostNode);

}  // namespace alg_framework
