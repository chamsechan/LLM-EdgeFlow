#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"

namespace alg_demo {
namespace {

// 数据集：[QUERY] / [PASSAGE] 标签段；一条查询配最多 8 个候选段落。
struct RerankStorage {
  std::string query;
  std::vector<std::string> passages;
  CompanyString query_str{};
  std::vector<CompanyString> passage_strs;
  CompanyOperatorRerankInput input{};
};

int BuildRerankRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  std::unordered_map<std::string, std::vector<std::string>> sections;
  std::string err;
  if (!ParseTagSections(options.dataset_path, &sections, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[RerankInput ERROR] " << err << std::endl;
      return 4;
    }
  }

  std::string query;
  std::vector<std::string> passages;
  if (!sections["QUERY"].empty()) query = sections["QUERY"][0];
  if (!sections["PASSAGE"].empty()) passages = sections["PASSAGE"];

  if (query.empty() || passages.empty()) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[RerankInput ERROR] Dataset missing required [QUERY] or "
                   "[PASSAGE] section."
                << std::endl;
      return 4;
    }
    std::cout << "[RerankInput WARN] Dataset sections missing, using fallback "
                 "sample."
              << std::endl;
    if (query.empty()) query = "怎么办理7天无理由退款？";
    if (passages.empty()) {
      passages = {"条款A: 境外交易加收3%手续费。",
                  "条款B: 售后退款支持7天无理由，原路退回付款账户。",
                  "条款C: 节假日人工客服支持延后一个工作日。"};
    }
  }

  auto storage = std::make_shared<RerankStorage>();
  storage->query = std::move(query);
  storage->passages = std::move(passages);
  const size_t candidate_count = std::min<size_t>(
      storage->passages.size(), COMPANY_OPERATOR_MAX_RERANK_CANDIDATES);
  storage->passages.resize(candidate_count);
  storage->query_str = BorrowCompanyString(storage->query);
  storage->passage_strs.reserve(candidate_count);

  storage->input.request_id = 80001;
  storage->input.query_text = &storage->query_str;
  storage->input.candidate_count = static_cast<int32_t>(candidate_count);
  for (size_t i = 0; i < candidate_count; ++i) {
    storage->passage_strs.push_back(BorrowCompanyString(storage->passages[i]));
    storage->input.candidate_passages[i] = &storage->passage_strs.back();
  }

  out->requests.assign(1, {});
  out->requests[0][DemoIoKey(inputs[0])] =
      llm_edgeflow::operator_api::MakeBorrowedOperatorInput(&storage->input);
  out->request_info.assign(1, nlohmann::json::object());
  out->request_info[0]["query"] = storage->query;
  out->request_info[0]["passages"] = storage->passages;
  out->storage = std::move(storage);
  return 0;
}

REGISTER_DEMO_INPUT("CompanyOperatorRerankInput", BuildRerankRequests);

}  // namespace
}  // namespace alg_demo
