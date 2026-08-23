#include <iomanip>
#include <iostream>
#include <vector>

#include "company_alg_interface.h"
#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"

namespace alg_demo {

int RunCrossRerankDemo(const DemoOptions& options) {
  PrintBanner("【业务 7 演示】纯语义精排打分业务",
              "Conf: " + options.config_path);

  std::unordered_map<std::string, std::vector<std::string>> sections;
  std::string err;
  if (!ParseTagSections(options.dataset_path, &sections, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[CrossRerankDemo ERROR] " << err << std::endl;
      return 4;
    }
  }

  std::string query = "怎么办理7天无理由退款？";
  std::vector<std::string> passages = {
      "条款A: 境外交易加收3%手续费。",
      "条款B: 售后退款支持7天无理由，原路退回付款账户。",
      "条款C: 节假日人工客服支持延后一个工作日。"};

  if (!sections["QUERY"].empty()) query = sections["QUERY"][0];
  if (!sections["PASSAGE"].empty()) passages = sections["PASSAGE"];

  if (passages.empty()) {
    if (options.allow_fallback_sample) {
      passages = {"条款A: 境外交易加收3%手续费。",
                  "条款B: 售后退款支持7天无理由，原路退回付款账户。",
                  "条款C: 节假日人工客服支持延后一个工作日。"};
    } else {
      std::cerr << "[CrossRerankDemo ERROR] Dataset missing [PASSAGE] section."
                << std::endl;
      return 4;
    }
  }

  CompanyRerankBatchInputStruct req{};
  req.request_id = 80001;
  req.query_text = query.c_str();
  req.candidate_count = std::min(static_cast<int>(passages.size()), 8);
  for (int i = 0; i < req.candidate_count; ++i) {
    req.candidate_passages[i] = passages[i].c_str();
  }

  std::vector<CompanyRerankBatchInputStruct> inputs = {req};
  std::vector<CompanyRerankBatchOutputStruct> outputs;
  std::vector<double> latencies;

  int ret = RunPlatformOperator<CompanyRerankBatchInputStruct,
                                CompanyRerankBatchOutputStruct>(
      options, "ranker.rerank_in", "ranker.rerank_out", inputs, &outputs,
      &latencies);
  if (ret != 0) {
    return ret;
  }

  std::cout << "\n>>> 业务 7 执行结果验证 <<<" << std::endl;
  PrintDivider();
  std::cout << "  Query Text     : \"" << query << "\"" << std::endl;

  std::vector<DemoSampleResult> sample_results;
  DemoSampleResult sample;
  sample.request_id = outputs[0].request_id;
  sample.status = 0;
  sample.latency_ms = latencies.empty() ? 0.0 : latencies[0];
  sample.output["query"] = query;

  nlohmann::json ranked_array = nlohmann::json::array();
  for (int k = 0; k < outputs[0].count; ++k) {
    int orig_idx = outputs[0].sorted_indices[k];
    std::cout << "  Rank #" << k << " [Score " << std::fixed
              << std::setprecision(4) << outputs[0].scores[k] << "] -> "
              << passages[orig_idx] << std::endl;

    nlohmann::json item;
    item["rank"] = k;
    item["score"] = outputs[0].scores[k];
    item["passage_index"] = orig_idx;
    item["passage_text"] = passages[orig_idx];
    ranked_array.push_back(item);
  }
  sample.output["ranked_results"] = ranked_array;
  sample_results.push_back(sample);

  ResultWriter writer(options);
  int w_ret = writer.WriteResults(sample_results, 0.0, &err);
  if (w_ret != 0) {
    std::cerr << "[CrossRerankDemo ERROR] Failed to write results: " << err
              << std::endl;
    return w_ret;
  }

  std::cout << "[CrossRerankDemo] Completed successfully." << std::endl;
  return 0;
}

REGISTER_DEMO_BUSINESS("cross_rerank", "【业务 7 演示】纯语义精排打分业务",
                       RunCrossRerankDemo);

}  // namespace alg_demo
