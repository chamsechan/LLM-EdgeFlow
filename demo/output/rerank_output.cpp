#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"

namespace alg_demo {
namespace {

// 候选文本取自请求信息的 passages；缺失时只显示候选序号。
void ShowRerankResult(const void* output, const nlohmann::json& request_info,
                      uint64_t* request_id, int32_t* status,
                      nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOperatorRerankOutput*>(output);
  const bool has_passages =
      request_info.contains("passages") && request_info["passages"].is_array();

  PrintDivider();
  if (request_info.contains("query")) {
    std::cout << "  Query Text     : \""
              << request_info["query"].get<std::string>() << "\"" << std::endl;
  }

  *request_id = out.request_id;
  *status = out.status_code;
  if (request_info.contains("query")) {
    (*sample_output)["query"] = request_info["query"];
  }

  nlohmann::json ranked_array = nlohmann::json::array();
  const int count =
      std::min<int32_t>(out.count, COMPANY_OPERATOR_MAX_RERANK_CANDIDATES);
  for (int k = 0; k < count; ++k) {
    const int32_t index = out.sorted_indices[k];
    const bool has_text =
        has_passages && index >= 0 &&
        static_cast<size_t>(index) < request_info["passages"].size();
    std::cout << "  Rank #" << k << " [Score " << std::fixed
              << std::setprecision(4) << out.scores[k] << "] -> ";
    if (has_text) {
      std::cout << request_info["passages"][index].get<std::string>();
    } else {
      std::cout << "passage #" << index;
    }
    std::cout << std::endl;

    nlohmann::json item;
    item["rank"] = k;
    item["score"] = out.scores[k];
    item["passage_index"] = index;
    if (has_text) item["passage_text"] = request_info["passages"][index];
    ranked_array.push_back(item);
  }
  (*sample_output)["ranked_results"] = ranked_array;
}

REGISTER_DEMO_OUTPUT("CompanyOperatorRerankOutput", ShowRerankResult);

}  // namespace
}  // namespace alg_demo
