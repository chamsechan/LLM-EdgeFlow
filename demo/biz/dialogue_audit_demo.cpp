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
  std::vector<CompanyString> channel_strs;
  std::vector<CompanyString> dialogue_strs;
  channel_strs.reserve(count);
  dialogue_strs.reserve(count);
  std::vector<CompanyPlatformAuditInput> inputs;
  inputs.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    channel_strs.push_back({static_cast<int32_t>(channels[i].size()),
                            const_cast<char*>(channels[i].data())});
    dialogue_strs.push_back({static_cast<int32_t>(dialogues[i].size()),
                             const_cast<char*>(dialogues[i].data())});
    inputs.push_back({static_cast<uint64_t>(40001 + i), &dialogue_strs.back(),
                      &channel_strs.back()});
  }

  struct OutputSummary {
    uint64_t request_id = 0;
    std::string risk_level;
    float risk_score = 0.0f;
    std::string matched_policy_clause;
    std::string audit_verdict_json;
  };
  std::vector<OutputSummary> output_summaries(count);
  std::vector<double> latencies;

  int ret = RunPlatformOperatorWithExtractor<CompanyPlatformAuditInput,
                                             CompanyPlatformAuditOutput>(
      options, "audit_channel.audit_in", "audit_channel.audit_out", inputs,
      [&](size_t idx, const CompanyPlatformAuditOutput& out) {
        output_summaries[idx].request_id = out.request_id;
        output_summaries[idx].risk_score = out.risk_score;
        if (out.risk_level && out.risk_level->data) {
          output_summaries[idx].risk_level.assign(out.risk_level->data,
                                                  out.risk_level->length);
        }
        if (out.matched_policy_clause && out.matched_policy_clause->data) {
          output_summaries[idx].matched_policy_clause.assign(
              out.matched_policy_clause->data,
              out.matched_policy_clause->length);
        }
        if (out.audit_verdict_json && out.audit_verdict_json->data) {
          output_summaries[idx].audit_verdict_json.assign(
              out.audit_verdict_json->data, out.audit_verdict_json->length);
        }
      },
      &latencies);
  if (ret != 0) {
    return ret;
  }

  std::cout << "\n>>> 业务 4 执行结果验证 (多模型协同质检) <<<" << std::endl;
  std::vector<DemoSampleResult> sample_results;
  sample_results.reserve(output_summaries.size());

  for (size_t i = 0; i < output_summaries.size(); ++i) {
    PrintDivider();
    std::cout << "  Audit #" << i
              << " | Request ID: " << output_summaries[i].request_id << "\n"
              << "  Channel       : " << channels[i] << "\n"
              << "  Dialogue Text : \"" << dialogues[i] << "\"\n"
              << "  Risk Level    : " << output_summaries[i].risk_level
              << " (Score: " << std::fixed << std::setprecision(2)
              << output_summaries[i].risk_score << ")\n"
              << "  Matched Policy: "
              << (!output_summaries[i].matched_policy_clause.empty()
                      ? output_summaries[i].matched_policy_clause
                      : "none")
              << "\n"
              << "  Audit Verdict : " << output_summaries[i].audit_verdict_json
              << std::endl;

    DemoSampleResult sample;
    sample.request_id = output_summaries[i].request_id;
    sample.status = 0;
    sample.latency_ms = (i < latencies.size()) ? latencies[i] : 0.0;
    sample.output["channel"] = channels[i];
    sample.output["risk_level"] = output_summaries[i].risk_level;
    sample.output["risk_score"] = output_summaries[i].risk_score;
    sample.output["matched_policy"] = output_summaries[i].matched_policy_clause;
    if (!output_summaries[i].audit_verdict_json.empty()) {
      auto parsed = nlohmann::json::parse(
          output_summaries[i].audit_verdict_json, nullptr, false);
      if (parsed.is_discarded()) {
        sample.output["audit_verdict_raw"] =
            output_summaries[i].audit_verdict_json;
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

REGISTER_DEMO_BIZ("dialogue_audit", "【业务 4 演示】智能对话风控质检业务",
                  RunDialogueAuditDemo);

}  // namespace alg_demo
