#include <iostream>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"
#include "operator/company_operator_types.h"

namespace alg_demo {

int RunAudioAsrDemo(const DemoOptions& options) {
  PrintBanner("【业务 6 演示】语音识别与意图槽位抽取",
              "Conf: " + options.config_path);

  std::string err;
  std::vector<AudioDatasetSample> dataset_samples;
  std::vector<CompanyOperatorAudioInput> inputs;
  std::vector<std::vector<float>> fallback_buffers;

  const bool is_real_profile =
      (options.suite == "real" ||
       options.profile.find("whisper") != std::string::npos ||
       options.config_path.find("whisper") != std::string::npos);

  bool loaded_from_dataset = false;
  if (!options.dataset_path.empty()) {
    std::string resolved = ResolvePath(options.dataset_path);
    if (!std::filesystem::exists(resolved)) {
      if (!options.allow_fallback_sample) {
        std::cerr << "[AudioAsrDemo ERROR] Dataset file not found: "
                  << options.dataset_path << std::endl;
        return 4;
      }
    } else if (resolved.rfind(".jsonl") != std::string::npos) {
      std::string read_err;
      if (!ReadAudioDataset(options.dataset_path, &dataset_samples,
                            &read_err)) {
        if (!options.allow_fallback_sample) {
          std::cerr << "[AudioAsrDemo ERROR] Failed to read audio dataset: "
                    << read_err << std::endl;
          return 4;
        }
      } else {
        loaded_from_dataset = true;
      }
    } else {
      if (is_real_profile && !options.allow_fallback_sample) {
        std::cerr << "[AudioAsrDemo ERROR] Real audio ASR profile requires a "
                     ".jsonl dataset, got: "
                  << options.dataset_path << std::endl;
        return 4;
      }
    }
  }

  if (is_real_profile && !loaded_from_dataset &&
      !options.allow_fallback_sample) {
    std::cerr << "[AudioAsrDemo ERROR] Real audio ASR profile requires a valid "
                 "audio dataset and fallback sample is disabled."
              << std::endl;
    return 4;
  }

  if (loaded_from_dataset) {
    inputs.reserve(dataset_samples.size());
    for (const auto& sample : dataset_samples) {
      inputs.push_back({sample.request_id, sample.pcm_data.data(),
                        static_cast<int32_t>(sample.pcm_data.size()),
                        sample.sample_rate});
    }
  } else {
    if (is_real_profile && !options.allow_fallback_sample) {
      std::cerr << "[AudioAsrDemo ERROR] Real audio ASR profile cannot use "
                   "fallback fixed audio."
                << std::endl;
      return 4;
    }
    fallback_buffers.emplace_back(16000, 0.01f);
    inputs.push_back({70001, fallback_buffers[0].data(),
                      static_cast<int32_t>(fallback_buffers[0].size()), 16000});
  }

  struct OutputSummary {
    uint64_t request_id = 0;
    std::string transcribed_text;
    std::string intent_slot_json;
  };
  std::vector<OutputSummary> output_summaries(inputs.size());
  std::vector<double> latencies;

  int ret = RunOperatorWithExtractor<CompanyOperatorAudioInput,
                                     CompanyOperatorAudioOutput>(
      options, "mic_0.audio_in", "mic_0.audio_out", inputs,
      [&](size_t idx, const CompanyOperatorAudioOutput& out) {
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
  std::vector<DemoSampleResult> sample_results;
  sample_results.reserve(inputs.size());
  for (size_t i = 0; i < inputs.size(); ++i) {
    std::cout << "  Sample #" << i << "\n"
              << "  Request ID     : " << output_summaries[i].request_id << "\n"
              << "  ASR Text       : " << output_summaries[i].transcribed_text
              << "\n"
              << "  Intent / Slots : " << output_summaries[i].intent_slot_json
              << std::endl;
    if (loaded_from_dataset && !dataset_samples[i].reference_text.empty()) {
      std::cout << "  Reference Text : " << dataset_samples[i].reference_text
                << "\n"
                << "  Expected Cat   : " << dataset_samples[i].expected_category
                << std::endl;
    }
    PrintDivider();

    DemoSampleResult sample;
    sample.request_id = output_summaries[i].request_id;
    sample.status = 0;
    sample.latency_ms = i < latencies.size() ? latencies[i] : 0.0;
    sample.output["transcribed_text"] = output_summaries[i].transcribed_text;
    if (!output_summaries[i].intent_slot_json.empty()) {
      auto parsed = nlohmann::json::parse(output_summaries[i].intent_slot_json,
                                          nullptr, false);
      if (parsed.is_discarded()) {
        sample.output["intent_slot_raw"] = output_summaries[i].intent_slot_json;
      } else {
        sample.output["intent_slot"] = parsed;
      }
    }
    sample_results.push_back(std::move(sample));
  }

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
