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

// 数据集：[DOC] / [QUERY] 标签段，按序号配对。
struct DocStorage {
  std::vector<std::string> docs;
  std::vector<std::string> queries;
  std::vector<CompanyString> doc_strs;
  std::vector<CompanyString> query_strs;
  std::vector<CompanyOperatorDocInput> inputs;
};

int BuildDocRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  std::unordered_map<std::string, std::vector<std::string>> sections;
  std::string err;
  if (!ParseTagSections(options.dataset_path, &sections, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[DocInput ERROR] " << err << std::endl;
      return 4;
    }
  }

  auto docs = sections["DOC"];
  auto queries = sections["QUERY"];
  if (docs.empty() || queries.empty()) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[DocInput ERROR] Dataset missing [DOC] or [QUERY] sections."
                << std::endl;
      return 4;
    }
    std::cout << "[DocInput WARN] Dataset sections missing, using fallback "
                 "sample."
              << std::endl;
    docs = {
        "企业级算法框架设计规范：采用4层分层架构，包含C-"
        "ABI适配层、Pipeline调度层、通用算子池与底层硬件引擎抽象。",
        "客户服务售后政策：支持7天无理由退货与全额退款。若商品存在质量问题，"
        "由平台承担双向运费并提供快速换货。"};
    queries = {"请简述该算法框架的架构设计与核心技术？",
               "商品有瑕疵，我想办理退款退货，售后流程是什么？"};
  }

  auto storage = std::make_shared<DocStorage>();
  storage->docs = std::move(docs);
  storage->queries = std::move(queries);
  const size_t count = std::min(storage->docs.size(), storage->queries.size());
  storage->doc_strs.reserve(count);
  storage->query_strs.reserve(count);
  storage->inputs.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    storage->doc_strs.push_back(BorrowCompanyString(storage->docs[i]));
    storage->query_strs.push_back(BorrowCompanyString(storage->queries[i]));
    storage->inputs.push_back({static_cast<uint64_t>(10001 + i),
                               &storage->doc_strs.back(),
                               &storage->query_strs.back()});
  }

  const std::string key = DemoIoKey(inputs[0]);
  out->requests.assign(count, {});
  out->request_info.assign(count, nlohmann::json::object());
  for (size_t i = 0; i < count; ++i) {
    out->requests[i][key] =
        llm_edgeflow::operator_api::MakeBorrowedOperatorInput(
            &storage->inputs[i]);
    out->request_info[i]["query"] = storage->queries[i];
  }
  out->storage = std::move(storage);
  return 0;
}

REGISTER_DEMO_INPUT("CompanyOperatorDocInput", BuildDocRequests);

}  // namespace
}  // namespace alg_demo
