#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

int BuildRerankRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  if (!out || inputs.size() != 1) return 3;

  std::unordered_map<std::string, std::vector<std::string>> sections;
  std::string err;
  if (!ParseTagSections(options.dataset_path, &sections, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[CrossRerankDemo ERROR] " << err << std::endl;
      return 4;
    }
  }

  struct Storage {
    std::string query;
    std::vector<std::string> passages;
    CompanyString query_cs{};
    std::vector<CompanyString> passage_cs;
    CompanyOperatorRerankInput carrier{};
  };
  auto storage = std::make_shared<Storage>();
  if (!sections["QUERY"].empty()) storage->query = sections["QUERY"][0];
  if (!sections["PASSAGE"].empty()) storage->passages = sections["PASSAGE"];
  if (storage->query.empty() || storage->passages.empty()) {
    if (options.allow_fallback_sample) {
      std::cout << "[CrossRerankDemo WARN] Dataset sections missing, using "
                   "fallback sample."
                << std::endl;
      if (storage->query.empty()) storage->query = "怎么办理7天无理由退款？";
      if (storage->passages.empty()) {
        storage->passages = {"条款A: 境外交易加收3%手续费。",
                             "条款B: 售后退款支持7天无理由，原路退回付款账户。",
                             "条款C: 节假日人工客服支持延后一个工作日。"};
      }
    } else {
      std::cerr << "[CrossRerankDemo ERROR] Dataset missing required [QUERY] "
                   "or [PASSAGE] section."
                << std::endl;
      return 4;
    }
  }

  storage->query_cs = {static_cast<int32_t>(storage->query.size()),
                       const_cast<char*>(storage->query.data())};
  const int cand_count = std::min(static_cast<int>(storage->passages.size()),
                                  COMPANY_OPERATOR_MAX_RERANK_CANDIDATES);
  storage->passage_cs.reserve(cand_count);
  storage->carrier.request_id = 80001;
  storage->carrier.service_type = inputs[0].service_type.value_or(0);
  storage->carrier.query_text = &storage->query_cs;
  storage->carrier.candidate_count = cand_count;
  for (int i = 0; i < cand_count; ++i) {
    storage->passage_cs.push_back(
        {static_cast<int32_t>(storage->passages[i].size()),
         const_cast<char*>(storage->passages[i].data())});
    storage->carrier.candidate_passages[i] = &storage->passage_cs.back();
  }

  DemoRequestBatch batch;
  batch.requests.resize(1);
  batch.requests[0]["demo." + inputs[0].type] =
      llm_edgeflow::operator_api::MakeBorrowedOperatorInput(&storage->carrier);
  batch.request_info.push_back(
      {{"query", storage->query}, {"passages", storage->passages}});
  batch.storage = storage;
  *out = std::move(batch);
  return 0;
}

REGISTER_DEMO_INPUT("CompanyOperatorRerankInput", BuildRerankRequests);

}  // namespace
}  // namespace alg_demo
