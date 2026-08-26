#include <iostream>
#include <vector>

#include "company_alg_interface.h"
#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"

namespace alg_demo {

int RunAudioAsrDemo(const DemoOptions& options) {
  PrintBanner("【业务 6 演示】语音识别与意图槽位抽取",
              "Conf: " + options.config_path);

  // 校验或加载测试集文件
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

  // 现阶段构建受控 16kHz PCM 单声道音频帧测试向量
  std::vector<float> pcm(16000, 0.01f);
  std::vector<CompanyAudioInputStruct> inputs = {
      {70001, pcm.data(), static_cast<int>(pcm.size()), 16000}};
  std::vector<CompanyAudioOutputStruct> outputs;
  std::vector<double> latencies;

  int ret =
      RunPlatformOperator<CompanyAudioInputStruct, CompanyAudioOutputStruct>(
          options, "mic_0.audio_in", "mic_0.audio_out", inputs, &outputs,
          &latencies);
  if (ret != 0) {
    return ret;
  }

  std::cout << "\n>>> 业务 6 执行结果验证 <<<" << std::endl;
  PrintDivider();
  const char* asr_txt =
      (outputs[0].transcribed_text && outputs[0].transcribed_text->data)
          ? outputs[0].transcribed_text->data
          : "";
  const char* slot_json =
      (outputs[0].intent_slot_json && outputs[0].intent_slot_json->data)
          ? outputs[0].intent_slot_json->data
          : "";
  std::cout << "  Request ID     : " << outputs[0].request_id << "\n"
            << "  ASR Text       : " << asr_txt << "\n"
            << "  Intent / Slots : " << slot_json << std::endl;

  std::vector<DemoSampleResult> sample_results;
  DemoSampleResult sample;
  sample.request_id = outputs[0].request_id;
  sample.status = 0;
  sample.latency_ms = latencies.empty() ? 0.0 : latencies[0];
  sample.output["transcribed_text"] = asr_txt;
  if (slot_json[0] != '\0') {
    auto parsed = nlohmann::json::parse(slot_json, nullptr, false);
    if (parsed.is_discarded()) {
      sample.output["intent_slot_raw"] = slot_json;
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

REGISTER_DEMO_BUSINESS("audio_asr", "【业务 6 演示】语音识别与意图槽位抽取",
                       RunAudioAsrDemo);

}  // namespace alg_demo
