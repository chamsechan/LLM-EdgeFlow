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

int BuildDocRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  if (!out || inputs.size() != 1) return 3;

  std::unordered_map<std::string, std::vector<std::string>> sections;
  std::string err;
  if (!ParseTagSections(options.dataset_path, &sections, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[DocQaDemo ERROR] " << err << std::endl;
      return 4;
    }
  }

  struct Storage {
    std::vector<std::string> docs;
    std::vector<std::string> queries;
    std::vector<CompanyString> doc_strs;
    std::vector<CompanyString> query_strs;
    std::vector<CompanyOperatorDocInput> carriers;
  };
  auto storage = std::make_shared<Storage>();
  storage->docs = sections["DOC"];
  storage->queries = sections["QUERY"];
  if (storage->docs.empty() || storage->queries.empty()) {
    if (options.allow_fallback_sample) {
      std::cout
          << "[DocQaDemo WARN] Dataset sections missing, using fallback sample."
          << std::endl;
      storage->docs = {
          "企业级算法框架设计规范：采用4层分层架构，包含C-"
          "ABI适配层、Pipeline调度层、通用算子池与底层硬件引擎抽象。",
          "客户服务售后政策：支持7天无理由退货与全额退款。若商品存在质量问题，"
          "由平台承担双向运费并提供快速换货。"};
      storage->queries = {"请简述该算法框架的架构设计与核心技术？",
                          "商品有瑕疵，我想办理退款退货，售后流程是什么？"};
    } else {
      std::cerr
          << "[DocQaDemo ERROR] Dataset missing [DOC] or [QUERY] sections."
          << std::endl;
      return 4;
    }
  }

  const size_t count = std::min(storage->docs.size(), storage->queries.size());
  storage->doc_strs.reserve(count);
  storage->query_strs.reserve(count);
  storage->carriers.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    storage->doc_strs.push_back({static_cast<int32_t>(storage->docs[i].size()),
                                 const_cast<char*>(storage->docs[i].data())});
    storage->query_strs.push_back(
        {static_cast<int32_t>(storage->queries[i].size()),
         const_cast<char*>(storage->queries[i].data())});
    storage->carriers.push_back({static_cast<uint64_t>(10001 + i),
                                 &storage->doc_strs.back(),
                                 &storage->query_strs.back()});
  }

  DemoRequestBatch batch;
  batch.requests.resize(count);
  for (size_t i = 0; i < count; ++i) {
    batch.requests[i]["demo." + inputs[0].type] =
        llm_edgeflow::operator_api::MakeBorrowedOperatorInput(
            &storage->carriers[i]);
  }
  batch.storage = storage;
  *out = std::move(batch);
  return 0;
}

REGISTER_DEMO_INPUT("CompanyOperatorDocInput", BuildDocRequests);

}  // namespace
}  // namespace alg_demo
