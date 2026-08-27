#include <iostream>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"
#include "platform/company_platform_types.h"

namespace alg_demo {

int RunAudioAsrDemo(const DemoOptions& options) {
  PrintBanner("【业务 6 演示】语音识别与意图槽位抽取",
              "Conf: " + options.config_path);

  std::string err;
  if (!options.dataset_path.empty()) {
    std::string resolved = ResolvePath(options.dataset_path);
    if (!std::filesystem::exists(resolved)) {
      if (!options.allow_fallback_sample) {
        std::cerr << "[AudioAsrDemo ERROR] Dataset file not found: "
                  << options.dataset_path << std::endl;
        return 4;
      }
    }
  }

  std::vector<float> pcm(16000, 0.01f);
  std::vector<CompanyPlatformAudioInput> inputs = {
      {70001, pcm.data(), static_cast<int32_t>(pcm.size()), 16000}};

  struct OutputSummary {
    uint64_t request_id = 0;
    std::string transcribed_text;
    std::string intent_slot_json;
  };
  std::vector<OutputSummary> output_summaries(1);
  std::vector<double> latencies;

  int ret = RunPlatformOperatorWithExtractor<CompanyPlatformAudioInput,
                                             CompanyPlatformAudioOutput>(
      options, "mic_0.audio_in", "mic_0.audio_out", inputs,
      [&](size_t idx, const CompanyPlatformAudioOutput& out) {
        output_summaries[idx].request_id = out.request_id;
        if (out.transcribed_text && out.transcribed_text->data) {
          output_summaries[idx].transcribed_text.assign(
              out.transcribed_text->data, out.transcribed_text->length);
        }
        if (out.intent_slot_json && out.intent_slot_json->data) {
          output_summaries[idx].intent_slot_json.assign(
              out.intent_slot_json->data, out.intent_slot_json->length);
        }
      },
      &latencies);
  if (ret != 0) {
    return ret;
  }

  std::cout << "\n>>> 业务 6 执行结果验证 <<<" << std::endl;
  PrintDivider();
  std::cout << "  Request ID     : " << output_summaries[0].request_id << "\n"
            << "  ASR Text       : " << output_summaries[0].transcribed_text
            << "\n"
            << "  Intent / Slots : " << output_summaries[0].intent_slot_json
            << std::endl;

  std::vector<DemoSampleResult> sample_results;
  DemoSampleResult sample;
  sample.request_id = output_summaries[0].request_id;
  sample.status = 0;
  sample.latency_ms = latencies.empty() ? 0.0 : latencies[0];
  sample.output["transcribed_text"] = output_summaries[0].transcribed_text;
  if (!output_summaries[0].intent_slot_json.empty()) {
    auto parsed = nlohmann::json::parse(output_summaries[0].intent_slot_json,
                                        nullptr, false);
    if (parsed.is_discarded()) {
      sample.output["intent_slot_raw"] = output_summaries[0].intent_slot_json;
    } else {
      sample.output["intent_slot"] = parsed;
    }
  }
  sample_results.push_back(sample);

  ResultWriter writer(options);
  int w_ret = writer.WriteResults(sample_results, 0.0, &err);
  if (w_ret != 0) {
    std::cerr << "[AudioAsrDemo ERROR] Failed to write results: " << err
              << std::endl;
    return w_ret;
  }

  std::cout << "[AudioAsrDemo] Completed successfully." << std::endl;
  return 0;
}

REGISTER_DEMO_BIZ("audio_asr", "【业务 6 演示】语音识别与意图槽位抽取",
                  RunAudioAsrDemo);

}  // namespace alg_demo
