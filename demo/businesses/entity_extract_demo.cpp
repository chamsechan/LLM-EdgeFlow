#include <iostream>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"
#include "platform/company_platform_types.h"

namespace alg_demo {

int RunEntityExtractDemo(const DemoOptions& options) {
  PrintBanner("【业务 1 演示】实体/名词提取业务",
              "Conf: " + options.config_path);

  std::vector<std::string> lines;
  std::string err;
  if (!ReadLinesFromFile(options.dataset_path, &lines, &err)) {
    if (options.allow_fallback_sample) {
      std::cout << "[EntityExtractDemo WARN] Dataset not found, using fallback "
                   "sample."
                << std::endl;
      lines = {
          "张三在清华大学毕业后加入了一家北京的人工智能公司，作为算法工程师负责"
          "NPU芯片和深度学习大模型的研发项目。"};
    } else {
      std::cerr << "[EntityExtractDemo ERROR] " << err << std::endl;
      return 4;
    }
  }

  if (lines.empty()) {
    if (options.allow_fallback_sample) {
      lines = {
          "张三在清华大学毕业后加入了一家北京的人工智能公司，作为算法工程师负责"
          "NPU芯片和深度学习大模型的研发项目。"};
    } else {
      std::cerr << "[EntityExtractDemo ERROR] Dataset file has no valid lines."
                << std::endl;
      return 4;
    }
  }

  std::vector<CompanyString> text_strs;
  text_strs.reserve(lines.size());
  std::vector<CompanyPlatformEntityInput> inputs;
  inputs.reserve(lines.size());

  for (size_t i = 0; i < lines.size(); ++i) {
    text_strs.push_back({static_cast<int32_t>(lines[i].size()),
                         const_cast<char*>(lines[i].data())});
    inputs.push_back({static_cast<uint64_t>(30001 + i), &text_strs.back()});
  }

  struct OutputSummary {
    uint64_t request_id = 0;
    std::string entities_json;
  };
  std::vector<OutputSummary> output_summaries(lines.size());
  std::vector<double> latencies;

  int ret = RunPlatformOperatorWithExtractor<CompanyPlatformEntityInput,
                                             CompanyPlatformEntityOutput>(
      options, "nlp_node.entity_in", "nlp_node.entity_out", inputs,
      [&](size_t idx, const CompanyPlatformEntityOutput& out) {
        output_summaries[idx].request_id = out.request_id;
        if (out.entities_json && out.entities_json->data) {
          output_summaries[idx].entities_json.assign(out.entities_json->data,
                                                     out.entities_json->length);
        }
      },
      &latencies);
  if (ret != 0) {
    return ret;
  }

  std::cout << "\n>>> 业务 1 执行结果验证 <<<" << std::endl;
  std::vector<DemoSampleResult> sample_results;
  sample_results.reserve(output_summaries.size());

  for (size_t i = 0; i < output_summaries.size(); ++i) {
    PrintDivider();
    std::cout << "  Input Sentence : \"" << lines[i] << "\"\n"
              << "  Request ID     : " << output_summaries[i].request_id << "\n"
              << "  Extracted JSON : " << output_summaries[i].entities_json
              << std::endl;

    DemoSampleResult sample;
    sample.request_id = output_summaries[i].request_id;
    sample.status = 0;
    sample.latency_ms = (i < latencies.size()) ? latencies[i] : 0.0;
    if (!output_summaries[i].entities_json.empty()) {
      auto parsed = nlohmann::json::parse(output_summaries[i].entities_json,
                                          nullptr, false);
      if (parsed.is_discarded()) {
        sample.output["entities_raw"] = output_summaries[i].entities_json;
      } else {
        sample.output["entities"] = parsed;
      }
    }
    sample_results.push_back(sample);
  }

  ResultWriter writer(options);
  int w_ret = writer.WriteResults(sample_results, 0.0, &err);
  if (w_ret != 0) {
    std::cerr << "[EntityExtractDemo ERROR] Failed to write results: " << err
              << std::endl;
    return w_ret;
  }

  std::cout << "[EntityExtractDemo] Completed successfully." << std::endl;
  return 0;
}

REGISTER_DEMO_BUSINESS("entity_extract", "【业务 1 演示】实体/名词提取业务",
                       RunEntityExtractDemo);

}  // namespace alg_demo
