#include <iostream>
#include <vector>

#include "company_alg_interface.h"
#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"

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

  std::vector<CompanyEntityInputStruct> inputs;
  inputs.reserve(lines.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    inputs.push_back({static_cast<uint64_t>(30001 + i), lines[i].c_str()});
  }

  std::vector<CompanyEntityOutputStruct> outputs;
  std::vector<double> latencies;

  int ret =
      RunPlatformOperator<CompanyEntityInputStruct, CompanyEntityOutputStruct>(
          options, "nlp_node.entity_in", "nlp_node.entity_out", inputs,
          &outputs, &latencies);
  if (ret != 0) {
    return ret;
  }

  std::cout << "\n>>> 业务 1 执行结果验证 <<<" << std::endl;
  std::vector<DemoSampleResult> sample_results;
  sample_results.reserve(outputs.size());

  for (size_t i = 0; i < outputs.size(); ++i) {
    PrintDivider();
    std::cout << "  Input Sentence : \"" << inputs[i].sentence_text << "\"\n"
              << "  Request ID     : " << outputs[i].request_id << "\n"
              << "  Extracted JSON : " << outputs[i].entities_json << std::endl;

    DemoSampleResult sample;
    sample.request_id = outputs[i].request_id;
    sample.status = 0;
    sample.latency_ms = (i < latencies.size()) ? latencies[i] : 0.0;
    if (outputs[i].entities_json[0] != '\0') {
      auto parsed =
          nlohmann::json::parse(outputs[i].entities_json, nullptr, false);
      if (parsed.is_discarded()) {
        sample.output["entities_raw"] = outputs[i].entities_json;
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
