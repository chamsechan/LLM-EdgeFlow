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

int RunDocQaDemo(const DemoOptions& options) {
  PrintBanner("【业务 3 演示】智能长文档问答业务",
              "Conf: " + options.config_path);

  std::unordered_map<std::string, std::vector<std::string>> sections;
  std::string err;
  if (!ParseTagSections(options.dataset_path, &sections, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[DocQaDemo ERROR] " << err << std::endl;
      return 4;
    }
  }

  auto docs = sections["DOC"];
  auto queries = sections["QUERY"];

  if (docs.empty() || queries.empty()) {
    if (options.allow_fallback_sample) {
      std::cout
          << "[DocQaDemo WARN] Dataset sections missing, using fallback sample."
          << std::endl;
      docs = {
          "企业级算法框架设计规范：采用4层分层架构，包含C-"
          "ABI适配层、Pipeline调度层、通用算子池与底层硬件引擎抽象。",
          "客户服务售后政策：支持7天无理由退货与全额退款。若商品存在质量问题，"
          "由平台承担双向运费并提供快速换货。"};
      queries = {"请简述该算法框架的架构设计与核心技术？",
                 "商品有瑕疵，我想办理退款退货，售后流程是什么？"};
    } else {
      std::cerr
          << "[DocQaDemo ERROR] Dataset missing [DOC] or [QUERY] sections."
          << std::endl;
      return 4;
    }
  }

  size_t count = std::min(docs.size(), queries.size());
  std::vector<CompanyString> doc_strings(count);
  std::vector<CompanyString> query_strings(count);
  std::vector<CompanyDocInputStruct> inputs;
  inputs.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    CompanyString_FromCString(&doc_strings[i], docs[i].c_str());
    CompanyString_FromCString(&query_strings[i], queries[i].c_str());
    inputs.push_back(
        {static_cast<uint64_t>(10001 + i), &doc_strings[i], &query_strings[i]});
  }

  std::vector<CompanyDocOutputStruct> outputs;
  std::vector<double> latencies;

  int ret = RunPlatformOperator<CompanyDocInputStruct, CompanyDocOutputStruct>(
      options, "rag_channel.doc_in", "rag_channel.doc_out", inputs, &outputs,
      &latencies);
  if (ret != 0) {
    return ret;
  }

  std::cout << "\n>>> 业务 3 执行结果验证 <<<" << std::endl;
  std::vector<DemoSampleResult> sample_results;
  sample_results.reserve(outputs.size());

  for (size_t i = 0; i < outputs.size(); ++i) {
    PrintDivider();
    const char* intent_str =
        (outputs[i].intent_name && outputs[i].intent_name->data)
            ? outputs[i].intent_name->data
            : "";
    const char* ans_str =
        (outputs[i].answer_text && outputs[i].answer_text->data)
            ? outputs[i].answer_text->data
            : "";
    std::cout << "  Result #" << i << " | Request ID: " << outputs[i].request_id
              << "\n"
              << "  Chunk Count   : " << outputs[i].chunk_count
              << " (1-to-N Sub-items)\n"
              << "  Intent Name   : "
              << (intent_str[0] != '\0' ? intent_str : "none")
              << " (Conf: " << std::fixed << std::setprecision(2)
              << outputs[i].confidence << ")\n"
              << "  LLM Answer    : " << ans_str << std::endl;

    DemoSampleResult sample;
    sample.request_id = outputs[i].request_id;
    sample.status = 0;
    sample.latency_ms = (i < latencies.size()) ? latencies[i] : 0.0;
    sample.output["chunk_count"] = outputs[i].chunk_count;
    sample.output["intent_name"] = intent_str;
    sample.output["confidence"] = outputs[i].confidence;
    sample.output["answer_text"] = ans_str;
    sample_results.push_back(sample);
  }

  ResultWriter writer(options);
  int w_ret = writer.WriteResults(sample_results, 0.0, &err);
  if (w_ret != 0) {
    std::cerr << "[DocQaDemo ERROR] Failed to write results: " << err
              << std::endl;
    return w_ret;
  }

  std::cout << "[DocQaDemo] Completed successfully." << std::endl;
  return 0;
}

REGISTER_DEMO_BUSINESS("doc_qa", "【业务 3 演示】智能长文档问答业务",
                       RunDocQaDemo);

}  // namespace alg_demo
