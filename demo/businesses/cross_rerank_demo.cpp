#include <iomanip>
#include <iostream>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"
#include "platform/company_platform_types.h"

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

  std::string query;
  std::vector<std::string> passages;

  if (!sections["QUERY"].empty()) query = sections["QUERY"][0];
  if (!sections["PASSAGE"].empty()) passages = sections["PASSAGE"];

  if (query.empty() || passages.empty()) {
    if (options.allow_fallback_sample) {
      std::cout << "[CrossRerankDemo WARN] Dataset sections missing, using "
                   "fallback sample."
                << std::endl;
      if (query.empty()) query = "怎么办理7天无理由退款？";
      if (passages.empty()) {
        passages = {"条款A: 境外交易加收3%手续费。",
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

  CompanyString query_cs{static_cast<int32_t>(query.size()),
                         const_cast<char*>(query.data())};
  std::vector<CompanyString> passage_cs;
  int cand_count = std::min(static_cast<int>(passages.size()), 8);
  passage_cs.reserve(cand_count);

  CompanyPlatformRerankInput req{};
  req.request_id = 80001;
  req.query_text = &query_cs;
  req.candidate_count = cand_count;
  for (int i = 0; i < cand_count; ++i) {
    passage_cs.push_back({static_cast<int32_t>(passages[i].size()),
                          const_cast<char*>(passages[i].data())});
    req.candidate_passages[i] = &passage_cs.back();
  }

  std::vector<CompanyPlatformRerankInput> inputs = {req};
  struct OutputSummary {
    uint64_t request_id = 0;
    int32_t count = 0;
    float scores[8] = {0};
    int32_t sorted_indices[8] = {0};
  };
  std::vector<OutputSummary> output_summaries(1);
  std::vector<double> latencies;

  int ret = RunPlatformOperatorWithExtractor<CompanyPlatformRerankInput,
                                             CompanyPlatformRerankOutput>(
      options, "ranker.rerank_in", "ranker.rerank_out", inputs,
      [&](size_t idx, const CompanyPlatformRerankOutput& out) {
        output_summaries[idx].request_id = out.request_id;
        output_summaries[idx].count = out.count;
        for (int i = 0; i < out.count && i < 8; ++i) {
          output_summaries[idx].scores[i] = out.scores[i];
          output_summaries[idx].sorted_indices[i] = out.sorted_indices[i];
        }
      },
      &latencies);
  if (ret != 0) {
    return ret;
  }

  std::cout << "\n>>> 业务 7 执行结果验证 <<<" << std::endl;
  PrintDivider();
  std::cout << "  Query Text     : \"" << query << "\"" << std::endl;

  std::vector<DemoSampleResult> sample_results;
  DemoSampleResult sample;
  sample.request_id = output_summaries[0].request_id;
  sample.status = 0;
  sample.latency_ms = latencies.empty() ? 0.0 : latencies[0];
  sample.output["query"] = query;

  nlohmann::json ranked_array = nlohmann::json::array();
  for (int k = 0; k < output_summaries[0].count; ++k) {
    int orig_idx = output_summaries[0].sorted_indices[k];
    std::cout << "  Rank #" << k << " [Score " << std::fixed
              << std::setprecision(4) << output_summaries[0].scores[k]
              << "] -> " << passages[orig_idx] << std::endl;

    nlohmann::json item;
    item["rank"] = k;
    item["score"] = output_summaries[0].scores[k];
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
