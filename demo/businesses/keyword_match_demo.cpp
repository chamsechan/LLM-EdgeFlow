#include <iostream>
#include <vector>

#include "company_alg_interface.h"
#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"

namespace alg_demo {

int RunKeywordMatchDemo(const DemoOptions& options) {
  PrintBanner("【业务 2 演示】关注词匹配业务", "Conf: " + options.config_path);

  std::vector<std::string> lines;
  std::string err;
  if (!ReadLinesFromFile(options.dataset_path, &lines, &err)) {
    if (options.allow_fallback_sample) {
      std::cout
          << "[KeywordMatchDemo WARN] Dataset not found, using fallback sample."
          << std::endl;
      lines = {"请帮我联系一下VIP专员，我有一笔大客户加急订单需要优先处理。",
               "今天天气真不错，阳光明媚，我想去公园散散步。"};
    } else {
      std::cerr << "[KeywordMatchDemo ERROR] " << err << std::endl;
      return 4;
    }
  }

  if (lines.empty()) {
    if (options.allow_fallback_sample) {
      lines = {"请帮我联系一下VIP专员，我有一笔大客户加急订单需要优先处理。",
               "今天天气真不错，阳光明媚，我想去公园散散步。"};
    } else {
      std::cerr << "[KeywordMatchDemo ERROR] Dataset file has no valid lines."
                << std::endl;
      return 4;
    }
  }

  std::vector<CompanyString> input_strings(lines.size());
  std::vector<CompanyKeywordInputStruct> inputs;
  inputs.reserve(lines.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    CompanyString_FromCString(&input_strings[i], lines[i].c_str());
    inputs.push_back({static_cast<uint64_t>(20001 + i), &input_strings[i]});
  }

  const char* default_ctrl_json =
      "{\n"
      "  \"categories\": {\n"
      "    \"RISK_COMPLAINT\": [\"投诉\", \"欺诈\", \"假货\", \"黑心商家\"],\n"
      "    \"VIP_SERVICE\": [\"VIP\", \"大客户\", \"加急\", \"专员\"],\n"
      "    \"URGENT_HELP\": [\"报警\", \"救命\", \"紧急\"]\n"
      "  }\n"
      "}";

  std::vector<CompanyKeywordOutputStruct> outputs;
  std::vector<double> latencies;

  int ret = RunPlatformOperator<CompanyKeywordInputStruct,
                                CompanyKeywordOutputStruct>(
      options, "client_channel.keyword_in", "client_channel.keyword_out",
      inputs, &outputs, &latencies,
      llm_edgeflow::platform::ControlCommand::kUpdateRules, default_ctrl_json);
  if (ret != 0) {
    return ret;
  }

  std::cout << "\n>>> 业务 2 执行结果验证 <<<" << std::endl;
  std::vector<DemoSampleResult> sample_results;
  sample_results.reserve(outputs.size());

  for (size_t i = 0; i < outputs.size(); ++i) {
    PrintDivider();
    const char* in_txt =
        (inputs[i].sentence_text && inputs[i].sentence_text->data)
            ? inputs[i].sentence_text->data
            : "";
    const char* out_json =
        (outputs[i].match_result_json && outputs[i].match_result_json->data)
            ? outputs[i].match_result_json->data
            : "";
    std::cout << "  Input #" << i << ": \"" << in_txt << "\"\n"
              << "  Request ID : " << outputs[i].request_id << "\n"
              << "  Is Hit     : "
              << (outputs[i].is_hit ? "YES (命中)" : "NO (未命中)") << "\n"
              << "  JSON Output: " << out_json << std::endl;

    DemoSampleResult sample;
    sample.request_id = outputs[i].request_id;
    sample.status = 0;
    sample.latency_ms = (i < latencies.size()) ? latencies[i] : 0.0;
    sample.output["is_hit"] = (outputs[i].is_hit != 0);
    if (out_json[0] != '\0') {
      auto parsed = nlohmann::json::parse(out_json, nullptr, false);
      if (parsed.is_discarded()) {
        sample.output["match_result_raw"] = out_json;
      } else {
        sample.output["match_result"] = parsed;
      }
    }
    sample_results.push_back(sample);
  }

  ResultWriter writer(options);
  int w_ret = writer.WriteResults(sample_results, 0.0, &err);
  if (w_ret != 0) {
    std::cerr << "[KeywordMatchDemo ERROR] Failed to write results: " << err
              << std::endl;
    return w_ret;
  }

  std::cout << "[KeywordMatchDemo] Completed successfully." << std::endl;
  return 0;
}

REGISTER_DEMO_BUSINESS("keyword_match", "【业务 2 演示】关注词匹配业务",
                       RunKeywordMatchDemo);

}  // namespace alg_demo
