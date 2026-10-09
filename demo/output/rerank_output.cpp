#include <algorithm>

#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

void ShowRerankResult(const void* output, const nlohmann::json& request_info,
                      uint64_t* request_id, int32_t* status,
                      nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOperatorRerankOutput*>(output);
  *request_id = out.request_id;
  *status = out.status_code;
  const auto query = request_info.find("query");
  if (query != request_info.end() && query->is_string()) {
    (*sample_output)["query"] = *query;
  }
  const auto passages = request_info.find("passages");
  nlohmann::json ranked_array = nlohmann::json::array();
  const int count = std::min(out.count, COMPANY_OPERATOR_MAX_RERANK_CANDIDATES);
  for (int k = 0; k < count; ++k) {
    const int orig_idx = out.sorted_indices[k];
    nlohmann::json item;
    item["rank"] = k;
    item["score"] = out.scores[k];
    item["passage_index"] = orig_idx;
    if (passages != request_info.end() && passages->is_array() &&
        orig_idx >= 0 && static_cast<size_t>(orig_idx) < passages->size() &&
        (*passages)[orig_idx].is_string()) {
      item["passage_text"] = (*passages)[orig_idx];
    }
    ranked_array.push_back(item);
  }
  (*sample_output)["ranked_results"] = ranked_array;
}

REGISTER_DEMO_OUTPUT("CompanyOperatorRerankOutput", ShowRerankResult);

}  // namespace
}  // namespace alg_demo
