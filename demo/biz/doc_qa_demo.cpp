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
  std::vector<CompanyString> doc_strs;
  std::vector<CompanyString> query_strs;
  doc_strs.reserve(count);
  query_strs.reserve(count);
  std::vector<CompanyPlatformDocInput> inputs;
  inputs.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    doc_strs.push_back({static_cast<int32_t>(docs[i].size()),
                        const_cast<char*>(docs[i].data())});
    query_strs.push_back({static_cast<int32_t>(queries[i].size()),
                          const_cast<char*>(queries[i].data())});
    inputs.push_back({static_cast<uint64_t>(10001 + i), &doc_strs.back(),
                      &query_strs.back()});
  }

  struct OutputSummary {
    uint64_t request_id = 0;
    std::string intent_name;
    float confidence = 0.0f;
    std::string answer_text;
    int32_t chunk_count = 0;
  };
  std::vector<OutputSummary> output_summaries(count);
  std::vector<double> latencies;

  int ret = RunPlatformOperatorWithExtractor<CompanyPlatformDocInput,
                                             CompanyPlatformDocOutput>(
      options, "rag_channel.doc_in", "rag_channel.doc_out", inputs,
      [&](size_t idx, const CompanyPlatformDocOutput& out) {
        output_summaries[idx].request_id = out.request_id;
        output_summaries[idx].confidence = out.confidence;
        output_summaries[idx].chunk_count = out.chunk_count;
        if (out.intent_name && out.intent_name->data) {
          output_summaries[idx].intent_name.assign(out.intent_name->data,
                                                   out.intent_name->length);
        }
        if (out.answer_text && out.answer_text->data) {
          output_summaries[idx].answer_text.assign(out.answer_text->data,
                                                   out.answer_text->length);
        }
      },
      &latencies);
  if (ret != 0) {
    return ret;
  }

  std::cout << "\n>>> 业务 3 执行结果验证 <<<" << std::endl;
  std::vector<DemoSampleResult> sample_results;
  sample_results.reserve(output_summaries.size());

  for (size_t i = 0; i < output_summaries.size(); ++i) {
    PrintDivider();
    std::cout << "  Result #" << i
              << " | Request ID: " << output_summaries[i].request_id << "\n"
              << "  Chunk Count   : " << output_summaries[i].chunk_count
              << " (1-to-N Sub-items)\n"
              << "  Intent Name   : "
              << (!output_summaries[i].intent_name.empty()
                      ? output_summaries[i].intent_name
                      : "none")
              << " (Conf: " << std::fixed << std::setprecision(2)
              << output_summaries[i].confidence << ")\n"
              << "  LLM Answer    : " << output_summaries[i].answer_text
              << std::endl;

    DemoSampleResult sample;
    sample.request_id = output_summaries[i].request_id;
    sample.status = 0;
    sample.latency_ms = (i < latencies.size()) ? latencies[i] : 0.0;
    sample.output["chunk_count"] = output_summaries[i].chunk_count;
    sample.output["intent_name"] = output_summaries[i].intent_name;
    sample.output["confidence"] = output_summaries[i].confidence;
    sample.output["answer_text"] = output_summaries[i].answer_text;
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

REGISTER_DEMO_BIZ("doc_qa", "【业务 3 演示】智能长文档问答业务", RunDocQaDemo);

}  // namespace alg_demo
