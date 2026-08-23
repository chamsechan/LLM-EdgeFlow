#include <iostream>
#include <vector>

#include "company_alg_interface.h"
#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"

namespace alg_demo {

int RunOcrDocQaDemo(const DemoOptions& options) {
  PrintBanner("【业务 5 演示】智能多模态图文票据问答",
              "Conf: " + options.config_path);

  std::unordered_map<std::string, std::vector<std::string>> sections;
  std::string err;
  if (!ParseTagSections(options.dataset_path, &sections, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[OcrDocQaDemo ERROR] " << err << std::endl;
      return 4;
    }
  }

  std::string img;
  std::string prompt;
  if (!sections["IMAGE"].empty()) img = sections["IMAGE"][0];
  if (!sections["PROMPT"].empty()) prompt = sections["PROMPT"][0];

  if (img.empty() || prompt.empty()) {
    if (options.allow_fallback_sample) {
      std::cout << "[OcrDocQaDemo WARN] Dataset sections missing, using "
                   "fallback sample."
                << std::endl;
      if (img.empty()) img = "./data/invoice_01.jpg";
      if (prompt.empty()) prompt = "提取发票代码、号码与总金额";
    } else {
      std::cerr << "[OcrDocQaDemo ERROR] Dataset missing required [IMAGE] or "
                   "[PROMPT] sections."
                << std::endl;
      return 4;
    }
  }

  std::vector<CompanyOcrDocInputStruct> inputs = {
      {60001, img.c_str(), prompt.c_str()}};
  std::vector<CompanyOcrDocOutputStruct> outputs;
  std::vector<double> latencies;

  int ret =
      RunPlatformOperator<CompanyOcrDocInputStruct, CompanyOcrDocOutputStruct>(
          options, "camera_0.frame", "camera_0.od_out", inputs, &outputs,
          &latencies);
  if (ret != 0) {
    return ret;
  }

  std::cout << "\n>>> 业务 5 执行结果验证 <<<" << std::endl;
  PrintDivider();
  std::cout << "  Request ID     : " << outputs[0].request_id << "\n"
            << "  OCR Box Count  : " << outputs[0].detected_box_count << "\n"
            << "  Extracted JSON : " << outputs[0].extracted_invoice_json
            << std::endl;

  std::vector<DemoSampleResult> sample_results;
  DemoSampleResult sample;
  sample.request_id = outputs[0].request_id;
  sample.status = 0;
  sample.latency_ms = latencies.empty() ? 0.0 : latencies[0];
  sample.output["detected_box_count"] = outputs[0].detected_box_count;
  if (outputs[0].extracted_invoice_json[0] != '\0') {
    auto parsed = nlohmann::json::parse(outputs[0].extracted_invoice_json,
                                        nullptr, false);
    if (parsed.is_discarded()) {
      sample.output["extracted_invoice_raw"] =
          outputs[0].extracted_invoice_json;
    } else {
      sample.output["extracted_invoice"] = parsed;
    }
  }
  sample_results.push_back(sample);

  ResultWriter writer(options);
  int w_ret = writer.WriteResults(sample_results, 0.0, &err);
  if (w_ret != 0) {
    std::cerr << "[OcrDocQaDemo ERROR] Failed to write results: " << err
              << std::endl;
    return w_ret;
  }

  std::cout << "[OcrDocQaDemo] Completed successfully." << std::endl;
  return 0;
}

REGISTER_DEMO_BUSINESS("ocr_doc_qa", "【业务 5 演示】智能多模态图文票据问答",
                       RunOcrDocQaDemo);

}  // namespace alg_demo
