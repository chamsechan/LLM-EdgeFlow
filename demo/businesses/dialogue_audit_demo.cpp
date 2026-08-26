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

int RunDialogueAuditDemo(const DemoOptions& options) {
  PrintBanner("【业务 4 演示】智能对话风控质检业务",
              "Conf: " + options.config_path);

  std::unordered_map<std::string, std::vector<std::string>> sections;
  std::string err;
  if (!ParseTagSections(options.dataset_path, &sections, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[DialogueAuditDemo ERROR] " << err << std::endl;
      return 4;
    }
  }

  auto channels = sections["CHANNEL"];
  auto dialogues = sections["DIALOGUE"];

  if (channels.empty() || dialogues.empty()) {
    if (options.allow_fallback_sample) {
      std::cout << "[DialogueAuditDemo WARN] Dataset sections missing, using "
                   "fallback sample."
                << std::endl;
      channels = {"VIP专席客服", "在线售后IM"};
      dialogues = {
          "亲，平台退款审核太慢了，你加我私人微信转账给我吧，我私下把商品寄给你"
          "，"
          "还能返现20元！",
          "您好，您的商品符合7天无理由退货政策，已为您在系统提交退款换货流程，"
          "请保持手机畅通。"};
    } else {
      std::cerr << "[DialogueAuditDemo ERROR] Dataset missing [CHANNEL] or "
                   "[DIALOGUE] sections."
                << std::endl;
      return 4;
    }
  }

  size_t count = std::min(channels.size(), dialogues.size());
  std::vector<CompanyString> channel_strings(count);
  std::vector<CompanyString> dialogue_strings(count);
  std::vector<CompanyAuditInputStruct> inputs;
  inputs.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    CompanyString_FromCString(&channel_strings[i], channels[i].c_str());
    CompanyString_FromCString(&dialogue_strings[i], dialogues[i].c_str());
    inputs.push_back({static_cast<uint64_t>(40001 + i), &dialogue_strings[i],
                      &channel_strings[i]});
  }

  std::vector<CompanyAuditOutputStruct> outputs;
  std::vector<double> latencies;

  int ret =
      RunPlatformOperator<CompanyAuditInputStruct, CompanyAuditOutputStruct>(
          options, "audit_channel.audit_in", "audit_channel.audit_out", inputs,
          &outputs, &latencies);
  if (ret != 0) {
    return ret;
  }

  std::cout << "\n>>> 业务 4 执行结果验证 (多模型协同质检) <<<" << std::endl;
  std::vector<DemoSampleResult> sample_results;
  sample_results.reserve(outputs.size());

  for (size_t i = 0; i < outputs.size(); ++i) {
    PrintDivider();
    const char* r_level = (outputs[i].risk_level && outputs[i].risk_level->data)
                              ? outputs[i].risk_level->data
                              : "";
    const char* m_policy = (outputs[i].matched_policy_clause &&
                            outputs[i].matched_policy_clause->data)
                               ? outputs[i].matched_policy_clause->data
                               : "";
    const char* verdict_json =
        (outputs[i].audit_verdict_json && outputs[i].audit_verdict_json->data)
            ? outputs[i].audit_verdict_json->data
            : "";
    std::cout << "  Audit #" << i << " | Request ID: " << outputs[i].request_id
              << "\n"
              << "  Channel       : " << channels[i] << "\n"
              << "  Dialogue Text : \"" << dialogues[i] << "\"\n"
              << "  Risk Level    : " << r_level << " (Score: " << std::fixed
              << std::setprecision(2) << outputs[i].risk_score << ")\n"
              << "  Matched Policy: "
              << (m_policy[0] != '\0' ? m_policy : "none") << "\n"
              << "  Audit Verdict : " << verdict_json << std::endl;

    DemoSampleResult sample;
    sample.request_id = outputs[i].request_id;
    sample.status = 0;
    sample.latency_ms = (i < latencies.size()) ? latencies[i] : 0.0;
    sample.output["channel"] = channels[i];
    sample.output["risk_level"] = r_level;
    sample.output["risk_score"] = outputs[i].risk_score;
    sample.output["matched_policy"] = m_policy;
    if (verdict_json[0] != '\0') {
      auto parsed = nlohmann::json::parse(verdict_json, nullptr, false);
      if (parsed.is_discarded()) {
        sample.output["audit_verdict_raw"] = verdict_json;
      } else {
        sample.output["audit_verdict"] = parsed;
      }
    }
    sample_results.push_back(sample);
  }

  ResultWriter writer(options);
  int w_ret = writer.WriteResults(sample_results, 0.0, &err);
  if (w_ret != 0) {
    std::cerr << "[DialogueAuditDemo ERROR] Failed to write results: " << err
              << std::endl;
    return w_ret;
  }

  std::cout << "[DialogueAuditDemo] Completed successfully." << std::endl;
  return 0;
}

REGISTER_DEMO_BUSINESS("dialogue_audit", "【业务 4 演示】智能对话风控质检业务",
                       RunDialogueAuditDemo);

}  // namespace alg_demo
