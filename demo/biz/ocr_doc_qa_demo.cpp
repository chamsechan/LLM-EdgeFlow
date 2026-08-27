#include <iostream>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"
#include "platform/company_platform_types.h"

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

  if (!ValidateConfigBusinessMatch(options.config_path, options.business,
                                   &err)) {
    std::cerr << "[OcrDocQaDemo ERROR] Config validation failed: " << err
              << std::endl;
    return 3;
  }

  llm_edgeflow::platform::ChipType chip_type =
      llm_edgeflow::platform::ChipType::kUnknown;
  if (!ParseChipType(options.chip, &chip_type)) {
    std::cerr << "[OcrDocQaDemo ERROR] Unsupported chip: " << options.chip
              << std::endl;
    return 3;
  }

  std::string model_root;
  std::string cfg_rel;
  ResolveModelRootAndConfig(options.config_path, &model_root, &cfg_rel);

  auto ops = llm_edgeflow::platform::Get_LLM_EDGEFLOW_OperatorTable();

  int max_batch_size = options.batch_size > 0 ? options.batch_size : 1;
  uint32_t requested_depth = options.depth_num > 0 ? options.depth_num : 25;
  if (requested_depth < static_cast<uint32_t>(max_batch_size)) {
    requested_depth = static_cast<uint32_t>(max_batch_size);
  }

  llm_edgeflow::platform::CreateParam param{};
  param.model_path = model_root.c_str();
  param.cfg_file_name = cfg_rel.c_str();
  param.device_id = options.device_id;
  param.platform_type = chip_type;
  param.max_frame_depth = requested_depth;

  void* raw_handle = nullptr;
  int ret = ops.Create(&raw_handle, &param);
  if (ret != 0 || !raw_handle) {
    std::cerr << "[OcrDocQaDemo ERROR] Failed ops.Create: "
              << llm_edgeflow::platform::GetPlatformLastError() << std::endl;
    return 5;
  }

  OperatorHandleGuard guard(ops, raw_handle);

  CompanyString img_str{static_cast<int32_t>(img.size()),
                        const_cast<char*>(img.data())};
  CompanyFrame frame{60001, &img_str, nullptr};
  CompanyString prompt_str{static_cast<int32_t>(prompt.size()),
                           const_cast<char*>(prompt.data())};

  llm_edgeflow::platform::NamedIoBatch in_batch(1);
  llm_edgeflow::platform::NamedIoBatch out_batch(1);

  in_batch[0]["camera_0.frame"] =
      llm_edgeflow::platform::MakeBorrowedPlatformInput(&frame);
  in_batch[0]["camera_0.string"] =
      llm_edgeflow::platform::MakeBorrowedPlatformInput(&prompt_str);
  out_batch[0]["camera_0.od_out"] = std::shared_ptr<void>();

  auto start_time = std::chrono::high_resolution_clock::now();
  ret = ops.Process(raw_handle, in_batch, out_batch);
  auto end_time = std::chrono::high_resolution_clock::now();

  double latency_ms =
      std::chrono::duration<double, std::milli>(end_time - start_time).count();

  if (ret != 0) {
    std::cerr << "[OcrDocQaDemo ERROR] ops.Process failed: "
              << llm_edgeflow::platform::GetPlatformLastError() << std::endl;
    return 5;
  }

  const auto* out_ptr = static_cast<const CompanyOdOutput*>(
      out_batch[0]["camera_0.od_out"].get());
  if (!out_ptr) {
    std::cerr << "[OcrDocQaDemo ERROR] Null output pointer received."
              << std::endl;
    return 5;
  }

  uint64_t req_id = out_ptr->request_id;
  int32_t box_count = out_ptr->detected_box_count;
  std::string result_json = (out_ptr->result_json && out_ptr->result_json->data)
                                ? std::string(out_ptr->result_json->data,
                                              out_ptr->result_json->length)
                                : "";

  out_batch.clear();  // 释放输出块，触发 weak Deleter 回池

  std::cout << "\n>>> 业务 5 执行结果验证 <<<" << std::endl;
  PrintDivider();
  std::cout << "  Request ID     : " << req_id << "\n"
            << "  OCR Box Count  : " << box_count << "\n"
            << "  Extracted JSON : " << result_json << std::endl;

  std::vector<DemoSampleResult> sample_results;
  DemoSampleResult sample;
  sample.request_id = req_id;
  sample.status = 0;
  sample.latency_ms = latency_ms;
  sample.output["detected_box_count"] = box_count;
  if (!result_json.empty()) {
    auto parsed = nlohmann::json::parse(result_json, nullptr, false);
    if (parsed.is_discarded()) {
      sample.output["extracted_invoice_raw"] = result_json;
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

REGISTER_DEMO_BIZ("ocr_doc_qa", "【业务 5 演示】智能多模态图文票据问答",
                  RunOcrDocQaDemo);

}  // namespace alg_demo
