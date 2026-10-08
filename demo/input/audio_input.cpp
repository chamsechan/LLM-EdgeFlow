#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"

namespace alg_demo {
namespace {

// 数据集：.jsonl 清单 + float32 PCM 文件；测试可用内置回退样本。
// "是否真实模型"只看 suite == "real"：真实模型不允许静默使用回退音频。
struct AudioStorage {
  std::vector<AudioDatasetSample> samples;
  std::vector<float> fallback_pcm;
  std::vector<CompanyOperatorAudioInput> inputs;
};

int BuildAudioRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  const bool is_real = options.suite == "real";
  auto storage = std::make_shared<AudioStorage>();

  bool loaded_from_dataset = false;
  if (!options.dataset_path.empty()) {
    const std::string resolved = ResolvePath(options.dataset_path);
    if (!std::filesystem::exists(resolved)) {
      if (!options.allow_fallback_sample) {
        std::cerr << "[AudioInput ERROR] Dataset file not found: "
                  << options.dataset_path << std::endl;
        return 4;
      }
    } else if (resolved.rfind(".jsonl") != std::string::npos) {
      std::string read_err;
      if (!ReadAudioDataset(options.dataset_path, &storage->samples,
                            &read_err)) {
        if (!options.allow_fallback_sample) {
          std::cerr << "[AudioInput ERROR] Failed to read audio dataset: "
                    << read_err << std::endl;
          return 4;
        }
      } else {
        loaded_from_dataset = true;
      }
    } else if (is_real && !options.allow_fallback_sample) {
      std::cerr << "[AudioInput ERROR] Real audio ASR profile requires a "
                   ".jsonl dataset, got: "
                << options.dataset_path << std::endl;
      return 4;
    }
  }

  if (is_real && !loaded_from_dataset && !options.allow_fallback_sample) {
    std::cerr << "[AudioInput ERROR] Real audio ASR profile requires a valid "
                 "audio dataset and fallback sample is disabled."
              << std::endl;
    return 4;
  }

  const int32_t service_type = DemoServiceType(inputs[0]);
  out->request_info.clear();
  if (loaded_from_dataset) {
    storage->inputs.reserve(storage->samples.size());
    for (const auto& sample : storage->samples) {
      storage->inputs.push_back({sample.request_id, sample.pcm_data.data(),
                                 static_cast<int32_t>(sample.pcm_data.size()),
                                 sample.sample_rate, service_type});
      nlohmann::json info = nlohmann::json::object();
      if (!sample.reference_text.empty()) {
        info["reference_text"] = sample.reference_text;
        info["expected_category"] = sample.expected_category;
      }
      out->request_info.push_back(std::move(info));
    }
  } else {
    storage->fallback_pcm.assign(16000, 0.01f);
    storage->inputs.push_back(
        {70001, storage->fallback_pcm.data(),
         static_cast<int32_t>(storage->fallback_pcm.size()), 16000,
         service_type});
    out->request_info.push_back(nlohmann::json::object());
  }

  const std::string key = DemoIoKey(inputs[0]);
  out->requests.assign(storage->inputs.size(), {});
  for (size_t i = 0; i < storage->inputs.size(); ++i) {
    out->requests[i][key] =
        llm_edgeflow::operator_api::MakeBorrowedOperatorInput(
            &storage->inputs[i]);
  }
  out->storage = std::move(storage);
  return 0;
}

REGISTER_DEMO_INPUT("CompanyOperatorAudioInput", BuildAudioRequests);

}  // namespace
}  // namespace alg_demo
